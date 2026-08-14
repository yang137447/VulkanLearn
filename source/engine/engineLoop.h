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
class RuntimeTestHooks;
class SubsystemCollection;
class WorldTransitionCoordinator;
class ShaderReloadCoordinator;
class ShaderCompileWorker;
class ShaderFileMonitor;
enum class RenderGraphReleaseMode;
struct MaterialDefinitionReloadBatch;
struct RuntimeCommandExecutionResult;
struct ShaderCompileWorkerResult;
struct ShaderReloadPlan;
struct WorldHandle;

struct WorldGraphTransactionTestFaultInjection
{
    size_t failGraphResourceCreationAt = 0;
    size_t failRenderPassCreationAt = 0;
    size_t failFramebufferCreationAt = 0;
    size_t failDescriptorCreationAt = 0;
    bool failPassMaterialContract = false;
    bool failAfterCandidateWorldBuilt = false;
    bool failViewTargetPrecheck = false;
    bool failAfterRuntimeBindingPrepared = false;
    bool failBeforeCommit = false;
    bool failResizeAfterSwapchainRecreate = false;
};

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
    int GetExitCode() const noexcept { return exitCode; }
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
        DeveloperUiLaunchMode developerUiMode,
        bool forceShaderRebuild);
    RuntimeResult<void> LoadInitialWorldAndRenderer();
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
    void ProcessPendingMaterialDefinitionReload();
    void MergePendingAutomaticShaderSources(
        const std::vector<std::string>& sourceIdentities,
        uint64_t sourceEpoch);
    void SubmitPendingAutomaticShaderReload();

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
    std::unique_ptr<ShaderFileMonitor> shaderFileMonitor;
    std::unique_ptr<ShaderCompileWorker> shaderCompileWorker;
    std::unique_ptr<PipelineFactory> pipelineFactory;
    std::unique_ptr<RendererBackendVulkan> rendererBackend;
    std::unique_ptr<RenderThread> renderThread;
    std::unique_ptr<RuntimeCommandExecutor> runtimeCommandExecutor;
    std::unique_ptr<WorldTransitionCoordinator> worldTransitionCoordinator;
    std::unique_ptr<Controller> controller;
    std::unique_ptr<UiSubsystem> uiSubsystem;
    uint64_t uiFrameIndex = 0;
    uint64_t nextShaderReloadGeneration = 1;
    uint64_t latestObservedSourceEpoch = 0;
    uint64_t latestSubmittedAutoReloadGeneration = 0;
    uint64_t inFlightAutoReloadGeneration = 0;
    uint64_t inFlightAutoReloadSourceEpoch = 0;
    uint64_t latestManualShaderReloadGeneration = 0;
    uint64_t latestManualShaderReloadCommittedGeneration = 0;
    uint64_t latestManualShaderReloadFailedGeneration = 0;
    uint64_t latestAutoReloadStaleDiscardGeneration = 0;
    uint64_t latestAutoReloadFailedGeneration = 0;
    uint64_t latestAutoReloadCommittedGeneration = 0;
    uint64_t latestAutoReloadShadercInvocations = 0;
    uint64_t totalAutoReloadShadercInvocations = 0;
    uint64_t pendingAutoReloadSourceEpoch = 0;
    uint64_t failedPendingAutoReloadSourceEpoch = 0;
    uint64_t pendingMaterialDefinitionSourceEpoch = 0;
    uint64_t failedPendingMaterialDefinitionSourceEpoch = 0;
    uint64_t latestMaterialDefinitionReloadCommittedGeneration = 0;
    uint64_t latestMaterialDefinitionReloadFailedGeneration = 0;
    std::set<std::string> pendingAutoReloadSources;
    std::set<std::string> pendingMaterialDefinitionSources;
    std::vector<std::string> lastSubmittedAutoReloadSources;
    std::vector<std::string> lastStaleAutoReloadSources;
    std::vector<std::string> lastCommittedAutoReloadSources;
    WorldGraphTransactionTestFaultInjection
        worldGraphTransactionTestFaultInjection;
};

} // namespace VL
