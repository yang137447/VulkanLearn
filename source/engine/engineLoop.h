#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "engine/runtimeCommand.h"
#include "engine/launchOptions.h"
#include "ui/uiSubsystem.h"
#include "world/loading/worldGraphTransactionCoordinator.h"
#include "shader/reload/shaderReloadRuntime.h"

class Controller;
class PipelineFactory;
class ShaderCompiler;

namespace VL
{

class PlatformApplication;
class PlatformWindow;
class GameInstance;
class RendererBackendVulkan;
class RenderThread;
class RuntimeConfig;
class RuntimeCommandExecutor;
class RuntimeValidationServices;
class SubsystemCollection;
class WorldTransitionCoordinator;
enum class RenderGraphReleaseMode;
struct MaterialDefinitionReloadBatch;
struct RuntimeCommandExecutionResult;
struct WorldHandle;

// Owns the high-level runtime lifecycle after PlatformApplication is ready.
// PlatformApplication hides SDL startup/window/event details; EngineLoop owns
// engine init, per-frame event/update/render dispatch, and renderer shutdown.
class EngineLoop : public ShaderReloadRuntimeHost
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
    int GetExitCode() const noexcept { return exitCode; }
    void QueueRuntimeCommand(RuntimeCommand command);
    void SetExitAfterRuntimeTests(bool enabled);

private:
    friend class RuntimeValidationServices;

    void Tick();
    void PumpPlatformEvents();
    void ApplyQueuedUiActions();
    void UpdateUiInputPolicy();
    void UpdateUiViewModel(float deltaTime);
    const RuntimeConfig& GetRuntimeConfig() const;
    SubsystemCollection& GetSubsystems();
    const SubsystemCollection& GetSubsystems() const;
    RuntimeResult<void> InitializeRuntimeSystems(
        PlatformWindow& window,
        std::vector<const char*>& vulkanExtensions,
        DeveloperUiLaunchMode developerUiMode,
        bool forceShaderRebuild);
    RuntimeResult<void> LoadInitialWorldAndRenderer(
        const std::string& initialSceneOverride);
    RuntimeResult<void> BindActiveWorldRuntimeObjects(const WorldHandle& worldHandle);
    RuntimeResult<void> RecreateRendererForWindowResize(uint32_t width, uint32_t height);
    RuntimeResult<void> ReloadRenderGraphResources();
    RuntimeResult<void> ReloadRenderGraphResources(VL::RenderGraphReleaseMode releaseMode);
    void WaitForRenderThreadIdle();
    void PollRenderThreadFatalError();
    void ProcessShaderRuntimeRequests(
        const RuntimeCommandExecutionResult& commandResult);
    void ProcessAutomaticShaderReloads();
    RuntimeResult<WorldHandle> ExecuteWorldGraphTransaction(
        const std::string& scenePath,
        const MaterialDefinitionReloadBatch*
            materialDefinitionReload = nullptr);
    RuntimeResult<WorldHandle>
        ExecuteMaterialDefinitionWorldGraphTransactionForTest(
            const std::set<std::string>& sourceIdentities,
            uint64_t batchId);
    void SetWorldGraphTransactionTestFaultInjection(
        WorldGraphTransactionTestFaultInjection injection) noexcept;
    void ProcessRequestedWorldTransition(
        RuntimeCommandExecutionResult& commandResult);

    uint64_t GetShaderReloadWorldGeneration() const noexcept override;
    void WaitForShaderReloadSafePoint() override;
    bool IsShaderReloadClosing() const noexcept override;
    void RefreshSceneAfterShaderReload() override;
    size_t GetShaderReloadRetirePendingCount() const noexcept override;
    RuntimeResult<WorldHandle> CommitMaterialDefinitionReload(
        const MaterialDefinitionReloadBatch& batch) override;

    PlatformApplication* platformApplication = nullptr;
    PlatformWindow* window = nullptr;
    std::unique_ptr<GameInstance> gameInstance;
    bool shouldClose = false;
    bool rendererBackendInitialized = false;
    bool shutdownCompleted = false;
    bool exitAfterRuntimeTests = false;
    bool useRenderThread = false;
    int exitCode = 0;
    std::vector<PlatformEvent> platformEvents;

    std::unique_ptr<ShaderCompiler> shaderCompiler;
    std::unique_ptr<ShaderReloadCoordinator> shaderReloadCoordinator;
    std::unique_ptr<ShaderReloadRuntime> shaderReloadRuntime;
    std::unique_ptr<PipelineFactory> pipelineFactory;
    std::unique_ptr<RendererBackendVulkan> rendererBackend;
    std::unique_ptr<RenderThread> renderThread;
    std::unique_ptr<RuntimeCommandExecutor> runtimeCommandExecutor;
    std::unique_ptr<RuntimeValidationServices> runtimeValidationServices;
    std::unique_ptr<WorldTransitionCoordinator> worldTransitionCoordinator;
    std::unique_ptr<WorldGraphTransactionCoordinator>
        worldGraphTransactionCoordinator;
    std::unique_ptr<Controller> controller;
    std::unique_ptr<UiSubsystem> uiSubsystem;
    uint64_t uiFrameIndex = 0;
    WorldGraphTransactionTestFaultInjection
        worldGraphTransactionTestFaultInjection;
};

} // namespace VL
