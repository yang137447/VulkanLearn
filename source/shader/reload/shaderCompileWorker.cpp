#include "shader/reload/shaderCompileWorker.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace VL
{

ShaderCompileWorker::~ShaderCompileWorker()
{
    Shutdown();
}

void ShaderCompileWorker::Start(ShaderCompiler& compiler)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (running)
    {
        return;
    }
    shaderCompiler = &compiler;
    stopRequested = false;
    planSubmitted = false;
    submittedPlan.reset();
    completedResult.reset();
    testPostCompileGateNextSubmit = false;
    waitingAtTestPostCompileGate = false;
    shutdownDiagnostics = {};
    running = true;
    thread = std::thread([this]() { ThreadMain(); });
}

bool ShaderCompileWorker::Submit(ShaderReloadPlan plan)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!running || stopRequested || compiling || planSubmitted ||
        completedResult.has_value())
    {
        return false;
    }
    submittedPlan = std::move(plan);
    planSubmitted = true;
    condition.notify_one();
    return true;
}

bool ShaderCompileWorker::HasCompletedResult() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return completedResult.has_value();
}

ShaderCompileWorkerResult
ShaderCompileWorker::TakeCompletedResult()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!completedResult.has_value())
    {
        throw std::runtime_error(
            "Shader compile worker has no completed result to take");
    }
    ShaderCompileWorkerResult result =
        std::move(*completedResult);
    completedResult.reset();
    return result;
}

bool ShaderCompileWorker::IsIdle() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return !compiling && !planSubmitted && !completedResult.has_value();
}

bool ShaderCompileWorker::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return running;
}

uint64_t ShaderCompileWorker::GetInFlightGeneration() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (completedResult.has_value())
    {
        return completedResult->generation;
    }
    return inFlightGeneration;
}

void ShaderCompileWorker::EnableTestCompileGate()
{
    std::lock_guard<std::mutex> lock(mutex);
    testCompileGateEnabled = true;
    testCompileGateNextSubmit = false;
    waitingAtTestCompileGate = false;
    testCompileGateReleased = false;
}

bool ShaderCompileWorker::IsWaitingAtTestCompileGate() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return waitingAtTestCompileGate;
}

void ShaderCompileWorker::ArmTestCompileGateForNextSubmit()
{
    std::lock_guard<std::mutex> lock(mutex);
    testCompileGateNextSubmit = true;
}

void ShaderCompileWorker::ReleaseTestCompileGate()
{
    std::lock_guard<std::mutex> lock(mutex);
    testCompileGateReleased = true;
    condition.notify_all();
}

void ShaderCompileWorker::DisableTestCompileGate()
{
    std::lock_guard<std::mutex> lock(mutex);
    testCompileGateEnabled = false;
    testCompileGateNextSubmit = false;
    testCompileGateReleased = true;
    condition.notify_all();
}

void ShaderCompileWorker::ArmTestPostCompileGateForNextSubmit()
{
    std::lock_guard<std::mutex> lock(mutex);
    shutdownDiagnostics = {};
    testPostCompileGateNextSubmit = true;
}

bool ShaderCompileWorker::IsWaitingAtTestPostCompileGate() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return waitingAtTestPostCompileGate;
}

void ShaderCompileWorker::Shutdown()
{
    std::thread joinableThread;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running)
        {
            return;
        }
        stopRequested = true;
        condition.notify_all();
        if (thread.joinable())
        {
            joinableThread = std::move(thread);
        }
    }
    if (joinableThread.joinable())
    {
        joinableThread.join();
    }
    std::lock_guard<std::mutex> lock(mutex);
    shutdownDiagnostics.joined = true;
    running = false;
    compiling = false;
    testCompileGateEnabled = false;
    testCompileGateNextSubmit = false;
    waitingAtTestCompileGate = false;
    testCompileGateReleased = false;
    testPostCompileGateNextSubmit = false;
    waitingAtTestPostCompileGate = false;
    inFlightGeneration = 0;
    planSubmitted = false;
    submittedPlan.reset();
    completedResult.reset();
    shaderCompiler = nullptr;
}

ShaderCompileWorkerShutdownDiagnostics
ShaderCompileWorker::GetShutdownDiagnostics() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return shutdownDiagnostics;
}

void ShaderCompileWorker::ThreadMain()
{
    for (;;)
    {
        ShaderReloadPlan plan;
        bool waitAtPostCompileGate = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this]()
            {
                return stopRequested || planSubmitted;
            });
            if (stopRequested)
            {
                return;
            }
            plan = std::move(*submittedPlan);
            submittedPlan.reset();
            planSubmitted = false;
            compiling = true;
            inFlightGeneration = plan.generation;
            waitAtPostCompileGate =
                testPostCompileGateNextSubmit;
            testPostCompileGateNextSubmit = false;
            if (testCompileGateEnabled || testCompileGateNextSubmit)
            {
                testCompileGateNextSubmit = false;
                waitingAtTestCompileGate = true;
                condition.notify_all();
                condition.wait(lock, [this]()
                {
                    return stopRequested ||
                        testCompileGateReleased;
                });
                waitingAtTestCompileGate = false;
                testCompileGateEnabled = false;
                testCompileGateReleased = false;
                if (stopRequested)
                {
                    compiling = false;
                    inFlightGeneration = 0;
                    return;
                }
            }
        }

        ShaderCompileWorkerResult result;
        result.generation = plan.generation;
        result.sourceEpoch = plan.sourceEpoch;
        result.changedSources = plan.changedSources;
        const auto startTime = std::chrono::steady_clock::now();
        try
        {
            result.batch = CompileGraphicsCandidates(
                *shaderCompiler,
                std::move(plan));
            result.succeeded = true;
            result.shadercInvocations =
                result.batch.shadercInvocations;
        }
        catch (const std::exception& exception)
        {
            result.errorMessage = exception.what();
        }
        result.elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startTime)
                .count();

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (waitAtPostCompileGate)
            {
                waitingAtTestPostCompileGate = true;
                shutdownDiagnostics.postCompileGateGeneration =
                    result.generation;
                condition.notify_all();
                condition.wait(lock, [this]()
                {
                    return stopRequested;
                });
                waitingAtTestPostCompileGate = false;
            }
            if (stopRequested)
            {
                shutdownDiagnostics.discardedGeneration =
                    result.generation;
                shutdownDiagnostics.discardedCompletedCandidate =
                    result.succeeded;
                compiling = false;
                inFlightGeneration = 0;
                return;
            }
            // A newer plan can only be submitted after this result is taken,
            // so overwriting a stale result here is never observed.
            compiling = false;
            completedResult = std::move(result);
            shutdownDiagnostics.completedResultPublishedGeneration =
                completedResult->generation;
        }
    }
}

} // namespace VL
