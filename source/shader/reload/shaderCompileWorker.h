#pragma once

// File responsibility: Runs CPU shader candidate compilation on a dedicated
// worker thread. It never touches Vulkan objects, live Material/PipelineFactory
// caches, or the disk manifest; it only publishes an immutable candidate batch
// for EngineLoop to validate and commit at a render-thread safe point.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "shader/reload/shaderReloadCoordinator.h"

class ShaderCompiler;

namespace VL
{

struct ShaderCompileWorkerResult
{
    uint64_t generation = 0;
    uint64_t sourceEpoch = 0;
    std::vector<std::string> changedSources;
    bool succeeded = false;
    ShaderReloadCandidateBatch batch;
    std::string errorMessage;
    double elapsedMilliseconds = 0.0;
    uint64_t shadercInvocations = 0;
};

struct ShaderCompileWorkerShutdownDiagnostics
{
    bool joined = false;
    uint64_t postCompileGateGeneration = 0;
    uint64_t discardedGeneration = 0;
    bool discardedCompletedCandidate = false;
    uint64_t completedResultPublishedGeneration = 0;
};

class ShaderCompileWorker
{
public:
    ShaderCompileWorker() = default;
    ~ShaderCompileWorker();

    ShaderCompileWorker(const ShaderCompileWorker&) = delete;
    ShaderCompileWorker& operator=(const ShaderCompileWorker&) = delete;

    void Start(ShaderCompiler& shaderCompiler);

    // Submits one plan. Returns false when the worker is still compiling a
    // previous plan, still holds an untaken result, or has been shut down.
    bool Submit(ShaderReloadPlan plan);

    bool HasCompletedResult() const;
    ShaderCompileWorkerResult TakeCompletedResult();

    bool IsIdle() const;
    bool IsRunning() const;
    uint64_t GetInFlightGeneration() const;

    // Runtime-test gate for deterministic in-flight source mutation coverage.
    // Production code leaves the gate disabled.
    void EnableTestCompileGate();
    void ArmTestCompileGateForNextSubmit();
    bool IsWaitingAtTestCompileGate() const;
    void ReleaseTestCompileGate();
    void DisableTestCompileGate();

    // Runtime-test gate after a complete candidate batch has been built but
    // before it can be published as completedResult. Shutdown is the normal
    // cancellation path for this gate.
    void ArmTestPostCompileGateForNextSubmit();
    bool IsWaitingAtTestPostCompileGate() const;

    // Requests a stop, wakes the thread, and joins. Safe to call more than
    // once. Any untaken result is discarded.
    void Shutdown();
    ShaderCompileWorkerShutdownDiagnostics
    GetShutdownDiagnostics() const;

private:
    void ThreadMain();

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread thread;
    ShaderCompiler* shaderCompiler = nullptr;
    bool running = false;
    bool stopRequested = false;
    bool planSubmitted = false;
    bool compiling = false;
    bool testCompileGateEnabled = false;
    bool testCompileGateNextSubmit = false;
    bool waitingAtTestCompileGate = false;
    bool testCompileGateReleased = false;
    bool testPostCompileGateNextSubmit = false;
    bool waitingAtTestPostCompileGate = false;
    uint64_t inFlightGeneration = 0;
    std::optional<ShaderReloadPlan> submittedPlan;
    std::optional<ShaderCompileWorkerResult> completedResult;
    ShaderCompileWorkerShutdownDiagnostics shutdownDiagnostics;
};

} // namespace VL
