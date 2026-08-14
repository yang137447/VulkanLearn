#include "engine/runtimeTestHooks.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "engine/diagnosticsSubsystem.h"
#include "engine/testing/runtimeTestFixtures.h"
#include "engine/testing/runtimeValidationServices.h"
#include "render/resource/resourceRetireQueue.h"
#include "world/world.h"
#include "world/worldManager.h"

namespace VL
{
using namespace RuntimeTestFixtures;
bool RuntimeTestHooks::BeginResizeStress(
    int resizeCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (resizeCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("Resize stress ignored because resize count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    resizeStressTotal = resizeCount;
    resizeStressRemaining = resizeCount;
    resizeStressCompletedCount = 0;
    resizeStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Resize stress started: count=" + std::to_string(resizeStressTotal));
    return true;
}

bool RuntimeTestHooks::BeginRenderGraphReloadStress(
    int reloadCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (reloadCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning(
            "Render graph reload stress ignored because reload count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    graphReloadStressTotal = reloadCount;
    graphReloadStressRemaining = reloadCount;
    graphReloadStressCompletedCount = 0;
    graphReloadStressWaitingForDrain = false;
    graphReloadRetireDrainFramesRemaining = 0;
    graphReloadMaxPendingRetiredResources = 0;
    graphReloadStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Render graph reload stress started: count=" +
        std::to_string(graphReloadStressTotal));
    return true;
}

bool RuntimeTestHooks::BeginFrameSmokeTest(
    int frameCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (frameCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("Frame smoke test ignored because frame count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    frameSmokeTotal = frameCount;
    frameSmokeCompletedCount = 0;
    frameSmokeTotalMs = 0.0;
    frameSmokeMaxMs = 0.0;
    frameSmokeMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalFrameCount = 0;
    frameSmokeIntervalTotalMs = 0.0;
    frameSmokeIntervalMaxMs = 0.0;
    frameSmokeIntervalMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalRenderLoopTotalMs = 0.0;
    frameSmokeIntervalRenderLoopMaxMs = 0.0;
    frameSmokeActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Frame smoke test started: frames=" + std::to_string(frameSmokeTotal));
    return true;
}

bool RuntimeTestHooks::BeginEnvironmentUpdateStress(
    int updateCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (updateCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning(
            "Environment update stress ignored because update count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    environmentUpdateStressTotal = updateCount;
    environmentUpdateStressCompletedCount = 0;
    environmentUpdateStressFrameBudget = std::max(300, (updateCount + 2) * 120);
    environmentUpdateStressPreviousActiveGeneration = 0;
    environmentUpdateStressObservedPreviousResources = false;
    waitingForProceduralSkyParametersResult = false;
    environmentUpdateStressBaselineTimingSamples.fill(0);
    environmentUpdateStressPrefilterMipCount = 0;
    environmentUpdateStressEnvironmentCubeIdentity = 0;
    environmentUpdateStressPrefilterCubeIdentity = 0;
    environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitInitialGeneration;
    environmentUpdateStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Environment update stress started: changes=" +
        std::to_string(environmentUpdateStressTotal));
    return true;
}

void RuntimeTestHooks::UpdateResizeStress(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!resizeStressActive || resizeStressRemaining <= 0)
    {
        return;
    }

    const std::array<uint32_t, 2> configuredWindowSize =
        validationServices.GetConfiguredWindowSize();
    const int baseWidth =
        static_cast<int>(configuredWindowSize[0]);
    const int baseHeight =
        static_cast<int>(configuredWindowSize[1]);
    const bool useSmallerSize = (resizeStressCompletedCount % 2) == 0;
    const int targetWidth = useSmallerSize ? std::max(320, baseWidth - 160) : baseWidth;
    const int targetHeight = useSmallerSize ? std::max(240, baseHeight - 90) : baseHeight;

    suppressNextResizeEvent = true;
    suppressedResizeWidth = static_cast<uint32_t>(targetWidth);
    suppressedResizeHeight = static_cast<uint32_t>(targetHeight);

    auto resizeResult = validationServices.ResizeWindowAndRenderer(
        static_cast<uint32_t>(targetWidth),
        static_cast<uint32_t>(targetHeight));
    if (resizeResult.IsFailure())
    {
        resizeStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportRuntimeError("Resize stress failed", resizeResult.Error());
        return;
    }

    ++resizeStressCompletedCount;
    --resizeStressRemaining;
    diagnostics.ReportInfo(
        "Resize stress step completed: " +
        std::to_string(resizeStressCompletedCount) +
        "/" +
        std::to_string(resizeStressTotal) +
        " size=" +
        std::to_string(targetWidth) +
        "x" +
        std::to_string(targetHeight));

    if (resizeStressRemaining <= 0)
    {
        resizeStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "Resize stress completed: " +
            std::to_string(resizeStressCompletedCount) +
            "/" +
            std::to_string(resizeStressTotal) +
            " resize transactions succeeded.");
    }
}

void RuntimeTestHooks::UpdateRenderGraphReloadStress(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!graphReloadStressActive)
    {
        return;
    }

    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    if (graphReloadStressWaitingForDrain)
    {
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(
            pendingRetiredResources,
            graphReloadMaxPendingRetiredResources);

        if (pendingRetiredResources == 0)
        {
            graphReloadStressActive = false;
            graphReloadStressWaitingForDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            diagnostics.ReportInfo(
                "Render graph reload stress completed: " +
                std::to_string(graphReloadStressCompletedCount) +
                "/" +
                std::to_string(graphReloadStressTotal) +
                " reloads succeeded, retire queue max pending=" +
                std::to_string(graphReloadMaxPendingRetiredResources) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            return;
        }

        --graphReloadRetireDrainFramesRemaining;
        if (graphReloadRetireDrainFramesRemaining <= 0)
        {
            graphReloadStressActive = false;
            graphReloadStressWaitingForDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "Render graph reload stress failed because retired graph resources did not drain before the frame budget expired. pending=" +
                std::to_string(pendingRetiredResources) +
                ", submittedEpoch=" +
                std::to_string(retireQueue.GetLastSubmittedEpoch()) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
        }
        return;
    }

    if (graphReloadStressRemaining <= 0)
    {
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(
            pendingRetiredResources,
            graphReloadMaxPendingRetiredResources);

        if (graphReloadStressTotal > 1 && graphReloadMaxPendingRetiredResources == 0)
        {
            graphReloadStressActive = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "Render graph reload stress failed because no retired graph resources were observed after repeated reloads.");
            return;
        }

        graphReloadStressWaitingForDrain = true;
        graphReloadRetireDrainFramesRemaining = RetireDrainFrameBudget;
        diagnostics.ReportInfo(
            "Render graph reload stress waiting for retire queue drain: pending=" +
            std::to_string(pendingRetiredResources) +
            ", maxPending=" +
            std::to_string(graphReloadMaxPendingRetiredResources) +
            ".");
        return;
    }

    auto reloadResult = validationServices.ReloadRenderGraphResources();
    if (reloadResult.IsFailure())
    {
        graphReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportRuntimeError(
            "Render graph reload stress failed",
            reloadResult.Error());
        return;
    }

    --graphReloadStressRemaining;
    ++graphReloadStressCompletedCount;

    const size_t pendingRetiredResources = retireQueue.GetPendingCount();
    UpdateMaxPendingRetiredResources(
        pendingRetiredResources,
        graphReloadMaxPendingRetiredResources);
    diagnostics.ReportInfo(
        "Render graph reload stress reloaded graph " +
        std::to_string(graphReloadStressCompletedCount) +
        "/" +
        std::to_string(graphReloadStressTotal) +
        ", pending retired resources=" +
        std::to_string(pendingRetiredResources) +
        ".");
}

void RuntimeTestHooks::RecordFrameRenderLoopTime(double renderLoopTimeMs)
{
    if (!frameSmokeActive)
    {
        return;
    }

    frameSmokeIntervalRenderLoopTotalMs += renderLoopTimeMs;
    frameSmokeIntervalRenderLoopMaxMs =
        std::max(frameSmokeIntervalRenderLoopMaxMs, renderLoopTimeMs);
}

void RuntimeTestHooks::RecordFrameTime(
    double frameTimeMs,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!frameSmokeActive)
    {
        return;
    }

    if (frameTimeMs <= 0.0 || !std::isfinite(frameTimeMs))
    {
        frameSmokeActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "Frame smoke test failed because a measured frame time was invalid.");
        return;
    }

    ++frameSmokeCompletedCount;
    frameSmokeTotalMs += frameTimeMs;
    frameSmokeMaxMs = std::max(frameSmokeMaxMs, frameTimeMs);
    frameSmokeMinMs = std::min(frameSmokeMinMs, frameTimeMs);
    ++frameSmokeIntervalFrameCount;
    frameSmokeIntervalTotalMs += frameTimeMs;
    frameSmokeIntervalMaxMs = std::max(frameSmokeIntervalMaxMs, frameTimeMs);
    frameSmokeIntervalMinMs = std::min(frameSmokeIntervalMinMs, frameTimeMs);

    if (frameSmokeIntervalFrameCount >= frameSmokeIntervalSize)
    {
        ReportFrameSmokeInterval(diagnostics);
    }

    if (frameSmokeCompletedCount < frameSmokeTotal)
    {
        return;
    }

    if (frameSmokeIntervalFrameCount > 0)
    {
        ReportFrameSmokeInterval(diagnostics);
    }

    frameSmokeActive = false;
    runtimeTestStatus = RuntimeTestStatus::Succeeded;
    const double averageFrameMs = frameSmokeTotalMs /
        static_cast<double>(std::max(1, frameSmokeCompletedCount));
    const double averageFps = 1000.0 / averageFrameMs;

    diagnostics.ReportInfo(
        "Frame smoke test completed: " +
        std::to_string(frameSmokeCompletedCount) +
        "/" +
        std::to_string(frameSmokeTotal) +
        " frames, avgFrameMs=" +
        std::to_string(averageFrameMs) +
        ", minFrameMs=" +
        std::to_string(frameSmokeMinMs) +
        ", maxFrameMs=" +
        std::to_string(frameSmokeMaxMs) +
        ", avgFps=" +
        std::to_string(averageFps));
}

void RuntimeTestHooks::ReportFrameSmokeInterval(
    const DiagnosticsSubsystem& diagnostics)
{
    const double averageFrameMs = frameSmokeIntervalTotalMs /
        static_cast<double>(std::max(1, frameSmokeIntervalFrameCount));
    const double averageFps = 1000.0 / averageFrameMs;
    const double averageRenderLoopMs = frameSmokeIntervalRenderLoopTotalMs /
        static_cast<double>(std::max(1, frameSmokeIntervalFrameCount));
    const ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();

    diagnostics.ReportInfo(
        "Frame smoke interval: frame=" +
        std::to_string(frameSmokeCompletedCount) +
        "/" +
        std::to_string(frameSmokeTotal) +
        ", intervalFrames=" +
        std::to_string(frameSmokeIntervalFrameCount) +
        ", avgFrameMs=" +
        std::to_string(averageFrameMs) +
        ", minFrameMs=" +
        std::to_string(frameSmokeIntervalMinMs) +
        ", maxFrameMs=" +
        std::to_string(frameSmokeIntervalMaxMs) +
        ", avgFps=" +
        std::to_string(averageFps) +
        ", avgRenderLoopMs=" +
        std::to_string(averageRenderLoopMs) +
        ", maxRenderLoopMs=" +
        std::to_string(frameSmokeIntervalRenderLoopMaxMs) +
        ", retiredPending=" +
        std::to_string(retireQueue.GetPendingCount()) +
        ", submittedEpoch=" +
        std::to_string(retireQueue.GetLastSubmittedEpoch()) +
        ", completedEpoch=" +
        std::to_string(retireQueue.GetLastCompletedEpoch()));

    frameSmokeIntervalFrameCount = 0;
    frameSmokeIntervalTotalMs = 0.0;
    frameSmokeIntervalMaxMs = 0.0;
    frameSmokeIntervalMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalRenderLoopTotalMs = 0.0;
    frameSmokeIntervalRenderLoopMaxMs = 0.0;
}

bool RuntimeTestHooks::ShouldSuppressResizeEvent(uint32_t width, uint32_t height)
{
    if (!suppressNextResizeEvent)
    {
        return false;
    }

    if (width == suppressedResizeWidth && height == suppressedResizeHeight)
    {
        suppressNextResizeEvent = false;
        return true;
    }

    return false;
}

void RuntimeTestHooks::UpdateEnvironmentUpdateStress(
    CommandBus& commandBus,
    const WorldManager& worldManager,
    const EnvironmentUpdateDiagnostics& environmentDiagnostics,
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    --environmentUpdateStressFrameBudget;
    if (environmentUpdateStressFrameBudget <= 0)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress exceeded its frame budget.",
            diagnostics);
        return;
    }

    const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress requires an active World.",
            diagnostics);
        return;
    }

    const WorldEnvironment& worldEnvironment = activeWorld->GetEnvironment();
    if (worldEnvironment.type != EnvironmentType::ProceduralSky)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress requires a procedural-sky World.",
            diagnostics);
        return;
    }

    const EnvironmentUpdateProgress& progress = environmentDiagnostics.progress;
    const EnvironmentGpuTimingSnapshot& timing = environmentDiagnostics.gpuTiming;

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitInitialGeneration)
    {
        const bool initialTimingsReady =
            timing.supported &&
            timing.Get(EnvironmentGpuProduct::Cubemap).sampleCount > 0 &&
            timing.Get(EnvironmentGpuProduct::SphericalHarmonics).sampleCount > 0 &&
            timing.Get(EnvironmentGpuProduct::Prefilter).sampleCount > 0 &&
            timing.Get(EnvironmentGpuProduct::Commit).sampleCount > 0;
        if (progress.stage != EnvironmentUpdateStage::Idle ||
            progress.activeGeneration == 0 ||
            !initialTimingsReady)
        {
            return;
        }

        environmentUpdateStressOriginalSkyParameters = worldEnvironment.skyParameters;
        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        environmentUpdateStressPrefilterMipCount = progress.prefilterMipCount;
        environmentUpdateStressEnvironmentCubeIdentity =
            validationServices.GetWorldTextureIdentity(
                "environmentCube");
        environmentUpdateStressPrefilterCubeIdentity =
            validationServices.GetWorldTextureIdentity(
                "prefilteredEnvironmentCube");
        if (environmentUpdateStressEnvironmentCubeIdentity == 0 ||
            environmentUpdateStressPrefilterCubeIdentity == 0 ||
            environmentUpdateStressPrefilterMipCount == 0)
        {
            FailEnvironmentUpdateStress(
                "Environment update stress could not capture active environment resources.",
                diagnostics);
            return;
        }

        environmentUpdateStressBaselineTimingSamples[0] =
            timing.Get(EnvironmentGpuProduct::Cubemap).sampleCount;
        environmentUpdateStressBaselineTimingSamples[1] =
            timing.Get(EnvironmentGpuProduct::SphericalHarmonics).sampleCount;
        environmentUpdateStressBaselineTimingSamples[2] =
            timing.Get(EnvironmentGpuProduct::Prefilter).sampleCount;
        environmentUpdateStressBaselineTimingSamples[3] =
            timing.Get(EnvironmentGpuProduct::Commit).sampleCount;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::RequestMutation;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::RequestMutation)
    {
        SkyParametersGPU changedSkyParameters = environmentUpdateStressOriginalSkyParameters;
        const int mutationIndex = environmentUpdateStressCompletedCount + 1;
        const float direction = (mutationIndex % 2) == 1 ? 1.0f : -1.0f;
        Eigen::Vector3f changedSunDirection =
            changedSkyParameters.sunDirectionIntensity.head<3>();
        changedSunDirection.x() += direction * 0.02f;
        changedSunDirection.normalize();
        changedSkyParameters.sunDirectionIntensity.head<3>() = changedSunDirection;

        // 测试子系统只投递意图；active World 的可变状态仍由命令执行器在 owner 侧修改。
        RuntimeCommand command;
        command.type = RuntimeCommandType::SetProceduralSkyParameters;
        command.skyParametersValue = changedSkyParameters;
        command.sourceText = "runtime-test: environmentstress mutation";
        commandBus.Queue(std::move(command));

        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        environmentUpdateStressObservedPreviousResources = false;
        waitingForProceduralSkyParametersResult = true;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitMutation;
        diagnostics.ReportInfo(
            "Environment update stress requested dirty generation " +
            std::to_string(mutationIndex) +
            "/" +
            std::to_string(environmentUpdateStressTotal) +
            ".");
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitMutation)
    {
        if (waitingForProceduralSkyParametersResult)
        {
            return;
        }
        if (progress.usingPreviousResources &&
            progress.activeGeneration == environmentUpdateStressPreviousActiveGeneration)
        {
            environmentUpdateStressObservedPreviousResources = true;
        }

        if (progress.stage != EnvironmentUpdateStage::Idle ||
            progress.activeGeneration <= environmentUpdateStressPreviousActiveGeneration)
        {
            return;
        }
        if (!environmentUpdateStressObservedPreviousResources)
        {
            FailEnvironmentUpdateStress(
                "Environment update committed without exposing the previous-resource interval.",
                diagnostics);
            return;
        }
        if (validationServices.GetWorldTextureIdentity(
                "environmentCube") !=
                environmentUpdateStressEnvironmentCubeIdentity ||
            validationServices.GetWorldTextureIdentity(
                "prefilteredEnvironmentCube") !=
                environmentUpdateStressPrefilterCubeIdentity)
        {
            FailEnvironmentUpdateStress(
                "Environment active texture identity changed during an incremental generation.",
                diagnostics);
            return;
        }

        ++environmentUpdateStressCompletedCount;
        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        diagnostics.ReportInfo(
            "Environment update stress committed generation " +
            std::to_string(environmentUpdateStressCompletedCount) +
            "/" +
            std::to_string(environmentUpdateStressTotal) +
            ".");
        environmentUpdateStressPhase =
            environmentUpdateStressCompletedCount < environmentUpdateStressTotal
            ? EnvironmentUpdateStressPhase::RequestMutation
            : EnvironmentUpdateStressPhase::RequestRestore;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::RequestRestore)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::SetProceduralSkyParameters;
        command.skyParametersValue = environmentUpdateStressOriginalSkyParameters;
        command.sourceText = "runtime-test: environmentstress restore";
        commandBus.Queue(std::move(command));

        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        environmentUpdateStressObservedPreviousResources = false;
        waitingForProceduralSkyParametersResult = true;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitRestore;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitRestore)
    {
        if (waitingForProceduralSkyParametersResult)
        {
            return;
        }
        if (progress.usingPreviousResources &&
            progress.activeGeneration == environmentUpdateStressPreviousActiveGeneration)
        {
            environmentUpdateStressObservedPreviousResources = true;
        }
        if (progress.stage != EnvironmentUpdateStage::Idle ||
            progress.activeGeneration <= environmentUpdateStressPreviousActiveGeneration)
        {
            return;
        }
        if (!environmentUpdateStressObservedPreviousResources)
        {
            FailEnvironmentUpdateStress(
                "Environment restore generation did not preserve the previous active resources.",
                diagnostics);
            return;
        }

        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitTimingDrain;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitTimingDrain)
    {
        const uint64_t generationCount =
            static_cast<uint64_t>(environmentUpdateStressTotal + 1);
        const uint64_t expectedCubemapSamples =
            environmentUpdateStressBaselineTimingSamples[0] + generationCount * 6;
        const uint64_t expectedShSamples =
            environmentUpdateStressBaselineTimingSamples[1] + generationCount;
        const uint64_t expectedPrefilterSamples =
            environmentUpdateStressBaselineTimingSamples[2] +
            generationCount * environmentUpdateStressPrefilterMipCount;
        const uint64_t expectedCommitSamples =
            environmentUpdateStressBaselineTimingSamples[3] + generationCount;

        if (timing.Get(EnvironmentGpuProduct::Cubemap).sampleCount < expectedCubemapSamples ||
            timing.Get(EnvironmentGpuProduct::SphericalHarmonics).sampleCount < expectedShSamples ||
            timing.Get(EnvironmentGpuProduct::Prefilter).sampleCount < expectedPrefilterSamples ||
            timing.Get(EnvironmentGpuProduct::Commit).sampleCount < expectedCommitSamples)
        {
            return;
        }

        environmentUpdateStressActive = false;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::Idle;
        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "Environment update stress completed: changes=" +
            std::to_string(environmentUpdateStressCompletedCount) +
            ", cubeLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::Cubemap).lastMilliseconds) +
            ", shLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::SphericalHarmonics).lastMilliseconds) +
            ", prefilterLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::Prefilter).lastMilliseconds) +
            ", commitLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::Commit).lastMilliseconds) +
            ".");
    }
}

void RuntimeTestHooks::FailEnvironmentUpdateStress(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    environmentUpdateStressActive = false;
    waitingForProceduralSkyParametersResult = false;
    environmentUpdateStressPhase = EnvironmentUpdateStressPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::NotifyProceduralSkyParametersResult(
    bool succeeded,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!environmentUpdateStressActive)
    {
        return;
    }

    if (!waitingForProceduralSkyParametersResult)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress received an unexpected sky-parameter command result.",
            diagnostics);
        return;
    }

    waitingForProceduralSkyParametersResult = false;
    if (!succeeded)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress could not apply procedural sky parameters through CommandBus.",
            diagnostics);
    }
}

} // namespace VL

