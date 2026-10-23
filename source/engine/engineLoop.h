#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>
#include <vector>

#include "core/runtimeResult.h"
#include "core/cpuFrameProfiler.h"
#include "core/performanceJsonlWriter.h"
#include "platform/platformEvent.h"
#include "engine/runtimeCommand.h"
#include "engine/launchOptions.h"
#include "engine/modelPreviewCamera.h"
#include "engine/editorThumbnailScheduler.h"
#include "editor/assets/editorThumbnailLibrary.h"
#include "ui/uiSubsystem.h"
#include "world/loading/worldGraphTransactionCoordinator.h"
#include "shader/reload/shaderReloadRuntime.h"

class Controller;
class PipelineFactory;
class ShaderCompiler;

namespace VL
{

class PlatformApplication;
class EditorThumbnailRenderSession;
class PlatformWindow;
class GameInstance;
class RendererBackendVulkan;
class RenderThread;
class RuntimeConfig;
class RuntimeCommandExecutor;
class ScriptBuildService;
class BlueprintAssetLoadSession;
class ScriptWorldContext;
class RuntimeValidationServices;
class SubsystemCollection;
class WorldTransitionCoordinator;
enum class RenderGraphReleaseMode;
struct MaterialDefinitionReloadBatch;
struct RuntimeCommandExecutionResult;
struct WorldHandle;
// 引擎内部的动画预览派生数据缓存；定义在 engineLoop.cpp，UI 侧不接触。
struct UiAnimationPreviewCache;

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
    // 启动期控制台脚本入口：转发给 ConsoleSubsystem，延迟若干帧后提交，
    // 让 showcase / debug 截图可以脚本化复现。
    void QueueLaunchConsoleCommands(
        const std::vector<std::string>& lines,
        int delayFrames);

private:
    friend class RuntimeValidationServices;

    void Tick();
    void PumpPlatformEvents();
    void ApplyQueuedUiActions();
    void UpdateUiInputPolicy();
    void UpdateUiViewModel(
        float deltaTime,
        const RuntimeCommandExecutionResult& commandResult);
    // 只填充逐帧变化的轻量字段（世界路径、离线标记、调试绘制、相机）。
    void FillUiViewModelLiveFields(
        UiViewModelSnapshot& snapshot,
        UiViewModelBuildMode buildMode,
        const WorldHandle& activeWorld);
    // 只在编辑器快照到期时调用：一次构建全部昂贵编辑器字段。
    void BuildUiViewModelEditorFields(
        UiViewModelSnapshot& snapshot,
        const WorldHandle& activeWorld);
    // 动画预览的派生数据按“世界代数 + 播放器身份”缓存；每帧只重算随播放时间
    // 变化的值，不重复分配 clip 列表/特征通道/schema。
    void CaptureUiAnimationPreview(
        UiViewModelSnapshot& snapshot,
        const World& world,
        uint64_t generation);
    void SynchronizeSceneViewport();
    void TickEditorThumbnails();
    void TickScriptBuild(float deltaTime);
    // --perf-jsonl：按墙钟每秒把同一份 Performance 快照追加一行。
    void TickPerformanceJsonl();
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
            materialDefinitionReload = nullptr,
        // 本次加载附加的场景对象（JSON 对象数组，可为空）。模型编辑器靠它把打开的模型
        // 放进中性预览环境，而不生成临时场景文件。
        const nlohmann::json& extraSceneObjects = nlohmann::json::array());
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
    // 渲染主循环节拍上限（Hz）；0 = 不限速。只影响 Run() 的补帧，不改变固定 60 Hz 物理子步。
    int frameLimitHz = 0;
    std::vector<PlatformEvent> platformEvents;

    std::unique_ptr<ShaderCompiler> shaderCompiler;
    std::unique_ptr<ShaderReloadCoordinator> shaderReloadCoordinator;
    std::unique_ptr<ShaderReloadRuntime> shaderReloadRuntime;
    std::unique_ptr<PipelineFactory> pipelineFactory;
    std::unique_ptr<RendererBackendVulkan> rendererBackend;
    std::unique_ptr<RenderThread> renderThread;
    std::unique_ptr<RuntimeCommandExecutor> runtimeCommandExecutor;
    std::unique_ptr<ScriptBuildService> scriptBuild;
    void TickBlueprintAssets();
    std::unique_ptr<BlueprintAssetLoadSession> blueprintAssetLoad;
    nlohmann::json blueprintAssetRequests = nlohmann::json::array();
    nlohmann::json activeBlueprintAssetRequests = nlohmann::json::array();
    std::map<std::string, std::unique_ptr<BlueprintAssetLoadSession>> loadedBlueprintAssets;
    std::weak_ptr<ScriptWorldContext> blueprintAssetContext;
    uint64_t blueprintAssetWorldGeneration = 0;
    bool autoCompileScripts = false;
    std::optional<std::filesystem::path> pendingBuiltAssembly;
    uint64_t pendingBuildWorldGeneration = 0;
    uint64_t autoPlayWorldGeneration = 0;
    std::string scriptBuildOutput;
    std::unique_ptr<RuntimeValidationServices> runtimeValidationServices;
    std::unique_ptr<WorldTransitionCoordinator> worldTransitionCoordinator;
    std::unique_ptr<WorldGraphTransactionCoordinator>
        worldGraphTransactionCoordinator;
    std::unique_ptr<Controller> controller;
    ModelPreviewCamera modelPreviewCamera;
    std::unique_ptr<UiSubsystem> uiSubsystem;
    std::unique_ptr<EditorThumbnailRenderSession> thumbnailSession;
    std::optional<Editor::ThumbnailRenderRequest> thumbnailRequest;
    uint64_t thumbnailSessionEpoch = 0;
    EditorThumbnailScheduler thumbnailScheduler;
    uint64_t uiFrameIndex = 0;
    // 视图模型可见性/刷新节拍：隐藏时不构建编辑器快照；可见时按 10 Hz 重建，
    // 选中/模式切换/世界换代/编译器状态等事件通过显式脏标记绕过等待。
    UiViewModelRefreshSchedule uiViewModelRefresh;
    UiViewModelBuildMode uiViewModelBuildMode = UiViewModelBuildMode::None;
    bool uiViewModelScriptBuildPending = false;
    uint64_t lastUiViewModelWorldGeneration = 0;
    std::string lastUiViewModelWorldPath;
    bool lastUiViewModelOfflineMode = false;
    ScriptPlayState lastUiViewModelScriptState = ScriptPlayState::Editing;
    std::unique_ptr<UiAnimationPreviewCache> uiAnimationPreviewCache;
    std::string pendingScriptBlueprintSelection;
    WorldGraphTransactionTestFaultInjection
        worldGraphTransactionTestFaultInjection;
    // 时间序列由 GT 写盘，避免渲染线程做文件 I/O；未开启时 IsOpen() 为 false。
    PerformanceJsonlWriter perfJsonl;
    std::chrono::steady_clock::time_point nextPerfJsonlWrite{};
    // GT 独占的 CPU 阶段计时器；每帧 CommitFrame 后交给 RenderSystem 的唯一快照。
    CpuFrameProfiler cpuFrameProfiler;
};

} // namespace VL
