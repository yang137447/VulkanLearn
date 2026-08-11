#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "engine/runtimeCommand.h"
#include "engine/launchOptions.h"
#include "ui/uiSubsystem.h"

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
class RuntimeTestHooks;
class SubsystemCollection;
class WorldTransitionCoordinator;
enum class RenderGraphReleaseMode;
struct RuntimeCommandExecutionResult;
struct WorldHandle;

// Owns the high-level runtime lifecycle after PlatformApplication is ready.
// PlatformApplication hides SDL startup/window/event details; EngineLoop owns
// engine init, per-frame event/update/render dispatch, and renderer shutdown.
class EngineLoop
{
public:
    EngineLoop();
    ~EngineLoop();

    RuntimeResult<void> Init(
        PlatformApplication& platformApplication,
        const RuntimeConfig& runtimeConfig,
        const LaunchOptions& launchOptions);
    int Run();
    void Shutdown();
    void QueueRuntimeCommand(RuntimeCommand command);
    void SetExitAfterRuntimeTests(bool enabled);

private:
    friend class RuntimeTestHooks;

    void Tick();
    void PumpPlatformEvents();
    void ApplyQueuedUiActions();
    void UpdateUiInputPolicy();
    void UpdateUiViewModel(float deltaTime);
    const RuntimeConfig& GetRuntimeConfig() const;
    SubsystemCollection& GetSubsystems();
    RuntimeResult<void> InitializeRuntimeSystems(
        PlatformWindow& window,
        std::vector<const char*>& vulkanExtensions,
        DeveloperUiLaunchMode developerUiMode);
    RuntimeResult<void> LoadInitialWorldAndRenderer();
    RuntimeResult<void> BindActiveWorldRuntimeObjects(const WorldHandle& worldHandle);
    RuntimeResult<void> RecreateRendererForWindowResize(uint32_t width, uint32_t height);
    RuntimeResult<void> ReloadRenderGraphResources();
    RuntimeResult<void> ReloadRenderGraphResources(VL::RenderGraphReleaseMode releaseMode);
    void WaitForRenderThreadIdle();
    void PollRenderThreadFatalError();

    PlatformApplication* platformApplication = nullptr;
    PlatformWindow* window = nullptr;
    std::unique_ptr<GameInstance> gameInstance;
    bool shouldClose = false;
    bool rendererBackendInitialized = false;
    bool shutdownCompleted = false;
    bool exitAfterRuntimeTests = false;
    int exitCode = 0;
    std::vector<PlatformEvent> platformEvents;

    std::unique_ptr<PipelineFactory> pipelineFactory;
    std::unique_ptr<RendererBackendVulkan> rendererBackend;
    std::unique_ptr<RenderThread> renderThread;
    std::unique_ptr<RuntimeCommandExecutor> runtimeCommandExecutor;
    std::unique_ptr<WorldTransitionCoordinator> worldTransitionCoordinator;
    std::unique_ptr<Controller> controller;
    std::unique_ptr<UiSubsystem> uiSubsystem;
    uint64_t uiFrameIndex = 0;
};

} // namespace VL
