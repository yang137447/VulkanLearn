#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "engine/runtimeCommand.h"

class Controller;
class PipelineFactory;

namespace VL
{

class PlatformApplication;
class PlatformWindow;
class GameInstance;
class RendererBackendVulkan;
class RenderThread;
class RuntimeConfig;
class RuntimeCommandExecutor;
class SubsystemCollection;
class WorldTransitionCoordinator;
enum class RenderGraphReleaseMode;
struct WorldHandle;

// Owns the high-level runtime lifecycle after PlatformApplication is ready.
// PlatformApplication hides SDL startup/window/event details; EngineLoop owns
// engine init, per-frame event/update/render dispatch, and renderer shutdown.
class EngineLoop
{
public:
    EngineLoop();
    ~EngineLoop();

    RuntimeResult<void> Init(PlatformApplication& platformApplication, const RuntimeConfig& runtimeConfig);
    int Run();
    void Shutdown();
    void QueueRuntimeCommand(RuntimeCommand command);
    void SetExitAfterRuntimeTests(bool enabled);
    void StartResizeStress(int resizeCount);
    void StartRenderGraphReloadStress(int reloadCount);
    void StartFrameSmokeTest(int frameCount);

private:
    void Tick();
    void PumpPlatformEvents();
    const RuntimeConfig& GetRuntimeConfig() const;
    SubsystemCollection& GetSubsystems();
    RuntimeResult<void> InitializeRuntimeSystems(
        PlatformWindow& window,
        std::vector<const char*>& vulkanExtensions);
    RuntimeResult<void> LoadInitialWorldAndRenderer();
    RuntimeResult<void> BindActiveWorldRuntimeObjects(const WorldHandle& worldHandle);
    RuntimeResult<void> RecreateRendererForWindowResize(uint32_t width, uint32_t height);
    RuntimeResult<void> ReloadRenderGraphResources(VL::RenderGraphReleaseMode releaseMode);
    void WaitForRenderThreadIdle();
    void PollRenderThreadFatalError();
    void UpdateResizeStress();
    void UpdateRenderGraphReloadStress();
    void UpdateFrameSmokeTest(double frameTimeMs);
    void ReportFrameSmokeInterval();
    void AddFrameSmokeRenderLoopTime(double renderLoopTimeMs);
    bool ShouldSuppressResizeEvent(uint32_t width, uint32_t height);

    PlatformApplication* platformApplication = nullptr;
    PlatformWindow* window = nullptr;
    std::unique_ptr<GameInstance> gameInstance;
    bool shouldClose = false;
    bool rendererBackendInitialized = false;
    bool shutdownCompleted = false;
    bool exitAfterRuntimeTests = false;
    int exitCode = 0;
    std::vector<PlatformEvent> platformEvents;
    bool resizeStressActive = false;
    bool resizeStressCompleted = false;
    bool resizeStressFailed = false;
    int resizeStressTotal = 0;
    int resizeStressRemaining = 0;
    int resizeStressCompletedCount = 0;
    bool graphReloadStressActive = false;
    bool graphReloadStressCompleted = false;
    bool graphReloadStressFailed = false;
    bool graphReloadStressWaitingForDrain = false;
    int graphReloadStressTotal = 0;
    int graphReloadStressRemaining = 0;
    int graphReloadStressCompletedCount = 0;
    int graphReloadRetireDrainFramesRemaining = 0;
    size_t graphReloadMaxPendingRetiredResources = 0;
    bool frameSmokeActive = false;
    bool frameSmokeCompleted = false;
    bool frameSmokeFailed = false;
    int frameSmokeTotal = 0;
    int frameSmokeCompletedCount = 0;
    double frameSmokeTotalMs = 0.0;
    double frameSmokeMaxMs = 0.0;
    double frameSmokeMinMs = 0.0;
    int frameSmokeIntervalSize = 5000;
    int frameSmokeIntervalFrameCount = 0;
    double frameSmokeIntervalTotalMs = 0.0;
    double frameSmokeIntervalMaxMs = 0.0;
    double frameSmokeIntervalMinMs = 0.0;
    double frameSmokeIntervalRenderLoopTotalMs = 0.0;
    double frameSmokeIntervalRenderLoopMaxMs = 0.0;
    bool suppressNextResizeEvent = false;
    uint32_t suppressedResizeWidth = 0;
    uint32_t suppressedResizeHeight = 0;

    std::unique_ptr<PipelineFactory> pipelineFactory;
    std::unique_ptr<RendererBackendVulkan> rendererBackend;
    std::unique_ptr<RenderThread> renderThread;
    std::unique_ptr<RuntimeCommandExecutor> runtimeCommandExecutor;
    std::unique_ptr<WorldTransitionCoordinator> worldTransitionCoordinator;
    std::unique_ptr<Controller> controller;
};

} // namespace VL
