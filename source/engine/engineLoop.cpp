#include "engine/engineLoop.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include "SDL3/SDL.h"
#include "controller.h"
#include "engine/gameInstance.h"
#include "engine/modelPreviewRequest.h"
#include "engine/modelPreviewRotation.h"
#include "engine/blueprintPreviewRequest.h"
#include "script/scriptBuildService.h"
#include "script/scriptBuildDiagnostics.h"
#include "script/scriptSourceLocation.h"
#include "engine/blueprintPreviewTransform.h"
#include "engine/blueprintAssetLoadSession.h"
#include "input/mouseMotionBatch.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeTestHooks.h"
#include "engine/subsystemCollection.h"
#include "engine/testing/runtimeValidationServices.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "editor/scene/scriptBlueprintAuthoring.h"
#include "editor/service/sceneDocumentService.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/selection/materialInstanceSelection.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "material/generator/materialDefinitionReloadBatch.h"
#include "pipeline/pipelineFactory.h"
#include "engine/editorThumbnailRenderSession.h"
#include "platform/platformApplication.h"
#include "platform/platformWindow.h"
#include "profiler.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/renderThread.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "render/resource/rendererResourceLoadCoordinator.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "shaderCompiler.h"
#include "world/loading/worldTransitionCoordinator.h"
#include "world/world.h"
#include "ui/uiRenderSnapshot.h"
#include "animation/motionFeatures.h"
#include "animation/skeletalPlayback.h"

namespace VL
{

// 动画预览派生数据：只有 active World 换代或 preview_model 播放器身份变化时失效。
// schema/通道名/clip 列表与播放时间无关，不能每帧重算。
struct UiAnimationPreviewCache
{
    const Animation::SkeletalPlayback* player = nullptr;
    uint64_t worldGeneration = 0;
    std::vector<std::string> clipIds;
    Animation::FeatureSchema schema;
    std::vector<std::string> featureChannels;
};

namespace
{

// 开发者 UI 隐藏且 motion debug 关闭时复用的冻结空视图：避免每帧为
// CaptureDebugDraw 构造一个禁用的 MotionDebugView，也避免缓存随场景增长。
const std::shared_ptr<const Animation::MotionDebugView>& DisabledMotionDebugView()
{
    static const std::shared_ptr<const Animation::MotionDebugView> view =
        std::make_shared<const Animation::MotionDebugView>();
    return view;
}

// 调试叠层的 UI 可见状态签名。快照几何每帧都新分配，但面板只消费
// 开关/类别/owner/计数这些字段；用它们的签名判断“这一节是否真的变了”。
uint64_t UiDebugDrawSignature(const Debug::Snapshot& snapshot)
{
    UiSectionRevision revision;
    revision.Mix(snapshot.enabled ? 1u : 0u)
        .Mix(snapshot.visibleCount)
        .Mix(snapshot.storedCount);
    for (const char character : snapshot.ownerFilter)
    {
        revision.Mix(static_cast<uint64_t>(static_cast<unsigned char>(character)));
    }
    for (const auto& category : snapshot.categories)
    {
        revision.Mix(category.enabled ? 1u : 0u).Mix(category.count);
        for (const char character : category.name)
        {
            revision.Mix(static_cast<uint64_t>(static_cast<unsigned char>(character)));
        }
    }
    for (const auto& owner : snapshot.owners)
    {
        for (const char character : owner)
        {
            revision.Mix(static_cast<uint64_t>(static_cast<unsigned char>(character)));
        }
    }
    return revision.Value();
}

RenderGraph::TestFaultInjection BuildRenderGraphTestFaultInjection(
    const WorldGraphTransactionTestFaultInjection& injection)
{
    RenderGraph::TestFaultInjection graphFault;
    graphFault.failResourceCreationAt =
        injection.failGraphResourceCreationAt;
    graphFault.failRenderPassCreationAt =
        injection.failRenderPassCreationAt;
    graphFault.failFramebufferCreationAt =
        injection.failFramebufferCreationAt;
    graphFault.failDescriptorCreationAt =
        injection.failDescriptorCreationAt;
    graphFault.failPassMaterialContract =
        injection.failPassMaterialContract;
    return graphFault;
}

} // namespace

EngineLoop::EngineLoop() = default;
EngineLoop::~EngineLoop() = default;

RuntimeResult<void> EngineLoop::Init(
    PlatformApplication& platformApplication,
    const RuntimeConfig& runtimeConfig,
    const LaunchOptions& launchOptions)
{
    PlatformWindow& platformWindow = platformApplication.GetWindow();
    if (!platformWindow.IsValid())
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.NullWindow",
            "Cannot initialize EngineLoop before PlatformApplication creates a valid window."));
    }

    this->platformApplication = &platformApplication;
    this->window = &platformWindow;
    gameInstance = std::make_unique<GameInstance>(runtimeConfig);
    runtimeValidationServices =
        std::make_unique<RuntimeValidationServices>(*this);
    shouldClose = false;
    shutdownCompleted = false;
    useRenderThread =
        launchOptions.workerThreadCountOverride
            .value_or(
                runtimeConfig.GetWorkerThreadCount()) == 2;
    // --frame-limit 只服务帧率验收，不参与渲染资源或脚本配置；
    // 0 表示保持原有的不限速行为。
    frameLimitHz = launchOptions.frameLimitHz;

    auto inputResult = GetSubsystems().GetInputSubsystem().Initialize(platformWindow);
    if (inputResult.IsFailure())
    {
        return inputResult;
    }

    auto runtimeResult = InitializeRuntimeSystems(
        platformWindow,
        platformApplication.GetVulkanExtensions(),
        launchOptions.developerUiMode,
        launchOptions.forceShaderRebuild);
    if (runtimeResult.IsFailure())
    {
        return runtimeResult;
    }

    RenderSystem::GetInstance().SetInitialOfflineMode(launchOptions.offlineRender);
    // 必须在 LoadInitialWorldAndRenderer 之前设置：材质 shader variant 与对象
    // GPU 资源在 World 加载事务里按该 flag 选择预蒙皮路径。
    RenderSystem::GetInstance().SetPrerenderSkinEnabled(launchOptions.prerenderSkin);
    // 必须在 LoadInitialWorldAndRenderer 之前设置：帧资源初始化时按该标志决定是否
    // 在首次采集时创建 timestamp query pool。
    RenderSystem::GetInstance().SetGpuPassStatsLoggingEnabled(launchOptions.gpuStats);
    gpuStatsEnabled = launchOptions.gpuStats;
    nextUiSectionStats = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    if (launchOptions.perfJsonlPath.has_value())
    {
        // --perf-jsonl 的时间序列需要 pass 数据，因此同时开启采集；
        // 打开失败必须在启动时给出原因，不能留下"看似在跑但文件为空"的假象。
        std::string jsonlError;
        if (!perfJsonl.Open(*launchOptions.perfJsonlPath, jsonlError))
        {
            return RuntimeResult<void>::Failure(
                MakeRuntimeError("EngineLoop.PerfJsonl", jsonlError));
        }
        RenderSystem::GetInstance().EnableGpuPassCollection();
        nextPerfJsonlWrite = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    auto worldResult = LoadInitialWorldAndRenderer(
        launchOptions.initialSceneOverride);
    if (worldResult.IsFailure())
    {
        return worldResult;
    }

    window->CenterOnScreen();
    window->Show();
    if (launchOptions.focusWindow) window->Focus();
#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
    if (runtimeConfig.GetScriptSettings().enabled)
    {
        scriptBuild = std::make_unique<ScriptBuildService>();
        const auto session = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        scriptBuild->Configure(VL_SCRIPT_DOTNET_EXECUTABLE, VL_SCRIPT_GAMEPLAY_PROJECT,
            std::filesystem::path(runtimeConfig.GetResourcePath()) / "Generated/Runtime/ScriptBuilds" / session,
            std::filesystem::path(runtimeConfig.GetScriptSettings().runtimeDirectory) / "VulkanLearn.Runtime.dll",
            VL_SCRIPT_BUILD_CONFIGURATION, runtimeConfig.GetResourcePath(), VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY);
    }
#endif
    UpdateUiInputPolicy();
    GetSubsystems().GetRuntimeClock().Reset();
    return RuntimeResult<void>::Success();
}

int EngineLoop::Run()
{
    while (!shouldClose)
    {
        // 帧率验收：每帧从渲染起点补足到目标节拍。RuntimeClock 仍按真实经过时间推进，
        // 因此 time.delta 与动画/脚本 tick 的 dt 会随限速后的帧间隔变大；
        // 模拟侧随后把单帧 dt 夹到 0.1 s；ScriptRuntime 刚体的 1/60 子步在物理世界内部。
        const std::chrono::steady_clock::time_point frameStart =
            std::chrono::steady_clock::now();
        Tick();

        if (frameLimitHz <= 0)
        {
            continue;
        }

        const std::chrono::steady_clock::duration framePeriod =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    1.0 / static_cast<double>(frameLimitHz)));
        const std::chrono::steady_clock::time_point deadline =
            frameStart + framePeriod;
        const std::chrono::steady_clock::duration remaining =
            deadline - std::chrono::steady_clock::now();
        // SDL_DelayPrecise 内部用 1 ms 短睡探测实际唤醒开销，最后自旋收尾，
        // 是高精度等待；这里再留 0.2 ms 余量吸收当前线程的唤醒/调度延迟，
        // 随后自旋到 deadline。只限制渲染节拍，固定 60 Hz 物理子步不变。
        const std::chrono::steady_clock::duration spinMargin =
            std::chrono::microseconds(200);
        if (remaining > spinMargin)
        {
            const auto waitNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    remaining - spinMargin)
                    .count();
            SDL_DelayPrecise(static_cast<Uint64>(waitNs));
        }
        while (std::chrono::steady_clock::now() < deadline)
        {
        }
    }

    return exitCode;
}

void EngineLoop::Shutdown()
{
    if (shutdownCompleted)
    {
        return;
    }

    // Stop the CPU compile worker before any Vulkan teardown. The worker never
    // touches Vulkan, and dropping its in-flight candidate here guarantees no
    // callback can run after device resources are released.
    perfJsonl.Close();
    const DiagnosticsSubsystem& diagnostics =
        GetSubsystems().GetDiagnosticsSubsystem();
    if (scriptBuild) scriptBuild->Shutdown();
    // 取消不阻塞普通帧；退出时统一等待纯CPU任务，再释放配置和renderer所有者。
    if (blueprintAssetLoad) blueprintAssetLoad->Cancel();
    blueprintAssetLoad.reset();
    BlueprintAssetLoadSession::WaitForWorkers();
    loadedBlueprintAssets.clear();
    GetSubsystems().GetScriptRuntime().Shutdown();
    if (shaderReloadRuntime)
    {
        shaderReloadRuntime->Shutdown();
    }

    if (rendererBackendInitialized)
    {
        if (renderThread)
        {
            renderThread->Stop();
            renderThread.reset();
        }

        rendererBackend->WaitIdle();
        thumbnailSession.reset();
        thumbnailRequest.reset();
        if (uiSubsystem)
        {
            RenderSystem::GetInstance().SetUiRenderSnapshotQueue(nullptr);
            uiSubsystem->Shutdown();
            uiSubsystem.reset();
        }
        RenderSystem::GetInstance().SetActiveWorld(nullptr);
        RenderSystem::GetInstance().ShutdownRenderObject();
        RenderGraph::GetInstance().Shutdown(*rendererBackend);
        controller.reset();
        GetSubsystems().GetWorldManager().ClearActiveWorld();
        RendererResourceCache::GetInstance().Clear();
        ResourceRetireQueue::GetInstance().ForceReleaseAll();
        RenderSystem::GetInstance().SetRendererBackend(nullptr);
        RenderSystem::GetInstance().SetPipelineFactory(nullptr);

    }

    Profiler::Instance().EndSession();
    shutdownCompleted = true;
    diagnostics.ReportInfo(
        "EngineLoop teardown complete.");
}

void EngineLoop::QueueRuntimeCommand(RuntimeCommand command)
{
    GetSubsystems().GetCommandBus().Queue(std::move(command));
}

void EngineLoop::SetExitAfterRuntimeTests(bool enabled)
{
    exitAfterRuntimeTests = enabled;
}

void EngineLoop::QueueLaunchConsoleCommands(const std::vector<std::string>& lines, int delayFrames)
{
    GetSubsystems().GetConsoleSubsystem().QueueLaunchCommands(lines, delayFrames);
}

uint64_t EngineLoop::GetShaderReloadWorldGeneration() const noexcept
{
    return GetSubsystems()
        .GetWorldManager()
        .GetActiveWorldHandle()
        .generation;
}

void EngineLoop::WaitForShaderReloadSafePoint()
{
    WaitForRenderThreadIdle();
}

bool EngineLoop::IsShaderReloadClosing() const noexcept
{
    return shouldClose;
}

void EngineLoop::RefreshSceneAfterShaderReload()
{
    RenderSystem::GetInstance()
        .RefreshResolvedSceneAfterShaderReload();
}

size_t EngineLoop::GetShaderReloadRetirePendingCount() const noexcept
{
    return ResourceRetireQueue::GetInstance().GetPendingCount();
}

RuntimeResult<WorldHandle>
EngineLoop::CommitMaterialDefinitionReload(
    const MaterialDefinitionReloadBatch& batch)
{
    return ExecuteWorldGraphTransaction(
        GetSubsystems().GetWorldManager()
            .GetActiveWorldHandle()
            .scenePath,
        &batch);
}

RuntimeResult<void> EngineLoop::InitializeRuntimeSystems(
    PlatformWindow& window,
    std::vector<const char*>& vulkanExtensions,
    DeveloperUiLaunchMode developerUiMode,
    bool forceShaderRebuild)
{
    // Material parameter includes are generated before shader compilation so
    // GLSL sees the latest JSON-driven parameter layout.
    MaterialParameterIncludeGenerator::GenerateAllIncludes();
    std::string shaderFolderPath = GetRuntimeConfig().ResolvePath("shader");
    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Shader folder path: " + shaderFolderPath);
    shaderCompiler = std::make_unique<ShaderCompiler>();
    shaderCompiler->StartCompile(
        shaderFolderPath,
        forceShaderRebuild);

    rendererBackend = std::make_unique<RendererBackendVulkan>();
    rendererBackend->Initialize(vulkanExtensions, window.GetNativeHandle());
    rendererBackendInitialized = true;
    RenderSystem::GetInstance().SetRendererBackend(rendererBackend.get());

    pipelineFactory = rendererBackend->CreatePipelineFactory();
    pipelineFactory->SetShaderCompiler(shaderCompiler.get());
    shaderReloadCoordinator =
        std::make_unique<ShaderReloadCoordinator>(
            *shaderCompiler,
            *pipelineFactory);
    RenderSystem::GetInstance().SetShaderReloadCoordinator(
        shaderReloadCoordinator.get());
    shaderReloadRuntime = std::make_unique<ShaderReloadRuntime>();
    shaderReloadRuntime->Initialize(
        *shaderCompiler,
        *shaderReloadCoordinator,
        shaderCompiler->GetShaderRoot());
    RenderSystem::GetInstance().SetPipelineFactory(pipelineFactory.get());
    runtimeCommandExecutor = std::make_unique<RuntimeCommandExecutor>();
    RendererResourceLoadCoordinator& resourceLoadCoordinator =
        RendererResourceLoadCoordinator::GetInstance();
    resourceLoadCoordinator.SetPipelineFactory(pipelineFactory.get());
    resourceLoadCoordinator.SetRendererBackend(rendererBackend.get());

    RenderGraph::GetInstance().LoadRenderGraph(
        GetRuntimeConfig().GetRenderGraphJson(),
        *rendererBackend);

    const UiSettings& uiSettings = GetRuntimeConfig().GetUiSettings();
    const auto& scriptSettings = GetRuntimeConfig().GetScriptSettings();
    try
    {
        GetSubsystems().GetInputSubsystem().ConfigureGameplay(scriptSettings.input);
        GetSubsystems().GetScriptRuntime().Initialize(scriptSettings.enabled,
            scriptSettings.runtimeDirectory, scriptSettings.assembly);
        GetSubsystems().GetScriptRuntime().SetResourceRoot(CommonFunction::GetResourcePath());
    }
    catch (const std::exception& error)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError("Script.StartupFailed", error.what()));
    }
    if (uiSettings.enabled)
    {
        UiSubsystemDesc uiDesc;
        uiDesc.viewportWidth = static_cast<uint32_t>(GetRuntimeConfig().GetWindowSize().x());
        uiDesc.viewportHeight = static_cast<uint32_t>(GetRuntimeConfig().GetWindowSize().y());
        const vk::Format uiFormat = rendererBackend->GetSwapchainImageFormat();
        uiDesc.srgbFramebuffer = uiFormat == vk::Format::eB8G8R8A8Srgb ||
            uiFormat == vk::Format::eR8G8B8A8Srgb;
        uiDesc.assetRoot = GetRuntimeConfig().ResolvePath(uiSettings.assetRoot);
        uiDesc.documentPath = GetRuntimeConfig().ResolvePath(
            (std::filesystem::path(uiSettings.assetRoot) / uiSettings.document).generic_string());
        uiDesc.localizationPath = GetRuntimeConfig().ResolvePath(
            (std::filesystem::path(uiSettings.assetRoot) / uiSettings.localization).generic_string());
        // 场景资产属于资源仓库的 Maps 内容域；不要沿用已废弃的顶层 scenes 目录。
        uiDesc.sceneRoot = GetRuntimeConfig().ResolvePath("Maps");
        uiDesc.defaultLocale = uiSettings.defaultLocale;
        for (const std::string& fontFace : uiSettings.fontFaces)
        {
            uiDesc.fontFaces.emplace_back(fontFace);
        }
        uiDesc.hotReload = uiSettings.hotReload;
        uiDesc.developerUiEnabled = uiSettings.developerUiEnabled;
        if (developerUiMode == DeveloperUiLaunchMode::Enabled)
        {
            uiDesc.developerUiEnabled = true;
        }
        else if (developerUiMode == DeveloperUiLaunchMode::Disabled)
        {
            uiDesc.developerUiEnabled = false;
        }
        uiDesc.developerUiVisible = uiSettings.developerUiVisible;
        uiDesc.previewAdapter =
            &RenderSystem::GetInstance().GetMaterialInstancePreviewAdapter();

        uiSubsystem = std::make_unique<UiSubsystem>();
        auto uiResult = uiSubsystem->Initialize(
            uiDesc,
            window,
            GetSubsystems().GetCommandBus(),
            GetSubsystems().GetDiagnosticsSubsystem());
        if (uiResult.IsFailure())
        {
            return uiResult;
        }

        RenderSystem::GetInstance().SetUiRenderSnapshotQueue(
            &uiSubsystem->GetRenderSnapshotQueue());
        RenderSystem::GetInstance().SetUiOverlayShaderPaths(
            GetRuntimeConfig().ResolvePath("uiOverlay_vert.spv"),
            GetRuntimeConfig().ResolvePath("uiOverlay_frag.spv"));
    }
    worldTransitionCoordinator = std::make_unique<WorldTransitionCoordinator>(
        GetSubsystems().GetWorldManager(),
        resourceLoadCoordinator,
        GetRuntimeConfig().GetWindowAspectRatio(),
        GetRuntimeConfig().GetShadowCascadeCount());
    worldTransitionCoordinator->SetScriptRuntime(&GetSubsystems().GetScriptRuntime());
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> EngineLoop::LoadInitialWorldAndRenderer(
    const std::string& initialSceneOverride)
{
    controller = std::make_unique<Controller>();
    controller->SetRotationSpeed(0.1f);
    worldGraphTransactionCoordinator =
        std::make_unique<WorldGraphTransactionCoordinator>(
            *rendererBackend,
            *shaderCompiler,
            *pipelineFactory,
            *worldTransitionCoordinator,
            GetSubsystems().GetWorldManager(),
            *controller,
            GetRuntimeConfig(),
            GetSubsystems().GetDiagnosticsSubsystem(),
            nullptr);

    RenderSystem& renderSystem = RenderSystem::GetInstance();
    renderSystem.InitializeWorldTransactionResources();

    // Startup uses the same isolated candidate package and transaction
    // publication path as runtime World replacement. The only difference is
    // that the active owner set is initially empty.
    const std::string& initialScenePath =
        initialSceneOverride.empty()
            ? GetRuntimeConfig().GetInitialSceneRelativePath()
            : initialSceneOverride;
    auto initialWorldResult = ExecuteWorldGraphTransaction(
        GetRuntimeConfig().ResolvePath(initialScenePath));
    if (initialWorldResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(initialWorldResult.Error());
    }

    if (uiSubsystem != nullptr)
    {
        const WorldHandle& activeWorld =
            GetSubsystems().GetWorldManager().GetActiveWorldHandle();
        uiSubsystem->NotifyWorldChanged(
            activeWorld.scenePath,
            activeWorld.generation);
    }

    renderSystem.FinalizeInitialRenderObjectInitialization();

    if (useRenderThread)
    {
        renderThread = std::make_unique<RenderThread>();
        renderThread->Start(renderSystem);
        worldGraphTransactionCoordinator->SetRenderThread(
            renderThread.get());
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Render thread started (workerThreadCount=2)");
    }
    else
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Synchronous render mode (workerThreadCount=1)");
    }

    auto consoleResult = GetSubsystems().GetConsoleSubsystem().Initialize(GetSubsystems().GetCommandBus(),
        uiSubsystem ? uiSubsystem->GetEditorCommandRuntime() : nullptr);
    if (consoleResult.IsFailure())
    {
        return consoleResult;
    }
    return RuntimeResult<void>::Success();
}

void EngineLoop::Tick()
{
    PROFILE_SCOPE("Frame");

    // CPU 阶段按 GT Tick 归零；早退帧不 Commit，下一帧 BeginFrame 会丢弃残余累加值。
    cpuFrameProfiler.BeginFrame();

    GetSubsystems().GetConsoleSubsystem().Update();

    ApplyQueuedUiActions();
    UpdateUiInputPolicy();
    if (shouldClose)
    {
        return;
    }

    const EnvironmentUpdateDiagnostics environmentTestDiagnostics =
        RenderSystem::GetInstance().GetEnvironmentUpdateDiagnostics();
    RuntimeTestHooks& runtimeTests = GetSubsystems().GetRuntimeTestHooks();
    runtimeTests.Update(
        GetSubsystems().GetCommandBus(),
        GetSubsystems().GetWorldManager(),
        environmentTestDiagnostics,
        *runtimeValidationServices,
        GetSubsystems().GetDiagnosticsSubsystem());
    PollRenderThreadFatalError();
    if (shouldClose)
    {
        return;
    }

    const WorldHandle activeWorldBeforeCommand =
        GetSubsystems().GetWorldManager().GetActiveWorldHandle();

    RuntimeCommandExecutionResult commandResult = runtimeCommandExecutor->ExecuteQueuedCommands(
        GetSubsystems().GetCommandBus(),
        RenderSystem::GetInstance(),
        GetSubsystems().GetWorldManager(),
        *worldTransitionCoordinator,
        runtimeTests,
        GetRuntimeConfig(),
        GetSubsystems().GetDiagnosticsSubsystem(),
        &GetSubsystems().GetInputSubsystem(),
        &GetSubsystems().GetScriptRuntime(),
        uiSubsystem ? uiSubsystem->GetSceneDocuments() : nullptr);
    commandResult.activeWorldBeforeCommand = activeWorldBeforeCommand;

    ProcessRequestedWorldTransition(commandResult);
    if (const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld())
    {
        const auto rect = uiSubsystem ? uiSubsystem->GetSceneViewportRect() : UiViewportRect{};
        const float aspect = rect.width && rect.height ? float(rect.width) / float(rect.height)
            : GetRuntimeConfig().GetWindowAspectRatio();
        modelPreviewCamera.Synchronize(*world, aspect);
    }
    if (commandResult.loadWorldAttempted &&
        uiSubsystem != nullptr)
    {
        uiSubsystem->NotifyWorldLoadCompleted();
    }

    ProcessShaderRuntimeRequests(commandResult);
    if (shouldClose)
    {
        return;
    }
    ProcessAutomaticShaderReloads();
    if (shouldClose)
    {
        return;
    }

    commandResult.activeWorldAfterCommand =
        GetSubsystems().GetWorldManager().GetActiveWorldHandle();

    if (uiSubsystem != nullptr &&
        (activeWorldBeforeCommand.generation !=
             commandResult.activeWorldAfterCommand.generation ||
         activeWorldBeforeCommand.scenePath !=
             commandResult.activeWorldAfterCommand.scenePath))
    {
        uiSubsystem->NotifyWorldChanged(
            commandResult.activeWorldAfterCommand.scenePath,
            commandResult.activeWorldAfterCommand.generation);
        if (auto* documents = uiSubsystem->GetSceneDocuments())
        {
            std::string error;
            const auto edits = documents->ReplayEdits(
                commandResult.activeWorldAfterCommand.scenePath,
                commandResult.activeWorldAfterCommand.generation, error);
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            // 草稿跨 World 保留；只在换代后重放，不重复入文档，不修改观察相机。
            if (world)
                for (const auto& edit : edits)
                {
                    if (!ApplyEditorActorProperty(*world, edit, error)) break;
                    if (edit.property == EditorActorProperty::CastShadows)
                        RenderSystem::GetInstance().SetCsmSettings(world->GetCsmSettings());
                }
            if (!error.empty())
                GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(error);
        }
    }

    runtimeTests.NotifyCommandResult(
        commandResult,
        GetSubsystems().GetDiagnosticsSubsystem());

    if (exitAfterRuntimeTests)
    {
        const RuntimeTestStatus runtimeTestStatus = runtimeTests.GetRuntimeTestStatus();
        if (runtimeTestStatus == RuntimeTestStatus::Succeeded)
        {
            exitCode = 0;
            shouldClose = true;
        }
        else if (runtimeTestStatus == RuntimeTestStatus::Failed)
        {
            exitCode = 2;
            shouldClose = true;
        }

        if (shouldClose)
        {
            return;
        }
    }

    const float deltaTime = GetSubsystems().GetRuntimeClock().TickDeltaSeconds();
    // UE 用 MaxUndilatedFrameTime/MaxSimulationTimeStep 防止 hitch 一帧推进整段：
    // 游戏/动画只消费上限 0.1 s 的模拟时间，FPS 与资产轮询仍用原始帧间隔。
    constexpr float MaxSimulationDelta = 0.1f;
    const float simulationDelta = std::min(deltaTime, MaxSimulationDelta);

    // input 由两段不连续区间组成（平台事件轮询 + 动作状态更新），其余 UI 策略
    // 与命令执行归入 other；同一阶段的多个区间在累加器里相加。
    {
        CpuFrameStageScope inputStage(cpuFrameProfiler, CpuFrameStage::Input);
        PumpPlatformEvents();
    }
    if (shouldClose)
    {
        return;
    }
    if (rendererBackend->NeedsSwapchainRecreate())
    {
        int drawableWidth = 0;
        int drawableHeight = 0;
        // 经平台层查询，而不是在这里直接碰 SDL：绘制尺寸的含义属于平台层，
        // 而且多窗口时"哪个窗口"必须是一个可传入的身份，不能靠一个隐含的全局窗口。
        window->GetSizeInPixels(drawableWidth, drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0)
        {
            GetSubsystems().GetDiagnosticsSubsystem().ReportError(
                std::string("Cannot query drawable size after swapchain out-of-date: ") +
                SDL_GetError());
            exitCode = 2;
            shouldClose = true;
            return;
        }
        if (drawableWidth > 0 && drawableHeight > 0)
        {
            // RT 已在上一帧归还所有权；SDL resize 事件可晚于 present 的失效结果。
            auto resizeResult = RecreateRendererForWindowResize(
                static_cast<uint32_t>(drawableWidth),
                static_cast<uint32_t>(drawableHeight));
            if (resizeResult.IsFailure())
            {
                GetSubsystems().GetDiagnosticsSubsystem().ReportRuntimeError(
                    "Swapchain out-of-date recovery failed", resizeResult.Error());
                exitCode = 2;
                shouldClose = true;
                return;
            }
        }
    }
    UpdateUiInputPolicy();

    runtimeTests.UpdateEngineLoopTests(
        *runtimeValidationServices,
        GetSubsystems().GetDiagnosticsSubsystem());
    if (shouldClose)
    {
        return;
    }

    {
        CpuFrameStageScope inputStage(cpuFrameProfiler, CpuFrameStage::Input);
        GetSubsystems().GetInputSubsystem().UpdateActionState();
    }

    {
        PROFILE_SCOPE("Update");
        auto& scripts = GetSubsystems().GetScriptRuntime();
        const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
        {
            // scripts 覆盖蓝图资产轮询、自动 Play 判定、编辑控制器与脚本 Tick；
            // DebugDraw 的帧边界清理也随脚本状态一起计入这里。
            CpuFrameStageScope scriptsStage(cpuFrameProfiler, CpuFrameStage::Scripts);
            // 命令先决定 Pause/World，再清理上一完整帧；Console/BeginPlay 的新请求仍保留。
            if (world && !scripts.IsPaused()) world->GetDebugDraw().BeginFrame();
            TickBlueprintAssets();
            // 故障 Stop 后先收回动态模板，再构造程序集候选，避免把上一轮资产收录为常驻模板。
            TickScriptBuild(deltaTime);
            if (world && world->GetGeneration() != autoPlayWorldGeneration &&
                GetRuntimeConfig().GetScriptSettings().autoPlay &&
                !RenderSystem::GetInstance().IsOfflineMode() &&
                std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json")
            {
                // 导出的游戏在正式World提交后进入同一Play入口；每代只启动一次，Stop不会自行重启。
                autoPlayWorldGeneration = world->GetGeneration();
                if (!world->GetScriptBlueprints().empty())
                {
                    std::string error;
                    if (!scripts.Control(*world, ScriptPlayState::Playing, error))
                        GetSubsystems().GetDiagnosticsSubsystem().ReportError("Game auto-play failed: " + error);
                }
            }
            // 离线渲染消费作者场景；模式切换先恢复作者 TRS，避免隐藏 Play 控件后仍继续游戏。
            if (world && RenderSystem::GetInstance().IsOfflineMode() && !scripts.IsEditing())
            {
                std::string error;
                scripts.Control(*world, ScriptPlayState::Editing, error);
            }
            if (scripts.IsEditing())
                controller->Update(simulationDelta, GetSubsystems().GetInputSubsystem().GetActionState());
            scripts.Tick(world, simulationDelta,
                GetSubsystems().GetInputSubsystem().GetActionState(),
                GetSubsystems().GetDiagnosticsSubsystem());
        }
        {
            // 调试绘制时间推进只随脚本编辑模式走，归入 scripts 的第二段区间。
            CpuFrameStageScope scriptsStage(cpuFrameProfiler, CpuFrameStage::Scripts);
            if (world && scripts.IsEditing()) world->GetDebugDraw().AdvanceGameTime(simulationDelta);
        }
        // 普通动画的编辑预览独立于 C# Play；绑定蓝图角色的发丝则消费
        // 同一份 scaled dt。冻结后交给 RT，不让曝光墙钟绕过 Pause/TimeScale。
        const float gameSimulationDelta =
            scripts.ContextTime().at("deltaTime").get<float>();
        const float hairSimulationDelta = scripts.IsEditing()
            ? simulationDelta : gameSimulationDelta;
        RenderSystem::GetInstance().SetSimulationFrameDeltas(
            simulationDelta, gameSimulationDelta, hairSimulationDelta);
        {
            CpuFrameStageScope animationStage(cpuFrameProfiler, CpuFrameStage::Animation);
            // 动画 CPU 蒙皮/求值发生在 World::TickAnimations，毛发 GT 步进在下一段单独计时。
            if (world && !RenderSystem::GetInstance().IsOfflineMode())
                world->TickAnimations(simulationDelta);
        }
        {
            CpuFrameStageScope hairStage(cpuFrameProfiler, CpuFrameStage::HairSimulation);
            if (!RenderSystem::GetInstance().IsOfflineMode())
            {
                // 场景风在游戏线程只读一次，随同一帧的 World 换代整体切换；没有活动
                // World（或场景未作者 wind）时用默认 WorldWind（强度 0），CPU 参考
                // solver 因此保持静止。阵风相位属于渲染线程的 windTimeSeconds，这里
                // 不复制也不另造时钟。
                const VL::WorldWind sceneWind =
                    world ? world->GetWind() : VL::WorldWind{};
                RendererResourceCache::GetInstance().AdvanceTressfxSimulations(
                    hairSimulationDelta, sceneWind);
            }
        }
        {
            CpuFrameStageScope scriptsStage(cpuFrameProfiler, CpuFrameStage::Scripts);
            if (world && !RenderSystem::GetInstance().IsOfflineMode() &&
                std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json" &&
                scripts.PollBlueprintAssetChanges(*world, deltaTime))
            {
                // ACT 内容变化仍经过下一帧正式 World/Graph 事务；不在观察器里修改活动资源或作者草稿。
                RuntimeCommand command;
                command.type = RuntimeCommandType::LoadWorld;
                command.stringValue = world->GetScenePath();
                QueueRuntimeCommand(std::move(command));
                scripts.SetFeedback("Actor asset changed; reloading scene");
            }
        }
    }

    if (uiSubsystem != nullptr)
    {
        // 先在 GT 稳定帧边界处理 editor/preview，再生成 UI 与 World snapshot。
        uiSubsystem->TickMaterialInstanceEditor();
    }
    {
        CpuFrameStageScope uiStage(cpuFrameProfiler, CpuFrameStage::UiViewModel);
        // UI 视图模型包含编辑器快照收集与 SetPerformanceSnapshot；面板自身按 5 Hz
        // 读冻结副本，不在这里重写 DOM。
        UpdateUiViewModel(simulationDelta, commandResult);
    }
    SynchronizeSceneViewport();
    TickEditorThumbnails();

    {
        PROFILE_SCOPE("RenderLoop");
        if (renderThread && renderThread->IsRunning())
        {
            // 这里是多线程模式。GT 只发布世界快照并唤醒渲染线程；
            // RT 随后完成消费、命令录制、提交与 present，GT 无法在不拆分
            // 线程边界的前提下观测这些内部区间，因此它们被 submitPresent 的
            // 等待吸收（契约里明确记录这一归属）。
            {
                CpuFrameStageScope snapshotStage(cpuFrameProfiler, CpuFrameStage::WorldSnapshot);
                RenderSystem::GetInstance().PublishSnapshotFromActiveWorld();
            }
            {
                CpuFrameStageScope recordStage(cpuFrameProfiler, CpuFrameStage::RenderRecord);
                renderThread->SubmitFrame();
            }
            {
                CpuFrameStageScope waitStage(cpuFrameProfiler, CpuFrameStage::SubmitPresent);
                // V1 keeps the GT/RT split deterministic instead of fully async:
                // RT owns frame recording, while GT waits before touching renderer
                // resources again.
                WaitForRenderThreadIdle();
            }
        }
        else
        {
            // 这里是单线程模式。世界快照发布仍单独计时；consume + 命令录制 +
            // submit + present 在 RenderLatestSnapshotOrLastGood 内串行完成，
            // 统一计入 renderRecord，submitPresent 在本模式下没有独立样本（n/a）。
            {
                CpuFrameStageScope snapshotStage(cpuFrameProfiler, CpuFrameStage::WorldSnapshot);
                RenderSystem::GetInstance().PublishSnapshotFromActiveWorld();
            }
            {
                CpuFrameStageScope recordStage(cpuFrameProfiler, CpuFrameStage::RenderRecord);
                RenderSystem::GetInstance().RenderLatestSnapshotOrLastGood();
            }
        }

    }

    // CPU 阶段测量到渲染段结束为止：FPS 标题、JSONL 与调试绘制收尾留在 other。
    // 冻结样本立即并入 GpuPassTimer 持有的唯一快照，随后 perf json/JSONL 与
    // 下一帧的面板读到的是 CPU 阶段 + GPU pass 的同一份组装结果。
    {
        const std::vector<PerformancePassSample>& cpuStages =
            cpuFrameProfiler.CommitFrame();
        RenderSystem::GetInstance().PublishCpuStageSamples(
            cpuStages,
            cpuFrameProfiler.WindowFrameCount());
    }

    {
        PROFILE_SCOPE("FPS");
        GetSubsystems().GetFpsTool().Calculate(deltaTime);
        window->SetTitle(GetSubsystems().GetFpsTool().getTitle());
    }

    TickPerformanceJsonl();
    TickUiSectionStats();

    PROFILE_FRAME();

    // 即使关闭 UI 也推进帧边界；Motion/Console/脚本共用同一生命周期。
    if (const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld())
        world->GetDebugDraw().EndFrame();
}

void EngineLoop::TickPerformanceJsonl()
{
    if (!perfJsonl.IsOpen())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < nextPerfJsonlWrite)
    {
        return;
    }
    // 固定墙钟节奏：渲染变慢时不补写、不批量刷行，agent 看到的时间序列
    // 与真实运行时长一一对应。第一行在启动满 1 秒后写出。
    nextPerfJsonlWrite = now + std::chrono::seconds(1);
    perfJsonl.Append(
        FormatPerformanceJson(RenderSystem::GetInstance().CapturePerformanceSnapshot()));
}

void EngineLoop::TickUiSectionStats()
{
    if (!gpuStatsEnabled || uiSubsystem == nullptr)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < nextUiSectionStats)
    {
        return;
    }
    nextUiSectionStats = now + std::chrono::seconds(1);
    // 诊断计数只在 --gpu-stats 下读取：证明静止帧没有分节重建/面板重建，
    // 交互帧才产生脏集。计数每秒归零，读出的是“每秒次数”。
    const UiSubsystem::RefreshStats stats = uiSubsystem->ConsumeRefreshStats();
    std::cout << "[UI] sections/s=" << stats.sectionPublishes
        << " panelBuilds/s=" << stats.panelBuilds
        << " runtimeViews/s=" << stats.runtimeViewUpdates
        << " bindingDirty/s=" << stats.bindingDirtyWrites
        // 第 3 层诊断：rmlUpdates + skipped ≈ 可见帧数；snapshotPublishes 通常
        // 等于 rmlUpdates，只有 DrawDebug 逐帧变化时才会更多。
        << " rmlUpdates/s=" << stats.rmlUpdates
        << " snapshotPublishes/s=" << stats.snapshotPublishes
        << " skipped/s=" << stats.skippedUpdates;
    for (size_t index = 0; index < UiSectionCount; ++index)
    {
        if (stats.sectionPublishesById[index] == 0)
        {
            continue;
        }
        std::cout << ' ' << UiSectionName(static_cast<UiSectionId>(index)) << ':'
            << stats.sectionPublishesById[index];
    }
    std::cout << std::endl;
}

void EngineLoop::TickScriptBuild(float deltaTime)
{
    if (!scriptBuild) return;
    auto& scripts = GetSubsystems().GetScriptRuntime();
    const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
    bool newCandidate = false;
    if (auto result = scriptBuild->Poll())
    {
        // 编译结果会改变编辑器脚本页的输出/状态文本：绕过 10 Hz 节拍立即重建。
        uiViewModelScriptBuildPending = true;
        scriptBuildOutput = std::move(result->log);
        auto& diagnostics = GetSubsystems().GetDiagnosticsSubsystem();
        for (const auto& entry : ParseScriptBuildDiagnostics(scriptBuildOutput))
        {
            if (entry.severity == LogSeverity::Error)
                diagnostics.ReportError(entry.message, "C# Compiler", entry.location);
            else if (entry.severity == LogSeverity::Warning)
                diagnostics.ReportWarning(entry.message, "C# Compiler", entry.location);
            else diagnostics.ReportInfo(entry.message, "C# Compiler", entry.location);
        }
        if (result->succeeded)
        {
            pendingBuiltAssembly = std::move(result->assembly);
            pendingBuildWorldGeneration = result->worldGeneration;
            newCandidate = true;
            scripts.SetFeedback("Gameplay compiled; validating reload candidate");
        }
        else scripts.SetFeedback(result->cancelled ? "Gameplay build cancelled" : "Gameplay build failed; see Log");
    }
    if (!world) return;
    if (pendingBuiltAssembly && world->GetGeneration() != pendingBuildWorldGeneration)
    {
        pendingBuiltAssembly.reset();
        scripts.SetFeedback("Compiled candidate discarded because the World changed");
    }
    const bool level = !RenderSystem::GetInstance().IsOfflineMode() &&
        std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json";
    if (pendingBuiltAssembly && level && (newCandidate || scripts.IsEditing()))
    {
        // 构建完成不等于加载成功；原有候选类/属性/OnCreate事务仍是唯一程序集发布入口。
        std::string error;
        const bool reloaded = scripts.ReloadAssembly(*world, error, pendingBuiltAssembly->string());
        // 在途 lease 可暂时阻止迁移：保留候选供 Reload 或 Stop 后重试，不每帧重复执行钩子。
        if (reloaded || scripts.IsEditing()) pendingBuiltAssembly.reset();
        auto& diagnostics = GetSubsystems().GetDiagnosticsSubsystem();
        if (reloaded) diagnostics.ReportInfo("Gameplay assembly reloaded", "Script");
        else diagnostics.ReportWarning("Reload rejected: " + error, "Script");
    }
    if (autoCompileScripts && level)
    {
        try
        {
            if (scriptBuild->ObserveChanges(deltaTime))
            {
                // 工作从 idle 进入 running：Busy 状态是可见回馈，立即刷新一次。
                uiViewModelScriptBuildPending = true;
                std::string error;
                if (!scriptBuild->Request(world->GetGeneration(), error)) scripts.SetFeedback(error);
                else scripts.SetFeedback("Source changed; compiling Gameplay...");
            }
        }
        catch (const std::exception& exception) { scripts.SetFeedback(exception.what()); }
    }
}

void EngineLoop::TickBlueprintAssets()
{
    auto& scripts = GetSubsystems().GetScriptRuntime();
    const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
    const auto context = world ? world->GetScriptContext() : nullptr;
    const uint64_t generation = world ? world->GetGeneration() : 0;
    auto& diagnostics = GetSubsystems().GetDiagnosticsSubsystem();
    if (generation != blueprintAssetWorldGeneration || context != blueprintAssetContext.lock() || scripts.IsEditing())
    {
        if (blueprintAssetLoad) blueprintAssetLoad->Cancel();
        blueprintAssetLoad.reset();
        blueprintAssetRequests.clear();
        activeBlueprintAssetRequests.clear();
        for (auto& item : loadedBlueprintAssets)
            if (item.second && world)
            {
                item.second->Release(*world);
                scripts.ForgetBlueprintAsset(*world, item.first);
            }
        loadedBlueprintAssets.clear();
        blueprintAssetContext = context;
        blueprintAssetWorldGeneration = generation;
    }
    if (!world) return;
    for (const auto& request : scripts.TakeAssetRequests(*world))
    {
        if (blueprintAssetLoad && blueprintAssetLoad->Path() == request.at("path"))
            activeBlueprintAssetRequests.push_back(request);
        else blueprintAssetRequests.push_back(request);
    }
    for (const auto& cancellation : scripts.TakeAssetCancellations(*world))
    {
        for (auto* requests : {&blueprintAssetRequests, &activeBlueprintAssetRequests})
            for (auto it = requests->begin(); it != requests->end();)
                if (it->at("id") == cancellation) it = requests->erase(it); else ++it;
    }
    if (blueprintAssetLoad && activeBlueprintAssetRequests.empty()) blueprintAssetLoad->Cancel();
    if (blueprintAssetLoad && blueprintAssetLoad->Step(*world))
    {
        const auto path = blueprintAssetLoad->Path();
        const bool success = blueprintAssetLoad->Error().empty();
        bool accepted = false;
        for (const auto& request : activeBlueprintAssetRequests)
            accepted = scripts.CompleteAssetLoad(*world, request.at("id").get<uint64_t>(), path,
                blueprintAssetLoad->Template(), blueprintAssetLoad->Error()) || accepted;
        if (success && accepted)
        {
            // entry在Begin前已分配；提交之后只移动会话owner，避免成功资源失去卸载入口。
            loadedBlueprintAssets.at(path) = std::move(blueprintAssetLoad);
            diagnostics.ReportInfo("Blueprint asset ready: " + path);
        }
        else
        {
            if (success) blueprintAssetLoad->Release(*world);
            else if (accepted) diagnostics.ReportWarning("Blueprint asset load failed: " + blueprintAssetLoad->Error());
            blueprintAssetLoad.reset();
            loadedBlueprintAssets.erase(path);
        }
        activeBlueprintAssetRequests.clear();
    }
    for (const auto& path : scripts.CollectUnusedBlueprintAssets(*world))
    {
        const auto session = loadedBlueprintAssets.find(path);
        if (session != loadedBlueprintAssets.end() && session->second)
        {
            session->second->Release(*world);
            scripts.ForgetBlueprintAsset(*world, path);
            loadedBlueprintAssets.erase(session);
            diagnostics.ReportInfo("Blueprint asset released: " + path);
        }
    }
    if (!blueprintAssetLoad && !blueprintAssetRequests.empty() && !scripts.IsEditing())
    {
        const auto path = blueprintAssetRequests.front().at("path").get<std::string>();
        for (auto it = blueprintAssetRequests.begin(); it != blueprintAssetRequests.end();)
            if (it->at("path") == path)
            { activeBlueprintAssetRequests.push_back(*it); it = blueprintAssetRequests.erase(it); }
            else ++it;
        if (world->GetScriptTemplates().contains(path))
        {
            for (const auto& request : activeBlueprintAssetRequests)
                scripts.CompleteAssetLoad(*world, request.at("id").get<uint64_t>(), path,
                    world->GetScriptTemplates().at(path));
            activeBlueprintAssetRequests.clear();
        }
        else
        {
            loadedBlueprintAssets.try_emplace(path);
            blueprintAssetLoad = std::make_unique<BlueprintAssetLoadSession>(
                *rendererBackend, *pipelineFactory, RenderGraph::GetInstance());
            try
            {
                blueprintAssetLoad->Begin(*world, activeBlueprintAssetRequests.front().at("id").get<uint64_t>(),
                    path, scripts.Classes());
            }
            catch (const std::exception& exception)
            {
                for (const auto& request : activeBlueprintAssetRequests)
                    scripts.CompleteAssetLoad(*world, request.at("id").get<uint64_t>(), path, nullptr, exception.what());
                blueprintAssetLoad.reset();
                loadedBlueprintAssets.erase(path);
                activeBlueprintAssetRequests.clear();
            }
        }
    }
}

void EngineLoop::ApplyQueuedUiActions()
{
    std::vector<UiAction> actions = GetSubsystems().GetCommandBus().DrainUiActions();
    for (const UiAction& action : actions)
    {
        if (action.type == UiActionType::OpenScriptSource ||
            (action.type == UiActionType::CreateScriptBlueprintAsset && action.script.contains("class")))
        {
            auto& scripts = GetSubsystems().GetScriptRuntime();
            std::string error;
            try
            {
                if (action.type == UiActionType::OpenScriptSource)
                {
                    const auto url = ScriptSourceEditorUrl(CommonFunction::GetResourcePath(),
                        scripts.Classes(), action.stringValue);
                    if (!SDL_OpenURL(url.c_str())) throw std::runtime_error(SDL_GetError());
                }
                else
                {
                    // 从类创建只依赖当前类目录，不依赖场景选中态或预览 World；不执行用户回调。
                    if (!scripts.IsEditing()) throw std::runtime_error("Stop gameplay before creating Blueprints");
                    if (!uiSubsystem || !uiSubsystem->GetSceneDocuments())
                        throw std::runtime_error("Blueprint authoring is unavailable");
                    if (uiSubsystem->GetSceneDocuments()->CreateScriptBlueprintFromClass(action.stringValue,
                        action.script.at("class").get<std::string>(), action.script.at("name").get<std::string>(),
                        scripts.Classes(), error))
                    {
                        scripts.SetFeedback("Blueprint asset created: " + action.stringValue);
                        uiSubsystem->RequestRefreshAssetCatalog();
                        continue;
                    }
                }
            }
            catch (const std::exception& exception) { error = exception.what(); }
            scripts.SetFeedback(error);
            if (!error.empty()) GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(error, "Script");
            continue;
        }
        if (action.type == UiActionType::CompileScripts || action.type == UiActionType::SetAutoCompileScripts)
        {
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            auto& scripts = GetSubsystems().GetScriptRuntime();
            if (!world || world->GetGeneration() != action.actorProperty.worldGeneration ||
                std::filesystem::path(world->GetScenePath()).filename() == "SC_model_preview.json" ||
                RenderSystem::GetInstance().IsOfflineMode() || !scriptBuild || !scriptBuild->Available())
            { scripts.SetFeedback("C# build is unavailable in this context"); continue; }
            if (action.type == UiActionType::SetAutoCompileScripts)
                autoCompileScripts = action.intValue != 0;
            else
            {
                std::string error;
                if (!scriptBuild->Request(world->GetGeneration(), error)) scripts.SetFeedback(error);
                else scripts.SetFeedback("Compiling Gameplay...");
            }
            continue;
        }
        if (action.type == UiActionType::SetScriptTimeScale)
        {
            auto& scripts = GetSubsystems().GetScriptRuntime();
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            std::string error;
            if (!world || world->GetGeneration() != action.actorProperty.worldGeneration)
                error = "The World changed since this time request";
            else if (RenderSystem::GetInstance().IsOfflineMode() ||
                std::filesystem::path(world->GetScenePath()).filename() == "SC_model_preview.json")
                error = "Time Scale requires a realtime level";
            else scripts.SetTimeScale(action.floatValue, error);
            scripts.SetFeedback(error);
            if (!error.empty()) GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(error, "Script");
            continue;
        }
        if (action.type == UiActionType::ControlScripts || action.type == UiActionType::ReloadScripts ||
            action.type == UiActionType::AddScriptBlueprint || action.type == UiActionType::SetScriptProperty ||
            action.type == UiActionType::AddScriptComponent || action.type == UiActionType::RemoveScriptComponent ||
            action.type == UiActionType::SetScriptLogicComponent ||
            action.type == UiActionType::AddScriptLogicComponent || action.type == UiActionType::RemoveScriptLogicComponent ||
            action.type == UiActionType::RenameScriptBlueprint || action.type == UiActionType::DuplicateScriptBlueprint ||
            action.type == UiActionType::DeleteScriptBlueprint || action.type == UiActionType::SetScriptPhysics ||
            action.type == UiActionType::UndoSceneEdit || action.type == UiActionType::RedoSceneEdit ||
            action.type == UiActionType::CreateScriptBlueprintAsset || action.type == UiActionType::SetScriptComponent)
        {
            auto& scripts = GetSubsystems().GetScriptRuntime();
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            std::string error;
            if (!world || world->GetGeneration() != action.actorProperty.worldGeneration)
                error = "The World changed since this script request";
            else if (RenderSystem::GetInstance().IsOfflineMode() ||
                std::filesystem::path(world->GetScenePath()).filename() == "SC_model_preview.json")
                error = "C# Actors require a realtime level";
            else if (action.type == UiActionType::ControlScripts)
            {
                // 同批Stop/Play也必须先移除上一上下文的动态模板，不能把它们当成新场景常驻资产。
                if (scripts.IsEditing()) TickBlueprintAssets();
                scripts.Control(*world, static_cast<ScriptPlayState>(action.intValue), error);
                if (scripts.IsEditing()) TickBlueprintAssets();
            }
            else if (action.type == UiActionType::ReloadScripts)
            {
                if (scripts.IsEditing()) TickBlueprintAssets();
                scripts.ReloadAssembly(*world, error, pendingBuiltAssembly ?
                    pendingBuiltAssembly->string() : std::string());
                pendingBuiltAssembly.reset();
                if (error.empty())
                {
                    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo("Gameplay assembly reloaded", "Script");
                    continue;
                }
            }
            else if (uiSubsystem && uiSubsystem->GetSceneDocuments())
            {
                std::string id;
                if (StageScriptBlueprintAction(*world, scripts, *uiSubsystem->GetSceneDocuments(),
                    action, id, error))
                {
                    if (action.type == UiActionType::CreateScriptBlueprintAsset)
                    {
                        scripts.SetFeedback("Blueprint asset created: " + action.stringValue);
                        uiSubsystem->RequestRefreshAssetCatalog();
                        continue;
                    }
                    pendingScriptBlueprintSelection = id.empty() ? "" : "blueprintInstance|" + id;
                    RuntimeCommand command;
                    command.type = RuntimeCommandType::LoadWorld;
                    command.stringValue = world->GetScenePath();
                    QueueRuntimeCommand(std::move(command));
                }
            }
            scripts.SetFeedback(error);
            if (!error.empty()) GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(error);
            continue;
        }
        if (action.type == UiActionType::SetActorProperty && !GetSubsystems().GetScriptRuntime().IsEditing())
        {
            GetSubsystems().GetScriptRuntime().SetFeedback("Stop gameplay before editing the scene");
            continue;
        }
        if (action.type == UiActionType::ToggleDeveloperUi ||
            action.type == UiActionType::SetLocale)
        {
            if (uiSubsystem != nullptr)
            {
                uiSubsystem->ApplyAction(action);
            }
            continue;
        }

        if (action.type == UiActionType::Quit)
        {
            shouldClose = true;
            continue;
        }

        if (action.type == UiActionType::SetCameraMoveSpeed)
        {
            controller->SetMoveVelocity(action.floatValue);
            GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
                "Camera move speed set to " + std::to_string(action.floatValue) + " cm/s");
            continue;
        }

        if (action.type == UiActionType::NavigateModelPreview)
        {
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            const auto rect = uiSubsystem ? uiSubsystem->GetSceneViewportRect() : UiViewportRect{};
            if (world && rect.width && rect.height)
                modelPreviewCamera.Apply(*world, action.previewCamera,
                    float(rect.width) / float(rect.height), float(rect.width));
            continue;
        }
        if (action.type == UiActionType::TransformBlueprintPreview)
        {
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            if (world && uiSubsystem && uiSubsystem->GetEditorCommandRuntime() &&
                GetSubsystems().GetScriptRuntime().IsEditing() && !RenderSystem::GetInstance().IsOfflineMode())
                for (const auto& document : uiSubsystem->GetEditorCommandRuntime()->GetSnapshot().blueprintDocuments)
                    if (document.assetPath == action.blueprintTransform.assetPath)
                    {
                        ApplyBlueprintPreviewTransform(*world, action.blueprintTransform,
                            document.documentRevision, document.document);
                        break;
                    }
            continue;
        }
        if (action.type == UiActionType::FrameEditorActor)
        {
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            if (world) FrameEditorActor(*world, action.actorProperty.worldGeneration,
                action.actorProperty.identity);
            continue;
        }

        RuntimeCommand command;
        command.sourceText = "ui";
        switch (action.type)
        {
        case UiActionType::PreviewBlueprint:
        {
            // UI 只传地址和 revision；候选几何必须来自 GT 文档 owner 已接受的草稿。
            if (!uiSubsystem || !uiSubsystem->GetEditorCommandRuntime() ||
                !GetSubsystems().GetScriptRuntime().IsEditing() ||
                RenderSystem::GetInstance().IsOfflineMode()) continue;
            const auto& documents = uiSubsystem->GetEditorCommandRuntime()->GetSnapshot().blueprintDocuments;
            bool found = false;
            for (const auto& document : documents)
                if (document.assetPath == action.stringValue &&
                    document.documentRevision == action.blueprintPreviewRevision)
                {
                    command.extraSceneObjects = BuildBlueprintPreviewObjects(action.stringValue,
                        action.blueprintPreviewRevision, document.documentRevision, document.document);
                    found = true;
                    break;
                }
            if (!found) continue;
            command.type = RuntimeCommandType::LoadWorld;
            command.stringValue = ModelPreviewScenePath;
            break;
        }
        case UiActionType::PreviewMaterial:
            command.type = RuntimeCommandType::LoadWorld;
            command.stringValue = ModelPreviewScenePath;
            command.extraSceneObjects = BuildMaterialPreviewObjects(action.stringValue);
            break;
        case UiActionType::PreviewModel:
            command.type = RuntimeCommandType::LoadWorld;
            command.stringValue = ModelPreviewScenePath;
            command.extraSceneObjects = BuildModelPreviewObjects(action.stringValue);
            // 厘米骨骼/TFX 资产按原尺度预览，不继承旧静态模型注入的 43 倍尺度。
            try
            {
                const auto buildPlan =
                    MeshAssetLoader::Prepare(action.stringValue).buildPlan;
                if (buildPlan.assetType == "tressfx")
                {
                    // 带 skinning 的毛发需要源角色姿态，视口不承担；由蓝图 TressFX
                    // 组件承载。这里硬拒绝，不能静默剥掉 skinning 后假装可预览。
                    if (buildPlan.tressfxSimulation.skinning)
                    {
                        GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(
                            "TressFX hair preview skipped: asset declares simulation.skinning "
                            "and is authored by the Blueprint TressFX component: " +
                            action.stringValue);
                        continue;
                    }
                    command.extraSceneObjects[0]["scale"] = {1.0, 1.0, 1.0};
                }
                else if (buildPlan.IsSkeletal())
                    command.extraSceneObjects[0]["scale"] = {1.0, 1.0, 1.0};
            }
            catch (const std::exception& error)
            {
                GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(error.what());
                continue;
            }
            break;
        case UiActionType::PreviewRetarget:
            try
            {
                const auto target = MeshAssetLoader::Prepare(action.retargetPreviewModel).buildPlan;
                if (!target.IsSkeletal() || target.animationSourceModelPath.empty() ||
                    target.retargetProfilePath.empty() ||
                    std::filesystem::weakly_canonical(CommonFunction::Path(target.retargetProfilePath)) !=
                    std::filesystem::weakly_canonical(CommonFunction::Path(action.stringValue)))
                    throw std::invalid_argument("Retarget preview model must reference the opened RT profile");
                const auto source = MeshAssetLoader::Prepare(target.animationSourceModelPath).buildPlan;
                if (!source.IsSkeletal() || !source.animationSourceModelPath.empty())
                    throw std::invalid_argument("Retarget preview source must be a non-retargeted skeletal mesh");
                command.type = RuntimeCommandType::LoadWorld;
                command.stringValue = ModelPreviewScenePath;
                command.extraSceneObjects = BuildRetargetPreviewObjects(
                    action.retargetPreviewModel, target.animationSourceModelPath, action.stringValue);
            }
            catch (const std::exception& error)
            {
                GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(error.what());
                continue;
            }
            break;
        case UiActionType::ControlAnimationPreview:
            command.type = RuntimeCommandType::ControlAnimation;
            command.actorProperty.worldGeneration = action.previewCamera.worldGeneration;
            command.stringValue = action.stringValue;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::ControlScriptAnimation:
            command.type = RuntimeCommandType::ControlScriptAnimation;
            command.actorProperty.worldGeneration = action.previewCamera.worldGeneration;
            command.stringValue = action.stringValue;
            command.intValue = action.intValue;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::ControlDebugDraw:
            command.type = RuntimeCommandType::ControlDebugDraw;
            command.actorProperty.worldGeneration = action.previewCamera.worldGeneration;
            command.debugDrawControl = action.debugDrawControl;
            break;
        case UiActionType::ControlMotionDebug:
            command.type = RuntimeCommandType::ControlMotionDebug;
            command.actorProperty.worldGeneration = action.previewCamera.worldGeneration;
            command.intValue = action.intValue;
            break;
        case UiActionType::ControlMotionReplay:
            command.type = RuntimeCommandType::ControlMotionReplay;
            command.actorProperty.worldGeneration = action.previewCamera.worldGeneration;
            command.intValue = action.intValue;
            break;
        case UiActionType::LoadWorld:
            command.type = RuntimeCommandType::LoadWorld;
            command.stringValue = action.stringValue;
            break;
        case UiActionType::SetDebugViewMode:
            command.type = RuntimeCommandType::SetDebugViewMode;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetToneMappingMode:
            command.type = RuntimeCommandType::SetToneMappingMode;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetCameraExposure:
            command.type = RuntimeCommandType::SetCameraExposure;
            command.stringValue = action.stringValue;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetOfflineMode:
            command.type = RuntimeCommandType::SetOfflineMode;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetOfflineSetting:
            command.type = RuntimeCommandType::SetOfflineSetting;
            command.stringValue = action.stringValue;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetOfflineAov:
            command.type = RuntimeCommandType::SetOfflineAov;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetOfflineExposure:
            command.type = RuntimeCommandType::SetOfflineExposure;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::ControlOfflineSession:
            command.type = RuntimeCommandType::ControlOfflineSession;
            command.intValue = action.intValue;
            break;
        case UiActionType::ExportOffline:
            command.type = RuntimeCommandType::ExportOffline;
            break;
        case UiActionType::SetBloomStrength:
            command.type = RuntimeCommandType::SetBloomParameter;
            command.bloomParameter = BloomParameter::Strength;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetBloomThreshold:
            command.type = RuntimeCommandType::SetBloomParameter;
            command.bloomParameter = BloomParameter::Threshold;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetBloomKnee:
            command.type = RuntimeCommandType::SetBloomParameter;
            command.bloomParameter = BloomParameter::Knee;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetBloomClamp:
            command.type = RuntimeCommandType::SetBloomParameter;
            command.bloomParameter = BloomParameter::Clamp;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmCastShadows:
            command.type = RuntimeCommandType::SetCsmCastShadows;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetCsmDynamicShadowDistance:
            command.type = RuntimeCommandType::SetCsmDynamicShadowDistance;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmDynamicShadowCascades:
            command.type = RuntimeCommandType::SetCsmDynamicShadowCascades;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetCsmCascadeDistributionExponent:
            command.type =
                RuntimeCommandType::SetCsmCascadeDistributionExponent;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmCascadeTransitionFraction:
            command.type =
                RuntimeCommandType::SetCsmCascadeTransitionFraction;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmShadowDistanceFadeoutFraction:
            command.type =
                RuntimeCommandType::SetCsmShadowDistanceFadeoutFraction;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmShadowBias:
            command.type = RuntimeCommandType::SetCsmShadowBias;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmShadowSlopeBias:
            command.type = RuntimeCommandType::SetCsmShadowSlopeBias;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetCsmShadowCascadeBiasDistribution:
            command.type =
                RuntimeCommandType::SetCsmShadowCascadeBiasDistribution;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SaveCsmSettingsToScene:
            command.type = RuntimeCommandType::SaveCsmSettingsToScene;
            break;
        case UiActionType::SetEnvironmentIntensity:
            command.type = RuntimeCommandType::SetEnvironmentIntensity;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetSunIntensity:
            command.type = RuntimeCommandType::SetSunIntensity;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetActorVisible:
            // 身份字符串与 0/1 原样跨越 GT→执行边界；World 查找与校验只在执行端做。
            command.type = RuntimeCommandType::SetActorVisible;
            command.stringValue = action.stringValue;
            command.intValue = action.intValue;
            break;
        case UiActionType::SetActorProperty:
            command.type = RuntimeCommandType::SetActorProperty;
            command.actorProperty = action.actorProperty;
            break;
        case UiActionType::RotatePreviewModel:
        {
            // 旋转的是预览对象，不是相机：毛发物理只对根点世界位置变化产生响应。
            const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
            if (!world) continue;
            const auto rotation = BuildModelPreviewRotationCommand(action.previewRotation,
                world->GetGeneration(), world->GetScenePath());
            if (!rotation) continue;
            command = *rotation;
            break;
        }
        case UiActionType::ResetHairSimulation:
            // 预览世界只有一个毛发对象，空目标即“所有 tressfx 对象”。
            command.type = RuntimeCommandType::ResetHairSimulation;
            command.stringValue.clear();
            break;
        case UiActionType::SetHairSimulationParameter:
            // 视口里的实时覆盖只作用于预览对象；作者值由独立的 Save to Asset 写入。
            command.type = RuntimeCommandType::SetHairSimulationParameter;
            command.stringValue.clear();
            command.hairLiveParameter.objectName = "preview_model";
            command.hairLiveParameter.field = action.previewHairParameter.field;
            command.hairLiveParameter.valueCount = action.previewHairParameter.valueCount;
            for (uint32_t component = 0; component < 3; ++component)
                command.hairLiveParameter.values[component] =
                    action.previewHairParameter.values[component];
            break;
        default:
            continue;
        }
        GetSubsystems().GetCommandBus().Queue(std::move(command));
    }

    // RmlUi 场景列表只提交值语义 selection 请求；在这里统一落到 renderer，
    // 确保选择描边与常规视口拾取走同一个稳定帧边界。
    if (uiSubsystem != nullptr)
    {
        const std::optional<Editor::Selection::MaterialInstanceModelContext>
            modelSelection =
                uiSubsystem->ConsumeMaterialInstanceModelSelectionRequest();
        if (modelSelection.has_value())
        {
            RenderSystem::GetInstance().SetSelectedMaterialInstanceModel(
                modelSelection.value());
        }

        const std::optional<Editor::Selection::MaterialInstanceSelection>
            materialSelection =
                uiSubsystem->ConsumeMaterialInstanceSelectionRequest();
        if (materialSelection.has_value())
        {
            RenderSystem::GetInstance().SetSelectedMaterialInstance(
                materialSelection.value());
        }

        // 选中灯光/相机/环境等非网格 actor 时清掉渲染端描边：编辑器里同一时刻
        // 只能有一个选中对象，不能让大纲与视口各留一个。
        if (uiSubsystem->ConsumeSceneSelectionClearRequest())
        {
            RenderSystem::GetInstance().ClearSelectedMaterialInstance();
        }
    }
}

void EngineLoop::UpdateUiInputPolicy()
{
    const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
    if (world && world->GetCamera())
    {
        // 资产预览已经有自己的 orbit 相机；只有关卡编辑使用独立观察相机。
        const bool editingLevel = uiSubsystem && uiSubsystem->IsDeveloperUiVisible() &&
            GetSubsystems().GetScriptRuntime().IsEditing() &&
            std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json";
        world->SetEditorViewEnabled(editingLevel);
        controller->SetViewTarget(editingLevel ? world->GetRenderCamera() :
            world->GetViewTarget().lock(), world->GetGeneration());
    }
    auto& inputSubsystem = GetSubsystems().GetInputSubsystem();
    if (uiSubsystem == nullptr || !uiSubsystem->IsInitialized())
    {
        inputSubsystem.SetGameKeyboardEnabled(true);
        inputSubsystem.SetGamePointerEnabled(true);
        window->ClearMouseConstraint();
        inputSubsystem.SetRelativeMouseModeAllowed(true);
        return;
    }

    const bool relativeAllowed = uiSubsystem->ShouldUseRelativeMouseModeForGame();
    const bool constrainToViewport =
        inputSubsystem.IsRelativeMouseModeRequested() &&
        uiSubsystem->IsDeveloperUiVisible() &&
        relativeAllowed;
    if (constrainToViewport)
    {
        const UiViewportRect& viewport = uiSubsystem->GetSceneViewportRect();
        window->SetMouseConstraint(
            static_cast<int>(viewport.x),
            static_cast<int>(viewport.y),
            static_cast<int>(viewport.width),
            static_cast<int>(viewport.height));
    }
    else
    {
        window->ClearMouseConstraint();
    }
    inputSubsystem.SetGameKeyboardEnabled(uiSubsystem->ShouldGameReceiveKeyboard());
    inputSubsystem.SetGamePointerEnabled(uiSubsystem->ShouldGameReceivePointer());
    inputSubsystem.SetRelativeMouseModeAllowed(relativeAllowed);
}

void EngineLoop::FillUiViewModelLiveFrame(
    UiViewModelFrameValues& frame,
    UiViewModelBuildMode buildMode,
    const WorldHandle& activeWorld)
{
    frame.activeWorldPath = activeWorld.scenePath;
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    frame.offlineMode = renderSystem.IsOfflineMode();
    if (buildMode == UiViewModelBuildMode::RuntimeHud)
    {
        // 运行时 HUD 每帧只冻结游戏文本；类/性能 JSON 属于编辑器面板，不进入这里。
        frame.hudView = GetSubsystems().GetScriptRuntime().HudView(
            activeWorld.generation);
    }
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    if (!world || frame.offlineMode)
    {
        return;
    }
    // DrawDebug 叠层在沉浸模式仍然显示，因此逐帧冻结相机动图；Motion 的
    // 历史/匹配模型只在开启时构建，关闭时复用一个冻结空视图，避免每个
    // FullEditor 帧都分配一份禁用视图并让相机分节每 10 Hz 误判为变化。
    frame.motionWorldGeneration = world->GetGeneration();
    if (world->IsMotionDebugEnabled())
    {
        frame.motionDebug = world->CaptureMotionDebug();
    }
    else
    {
        frame.motionDebug = DisabledMotionDebugView();
    }
    frame.debugDraw = world->CaptureDebugDraw(*frame.motionDebug);
    if (const auto& camera = world->GetRenderCamera())
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            frame.motionCamera.position[axis] = camera->GetPosition()[axis];
            frame.motionCamera.forward[axis] = camera->GetForwardVector()[axis];
            frame.motionCamera.right[axis] = camera->GetRightVector()[axis];
            frame.motionCamera.up[axis] = camera->GetUpVector()[axis];
        }
        frame.motionCamera.horizontalFovDegrees = camera->GetHFOV();
        frame.motionCamera.clipNear = camera->GetClipNear();
        frame.motionCamera.clipFar = camera->GetClipFar();
    }
}

void EngineLoop::BuildUiScriptsSection(
    UiScriptsSection& section,
    const WorldHandle& activeWorld)
{
    const ScriptRuntimeSubsystem& scriptRuntime = GetSubsystems().GetScriptRuntime();
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();

    section.scripts = scriptRuntime.View(activeWorld.generation);
    section.scripts.buildAvailable = scriptBuild && scriptBuild->Available();
    section.scripts.buildBusy = scriptBuild && scriptBuild->Busy();
    section.scripts.autoCompile = autoCompileScripts;
    section.scripts.buildOutput = scriptBuildOutput;
    if (world && uiSubsystem->GetSceneDocuments())
    {
        // CanUndo 内部做 weakly_canonical，是隐藏状态下最贵的一段；只在脚本节
        // 真正重建时求值（选中/命令/世界换代/编译状态事件），静止帧不重复执行。
        section.scripts.canUndo =
            uiSubsystem->GetSceneDocuments()->CanUndo(world->GetScenePath());
        section.scripts.canRedo =
            uiSubsystem->GetSceneDocuments()->CanUndo(world->GetScenePath(), true);
    }
}

void EngineLoop::BuildUiAnimationSection(
    UiAnimationPreviewSection& section,
    const WorldHandle& activeWorld)
{
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    if (!world)
    {
        return;
    }
    // 动画时间是时间相关值：允许随 10 Hz 刷新节拍变化；暂停且数值不变时
    // 分节发布会按值比较，不提升版本。
    CaptureUiAnimationPreview(section, *world, activeWorld.generation);
}

void EngineLoop::BuildUiPerformanceSection(UiPerformanceSection& section)
{
    // 与 perf json/JSONL 共用同一份冻结快照；这里只做一次值拷贝，
    // 面板随后按自己的 5 Hz 节奏读取，不逐帧接触 RenderSystem。
    section.performance = RenderSystem::GetInstance().CapturePerformanceSnapshot();
    // 脚本时间/计数器同样属于时间序列；它们不进入事件驱动的脚本分节，
    // 否则 Log 的 Scripts 页在静止帧会显示过期的时间。
    section.scriptPerformance =
        GetSubsystems().GetScriptRuntime().PerformanceView();
}

void EngineLoop::BuildUiViewportCameraSection(
    UiViewportCameraSection& section,
    const UiViewModelFrameValues& frameValues)
{
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    section.debugViewMode = renderSystem.GetDebugViewMode();
    section.toneMappingMode = renderSystem.GetToneMappingMode();
    section.motionWorldGeneration = frameValues.motionWorldGeneration;
    section.motionDebug = frameValues.motionDebug;
    section.debugDraw = frameValues.debugDraw;
    if (frameValues.debugDraw)
    {
        section.debugDrawSignature = UiDebugDrawSignature(*frameValues.debugDraw);
    }
    section.motionCamera = frameValues.motionCamera;
    section.cameraMoveSpeed = controller != nullptr ? controller->GetMoveVelocity() : 1000.0f;
    if (world && world->GetCamera())
    {
        section.hasCamera = true;
        section.cameraExposure = world->GetCamera()->GetExposure();
    }
}

void EngineLoop::BuildUiEnvironmentLightingSection(
    UiEnvironmentLightingSection& section,
    const WorldHandle& activeWorld)
{
    (void)activeWorld;
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    section.bloomStrength = renderSystem.GetBloomStrength();
    section.bloomThreshold = renderSystem.GetBloomThreshold();
    section.bloomKnee = renderSystem.GetBloomKnee();
    section.bloomClamp = renderSystem.GetBloomClamp();
    const CsmSettings& csmSettings = renderSystem.GetCsmSettings();
    section.csmCastShadows = csmSettings.castShadows;
    section.csmDynamicShadowDistance = csmSettings.dynamicShadowDistance;
    section.csmDynamicShadowCascades = csmSettings.cascadeCount;
    section.csmCascadeDistributionExponent = csmSettings.cascadeDistributionExponent;
    section.csmCascadeTransitionFraction = csmSettings.cascadeTransitionFraction;
    section.csmShadowDistanceFadeoutFraction = csmSettings.shadowDistanceFadeoutFraction;
    section.csmShadowBias = csmSettings.shadowBias;
    section.csmShadowSlopeBias = csmSettings.shadowSlopeBias;
    section.csmShadowCascadeBiasDistribution = csmSettings.shadowCascadeBiasDistribution;
    section.environmentIntensity = renderSystem.GetEnvironmentIntensity();
    section.sunIntensity = 1.0f;
    if (world && world->GetPrimaryDirectionalLight())
    {
        section.sunIntensity = world->GetPrimaryDirectionalLight()->GetIntensity();
    }
    const EnvironmentUpdateDiagnostics environmentDiagnostics =
        renderSystem.GetEnvironmentUpdateDiagnostics();
    section.environmentActiveGeneration = environmentDiagnostics.progress.activeGeneration;
    section.environmentPendingGeneration = environmentDiagnostics.progress.pendingGeneration;
    section.environmentUpdateStage = ToString(environmentDiagnostics.progress.stage);
    section.environmentCubemapFacesCompleted =
        environmentDiagnostics.progress.cubemapFacesCompleted;
    section.environmentCubemapFaceCount = environmentDiagnostics.progress.cubemapFaceCount;
    section.environmentShUpdatesCompleted = environmentDiagnostics.progress.shUpdatesCompleted;
    section.environmentPrefilterMipsCompleted =
        environmentDiagnostics.progress.prefilterMipsCompleted;
    section.environmentPrefilterMipCount = environmentDiagnostics.progress.prefilterMipCount;
    section.environmentUsesPreviousResources =
        environmentDiagnostics.progress.usingPreviousResources;
    section.environmentGpuTimingSupported = environmentDiagnostics.gpuTiming.supported;
    section.environmentCubemapGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Cubemap).lastMilliseconds;
    section.environmentShGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::SphericalHarmonics).lastMilliseconds;
    section.environmentPrefilterGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Prefilter).lastMilliseconds;
    section.environmentCommitGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Commit).lastMilliseconds;
}

void EngineLoop::BuildUiOfflineSection(
    UiOfflineSection& section,
    const RuntimeCommandExecutionResult& commandResult)
{
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    section.offlineMode = renderSystem.IsOfflineMode();
    section.state = static_cast<int>(renderSystem.GetOfflineState());
    section.samples = renderSystem.GetOfflineSettings().samples;
    section.bounces = renderSystem.GetOfflineSettings().bounces;
    section.lightSamples = renderSystem.GetOfflineSettings().lightSamples;
    section.seed = renderSystem.GetOfflineSettings().seed;
    section.aov = static_cast<int>(renderSystem.GetOfflineAov());
    section.exposure = renderSystem.GetOfflineExposure();
    section.error = renderSystem.GetOfflineError();
    if (const auto result = renderSystem.CaptureOfflineResult())
    {
        section.completedSamples = result->film->completedSamples;
    }
    section.modeFeedback = commandResult.offlineUiModeFeedback;
}

void EngineLoop::BuildUiSceneOutlinerContent()
{
    // 大纲列表是逐帧替换的不可变数组：内容变化时换新指针，静止帧复用旧指针，
    // 这样大纲分节版本不会因为 10 Hz 轮询而推进。
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    auto actors =
        std::make_shared<std::vector<EditorUi::EditorSceneActorSnapshot>>();
    if (world != nullptr)
    {
        *actors = BuildEditorSceneActors(*world);
    }
    uiSceneActors = std::move(actors);

    auto models =
        std::make_shared<std::vector<EditorUi::MaterialInstanceModelSnapshot>>();
    const auto contexts =
        RenderSystem::GetInstance().GetMaterialInstanceModelContexts();
    models->reserve(contexts.size());
    for (const Editor::Selection::MaterialInstanceModelContext& model : contexts)
    {
        EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
        modelSnapshot.worldGeneration = model.worldGeneration;
        modelSnapshot.scenePath = model.scenePath;
        modelSnapshot.objectId = model.objectId;
        modelSnapshot.displayName = model.displayName;
        modelSnapshot.objectIdentity = model.objectIdentity;
        modelSnapshot.materials.reserve(model.materials.size());
        for (const Editor::Selection::MaterialInstanceModelMaterial& material :
             model.materials)
        {
            EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
            materialSnapshot.objectId = material.objectId;
            materialSnapshot.materialSlotIndex = material.materialSlotIndex;
            materialSnapshot.materialSlotName = material.materialSlotName;
            materialSnapshot.materialInstancePath = material.materialInstancePath;
            modelSnapshot.materials.push_back(std::move(materialSnapshot));
        }
        models->push_back(std::move(modelSnapshot));
    }
    uiSceneModels = std::move(models);
}

void EngineLoop::BuildUiViewModelSections(
    UiViewModelSectionUpdate& sections,
    UiSectionMask rebuild,
    const UiViewModelFrameValues& frameValues,
    const WorldHandle& activeWorld,
    const RuntimeCommandExecutionResult& commandResult)
{
    if ((rebuild & UiSectionBit(UiSectionId::Scripts)) != 0)
    {
        UiScriptsSection scripts;
        BuildUiScriptsSection(scripts, activeWorld);
        // 调用方已经确认这一节需要重建；输入版本仍显式递增，供存储层做
        // “同一输入只发布一次”的防御性去重。
        sections.scriptsRevision = ++uiScriptsSectionRevision;
        sections.scripts = std::move(scripts);
    }
    if ((rebuild & UiSectionBit(UiSectionId::Animation)) != 0)
    {
        UiAnimationPreviewSection animation;
        BuildUiAnimationSection(animation, activeWorld);
        sections.animation = std::move(animation);
    }
    if ((rebuild & UiSectionBit(UiSectionId::Performance)) != 0)
    {
        UiPerformanceSection performance;
        BuildUiPerformanceSection(performance);
        sections.performance = std::move(performance);
    }
    if ((rebuild & UiSectionBit(UiSectionId::ViewportCamera)) != 0)
    {
        UiViewportCameraSection viewport;
        BuildUiViewportCameraSection(viewport, frameValues);
        sections.viewportCamera = std::move(viewport);
    }
    if ((rebuild & UiSectionBit(UiSectionId::EnvironmentLighting)) != 0)
    {
        UiEnvironmentLightingSection environment;
        BuildUiEnvironmentLightingSection(environment, activeWorld);
        sections.environmentLighting = std::move(environment);
    }
    if ((rebuild & UiSectionBit(UiSectionId::Offline)) != 0)
    {
        UiOfflineSection offline;
        BuildUiOfflineSection(offline, commandResult);
        sections.offline = std::move(offline);
    }
    if ((rebuild & UiSectionBit(UiSectionId::SceneOutliner)) != 0)
    {
        BuildUiSceneOutlinerContent();
        uiSubsystem->SetSceneActors(uiSceneActors);
        uiSubsystem->SetMaterialInstanceSceneModels(uiSceneModels);
    }
}

void EngineLoop::CaptureUiAnimationPreview(
    UiAnimationPreviewSection& section,
    const World& world,
    uint64_t generation)
{
    for (const auto& entry : world.GetMeshObjects())
    {
        if (!entry.second.animation ||
            entry.second.sceneObjectIdentity != "preview_model")
        {
            continue;
        }
        const auto& player = *entry.second.animation;
        const auto& animation = player.Asset().AnimationAsset();
        if (!uiAnimationPreviewCache)
        {
            uiAnimationPreviewCache = std::make_unique<UiAnimationPreviewCache>();
        }
        UiAnimationPreviewCache& cache = *uiAnimationPreviewCache;
        // 先比较代数：换代后的旧 player 指针不能解引用，也不能作为缓存命中依据。
        const bool cacheHit = cache.worldGeneration == generation &&
            cache.player == &player &&
            cache.clipIds.size() == animation.clips.size();
        if (!cacheHit)
        {
            cache.player = &player;
            cache.worldGeneration = generation;
            cache.clipIds.clear();
            cache.clipIds.reserve(animation.clips.size());
            for (const auto& clip : animation.clips)
            {
                cache.clipIds.push_back(clip.id);
            }
            // schema 只由骨架决定；通道名只由 schema 决定。二者随播放器身份缓存。
            cache.schema = Animation::PreviewFeatureSchema(animation.mesh);
            cache.featureChannels = Animation::FeatureChannelNames(cache.schema);
        }
        section.worldGeneration = generation;
        section.clips = cache.clipIds;
        section.clip = player.SelectedClip();
        section.time = player.Time();
        section.duration = player.Duration();
        section.playing = player.IsPlaying();
        section.jointCount = player.Asset().mesh.joints.size();
        if (!animation.clips.empty())
        {
            const auto& clip = animation.clips.at(player.SelectedClip());
            section.featureChannels = cache.featureChannels;
            if (Animation::CanExtractClipFeatures(
                    clip, cache.schema, player.Time(), player.IsLooping()))
            {
                section.features = Animation::ExtractClipFeatures(
                    animation.mesh, clip, cache.schema, player.Time(), player.IsLooping());
                section.featureStatus = "Root heading | cm, cm/s | pre-IK";
            }
            else
            {
                section.featureStatus = "Incomplete nonloop feature window";
            }
        }
        break;
    }
}

void EngineLoop::UpdateUiViewModel(
    float deltaTime,
    const RuntimeCommandExecutionResult& commandResult)
{
    if (uiSubsystem == nullptr || !uiSubsystem->IsInitialized())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto& worldManager = GetSubsystems().GetWorldManager();
    const WorldHandle& activeWorld = worldManager.GetActiveWorldHandle();
    const ScriptRuntimeSubsystem& scriptRuntime = GetSubsystems().GetScriptRuntime();

    // 可见性决定构建档位：编辑器隐藏时绝不构建编辑器分节，沉浸模式只保留 DrawDebug。
    const UiViewModelBuildMode buildMode = DecideUiViewModelBuildMode(
        true,
        uiSubsystem->IsDeveloperUiVisible(),
        uiSubsystem->IsRuntimeHudVisible());

    UiViewModelFrameValues frame;
    frame.frameIndex = uiFrameIndex++;
    frame.deltaTimeSeconds = deltaTime;
    frame.framesPerSecond = GetSubsystems().GetFpsTool().getFPS();
    frame.cameraMoveSpeed = controller != nullptr ? controller->GetMoveVelocity() : 1000.0f;
    FillUiViewModelLiveFrame(frame, buildMode, activeWorld);
    // 脚本驱动的选中请求在判定脏标记之前处理，选中高亮才能在同一个 GT 帧重建。
    if (!pendingScriptBlueprintSelection.empty() &&
        uiSubsystem->RequestActorSelection(pendingScriptBlueprintSelection))
    {
        pendingScriptBlueprintSelection.clear();
    }

    const ScriptPlayState scriptState = scriptRuntime.IsEditing()
        ? ScriptPlayState::Editing
        : (scriptRuntime.IsPaused() ? ScriptPlayState::Paused : ScriptPlayState::Playing);
    // 脚本编译状态没有逐帧比较大字符串：TickScriptBuild 在工作开始/轮询结果时置脏。
    const bool buildStateDirty = uiViewModelScriptBuildPending;
    uiViewModelScriptBuildPending = false;
    // 事件路径只重脏自己声明的分节；先单独消费掩码，避免短路求值把它留到下一帧。
    const UiSectionMask requested = uiSubsystem->ConsumeViewModelRefreshSections();
    const bool visibilityOrModeChanged = buildMode != uiViewModelBuildMode;
    const bool worldChanged =
        activeWorld.generation != lastUiViewModelWorldGeneration ||
        activeWorld.scenePath != lastUiViewModelWorldPath;
    // World 换代/可见性切换等于所有输入都换了一份：全部节重建。
    const bool rebuildAll = visibilityOrModeChanged || worldChanged ||
        commandResult.worldChanged || commandResult.loadWorldAttempted;
    UiSectionMask forced = requested;
    if (commandResult.offlineUiModeFeedback.has_value() ||
        frame.offlineMode != lastUiViewModelOfflineMode)
    {
        forced |= UiSectionBit(UiSectionId::Offline);
    }
    if (scriptState != lastUiViewModelScriptState || buildStateDirty)
    {
        forced |= UiSectionBit(UiSectionId::Scripts);
    }
    const bool forceRefresh = rebuildAll || forced != 0;
    const bool editorSnapshotDue = ShouldBuildEditorViewModel(
        buildMode, uiViewModelRefresh, now, forceRefresh);

    UiViewModelSectionUpdate sections;
    if (editorSnapshotDue)
    {
        const UiSectionMask rebuild = PlanUiEditorSectionRefresh(
            forced, rebuildAll, uiViewModelSectionFirstRefresh);
        BuildUiViewModelSections(
            sections, rebuild, frame, activeWorld, commandResult);
        uiViewModelRefresh.MarkRefreshed(now);
        uiViewModelSectionFirstRefresh = false;
    }
    uiSubsystem->Update(sections, frame, buildMode, editorSnapshotDue);
    uiViewModelBuildMode = buildMode;
    lastUiViewModelWorldGeneration = activeWorld.generation;
    lastUiViewModelWorldPath = activeWorld.scenePath;
    lastUiViewModelOfflineMode = frame.offlineMode;
    lastUiViewModelScriptState = scriptState;
}

void EngineLoop::SynchronizeSceneViewport()
{
    if (uiSubsystem == nullptr) return;
    const auto& rect = uiSubsystem->GetSceneViewportRect();
    const auto aspect = ViewportAspect(rect);
    auto& renderSystem = RenderSystem::GetInstance();
    renderSystem.SetSceneViewport(rect, uiSubsystem->IsSceneViewportEnabled());
    const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
    if (aspect && world && world->GetRenderCamera())
    {
        // 在 World snapshot 冻结前更新投影；渲染、阴影视锥、离线相机与拾取共享同一相机。
        auto& camera = *world->GetRenderCamera();
        camera.SetProjection(camera.GetHFOV(), *aspect, camera.GetClipNear(), camera.GetClipFar());
    }
}

void EngineLoop::TickEditorThumbnails()
{
    if (!uiSubsystem) return;
    auto* library = uiSubsystem->GetThumbnailLibrary();
    if (!library) return;
    if (thumbnailRequest && !library->IsRenderRequestCurrent(*thumbnailRequest))
    {
        thumbnailRequest.reset();
    }
    if (!thumbnailScheduler.CanAdvance(std::chrono::steady_clock::now(), platformEvents,
        uiSubsystem->CanRenderThumbnails())) return;
    // 上一帧 RT 已归还所有权；这里不调用活动 World 的加载事务，也不重绑活动图。
    if (!thumbnailRequest)
    {
        thumbnailRequest = library->TakeRenderRequest();
        if (!thumbnailRequest) return;
        // Refresh 丢弃会话缓存，但不在 GT 等待旧的 CPU 导入/解码；Begin 会拒绝迟到结果。
        if (thumbnailSession && thumbnailSessionEpoch != thumbnailRequest->epoch) thumbnailSession->Reset();
        thumbnailSessionEpoch = thumbnailRequest->epoch;
        if (!thumbnailSession)
            thumbnailSession = std::make_unique<EditorThumbnailRenderSession>(*rendererBackend, *pipelineFactory);
        thumbnailSession->Begin(thumbnailRequest->path, thumbnailRequest->kind, GetRuntimeConfig().GetRenderGraphJson());
        return;
    }
    if (!thumbnailSession)
    {
        thumbnailSession = std::make_unique<EditorThumbnailRenderSession>(*rendererBackend, *pipelineFactory);
        thumbnailSession->Begin(thumbnailRequest->path, thumbnailRequest->kind, GetRuntimeConfig().GetRenderGraphJson());
        return;
    }
    if (thumbnailSession->Step())
    {
        library->CompleteRenderRequest(*thumbnailRequest, thumbnailSession->Image(), thumbnailSession->Error());
        thumbnailRequest.reset();
    }
}

void EngineLoop::PumpPlatformEvents()
{
    PROFILE_SCOPE("Events");

    platformApplication->PollEvents(platformEvents);
    // 高回报率鼠标不能让相机移动重复触发整棵 UI 命中测试；普通 UI 拖动保留原事件。
    if (GetSubsystems().GetInputSubsystem().IsRelativeMouseModeEnabled())
        CoalesceRelativeMouseMotion(platformEvents);
    for (const PlatformEvent& event : platformEvents)
    {
        if ((event.type == PlatformEventType::KeyDown ||
             event.type == PlatformEventType::KeyUp) &&
            event.key == PlatformKey::Escape)
        {
            if (event.type == PlatformEventType::KeyDown && !event.repeat)
            {
                // Esc 优先取消资产放置，避免同一次按键又请求游戏相对鼠标捕获。
                if (uiSubsystem && uiSubsystem->CancelBlueprintAssetDrag()) continue;
                auto& inputSubsystem = GetSubsystems().GetInputSubsystem();
                inputSubsystem.ToggleRelativeMouseModeRequest();
                const bool captureRequested =
                    inputSubsystem.IsRelativeMouseModeRequested();
                std::string message;
                if (!captureRequested)
                {
                    message = "Mouse capture disabled";
                }
                else if (inputSubsystem.IsRelativeMouseModeEnabled())
                {
                    message = "Mouse capture enabled";
                }
                else
                {
                    message =
                        "Mouse capture requested "
                        "(suspended while UI owns the pointer)";
                }
                message += " (press Esc to toggle)";
                GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(message);
            }
            continue;
        }

        if (event.type == PlatformEventType::MouseButtonDown &&
            event.mouseButton == PlatformMouseButton::Left && uiSubsystem != nullptr &&
            !GetSubsystems().GetInputSubsystem().IsRelativeMouseModeRequested() &&
            !GetSubsystems().GetInputSubsystem().IsRelativeMouseModeEnabled() &&
            uiSubsystem->IsScenePickTarget(event.mouseX, event.mouseY))
        {
            const std::optional<Editor::Selection::ScenePickRequest> pickRequest =
                Editor::Selection::BuildScenePickRequest(
                    event,
                    uiSubsystem->GetViewportWidth(),
                    uiSubsystem->GetViewportHeight());
            if (pickRequest.has_value())
            {
                const auto point = uiSubsystem->GetSceneViewportPoint(pickRequest->mouseX, pickRequest->mouseY);
                if (!point) continue;
                const std::optional<Editor::Selection::MaterialInstanceSelection>
                    selection = RenderSystem::GetInstance().PickMaterialInstanceAt(
                        point->x,
                        point->y,
                        point->renderWidth,
                        point->renderHeight);
                if (selection.has_value())
                {
                    // 高亮是视口选择反馈，不依赖 MI 面板是否成功打开；
                    // 这样即使编辑器文档暂时不可用，点击结果仍立即可见。
                    RenderSystem::GetInstance().SetSelectedMaterialInstance(*selection);
                    const bool selected =
                        uiSubsystem->SelectSceneObjectFromPick(*selection);
                    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
                        selected
                            ? "Selected scene object '" + selection->objectIdentity + "'."
                            : "Selected scene object '" +
                                selection->objectIdentity +
                                "' but the editor could not update its selection.");
                }
                else
                {
                    RenderSystem::GetInstance().ClearSelectedMaterialInstance();
                    uiSubsystem->ClearMaterialInstanceSceneSelection();
                }
            }
        }

        if (uiSubsystem != nullptr && uiSubsystem->HandlePlatformEvent(event))
        {
            continue;
        }

        if (event.type == PlatformEventType::KeyDown)
        {
            continue;
        }

        if (event.type == PlatformEventType::WindowResized)
        {
            // 两种尺寸在这里分流，且必须分流：Vulkan 的 swapchain 要**物理像素**，
            // 而 UI 布局要**逻辑尺寸**。此前两者都拿逻辑尺寸，于是 swapchain 在系统缩放下
            // 会被建成逻辑尺寸 —— 在 100% 缩放上看不出来，所以这条只能在代码里保证。
            uint32_t drawableWidth = event.pixelWidth;
            uint32_t drawableHeight = event.pixelHeight;
            if (drawableWidth == 0 || drawableHeight == 0)
            {
                // 事件没带像素尺寸时（例如合成器未及时更新），回退到像素查询而不是逻辑尺寸，
                // 因为"用错尺寸"比"尺寸略旧"严重得多。
                int queriedWidth = 0;
                int queriedHeight = 0;
                window->GetSizeInPixels(queriedWidth, queriedHeight);
                drawableWidth = static_cast<uint32_t>(queriedWidth > 0 ? queriedWidth : 0);
                drawableHeight = static_cast<uint32_t>(queriedHeight > 0 ? queriedHeight : 0);
            }
            GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
                "Window resized to " +
                std::to_string(event.width) +
                "x" +
                std::to_string(event.height) +
                " (drawable " +
                std::to_string(drawableWidth) +
                "x" +
                std::to_string(drawableHeight) +
                ")");
            auto resizeResult = RecreateRendererForWindowResize(
                static_cast<int>(drawableWidth), static_cast<int>(drawableHeight));
            if (resizeResult.IsFailure())
            {
                GetSubsystems().GetDiagnosticsSubsystem().ReportRuntimeError(
                    "Window resize failed",
                    resizeResult.Error());
                exitCode = 2;
                shouldClose = true;
            }
            continue;
        }

        if (event.type == PlatformEventType::Quit)
        {
            GetSubsystems().GetDiagnosticsSubsystem().ReportInfo("Quit event received");
            shouldClose = true;
        }
    }
}

const RuntimeConfig& EngineLoop::GetRuntimeConfig() const
{
    return gameInstance->GetRuntimeConfig();
}

SubsystemCollection& EngineLoop::GetSubsystems()
{
    return gameInstance->GetSubsystems();
}

const SubsystemCollection& EngineLoop::GetSubsystems() const
{
    return gameInstance->GetSubsystems();
}

void EngineLoop::WaitForRenderThreadIdle()
{
    if (renderThread && renderThread->IsRunning())
    {
        renderThread->WaitUntilIdle();
        PollRenderThreadFatalError();
    }
}

void EngineLoop::PollRenderThreadFatalError()
{
    if (!renderThread || !renderThread->HasFatalError())
    {
        return;
    }

    const std::string message = renderThread->ConsumeFatalError();
    GetSubsystems().GetDiagnosticsSubsystem().ReportError(
        "Render thread failed: " + message);
    exitCode = 2;
    shouldClose = true;
}

void EngineLoop::ProcessShaderRuntimeRequests(
    const RuntimeCommandExecutionResult& commandResult)
{
    shaderReloadRuntime->ProcessManualReload(
        commandResult,
        *this,
        GetSubsystems().GetDiagnosticsSubsystem());
}

void EngineLoop::ProcessAutomaticShaderReloads()
{
    shaderReloadRuntime->ProcessAutomaticReloads(
        *this,
        GetSubsystems().GetDiagnosticsSubsystem());
}

RuntimeResult<WorldHandle>
EngineLoop::ExecuteWorldGraphTransaction(
    const std::string& scenePath,
    const MaterialDefinitionReloadBatch*
        materialDefinitionReload,
    const nlohmann::json& extraSceneObjects)
{
    if (!rendererBackend || !rendererBackendInitialized)
    {
        return RuntimeResult<WorldHandle>::Failure(
            MakeRuntimeError(
                "EngineLoop.WorldTransactionBeforeRendererInit",
                "Cannot prepare a World/RenderGraph transaction before the renderer backend is initialized.",
                scenePath));
    }
    if (!controller || !worldGraphTransactionCoordinator)
    {
        return RuntimeResult<WorldHandle>::Failure(
            MakeRuntimeError(
                "EngineLoop.MissingWorldTransactionCoordinator",
                "Cannot commit a World/RenderGraph transaction before its owners are initialized.",
                scenePath));
    }

    nlohmann::json sceneDocument;
    if (uiSubsystem)
        if (auto* documents = uiSubsystem->GetSceneDocuments())
        {
            std::string error;
            sceneDocument = documents->DraftForWorldLoad(scenePath, error);
            if (!error.empty())
                return RuntimeResult<WorldHandle>::Failure(
                    MakeRuntimeError("Scene.DraftSourceChanged", error, scenePath));
        }
    auto result = worldGraphTransactionCoordinator->Execute(
        scenePath,
        materialDefinitionReload,
        extraSceneObjects,
        sceneDocument);
    if (result.IsSuccess())
    {
        GetSubsystems().GetScriptRuntime().SynchronizeWorld(
            GetSubsystems().GetWorldManager().GetActiveWorld());
        GetSubsystems().GetScriptRuntime().SetFeedback("");
    }
    else GetSubsystems().GetScriptRuntime().SetFeedback(result.Error().message);
    return result;
}

RuntimeResult<WorldHandle>
EngineLoop::ExecuteMaterialDefinitionWorldGraphTransactionForTest(
    const std::set<std::string>& sourceIdentities,
    uint64_t batchId)
{
    MaterialDefinitionReloadBatch batch =
        shaderReloadRuntime
            ->BuildMaterialDefinitionReloadBatchForValidation(
                batchId,
                sourceIdentities);
    return ExecuteWorldGraphTransaction(
        GetSubsystems().GetWorldManager()
            .GetActiveWorldHandle().scenePath,
        &batch);
}

void EngineLoop::SetWorldGraphTransactionTestFaultInjection(
    WorldGraphTransactionTestFaultInjection injection) noexcept
{
    worldGraphTransactionTestFaultInjection = injection;
    if (worldGraphTransactionCoordinator)
    {
        worldGraphTransactionCoordinator->SetFaultInjection(
            injection);
    }
}

void EngineLoop::ProcessRequestedWorldTransition(
    RuntimeCommandExecutionResult& commandResult)
{
    if (!commandResult.worldLoadRequested)
    {
        return;
    }

    auto transactionResult =
        ExecuteWorldGraphTransaction(
            commandResult
                .loadWorldResolvedPath,
            nullptr,
            commandResult.loadWorldExtraSceneObjects);
    if (transactionResult.IsFailure())
    {
        commandResult.loadWorldError =
            transactionResult.Error();
        commandResult.rendererResourcesAfterLoad =
            CaptureRuntimeRendererResourceFingerprint();
        GetSubsystems()
            .GetDiagnosticsSubsystem()
            .ReportRuntimeError(
                "LoadWorld transaction failed",
                transactionResult.Error());
        return;
    }

    commandResult.worldChanged = true;
    commandResult.loadWorldSucceeded = true;
    commandResult.worldRuntimeBindingAttempted =
        true;
    commandResult
        .worldRuntimeBindingSucceeded = true;
    commandResult.loadedWorld =
        transactionResult.Value();
    commandResult.rendererResourcesAfterLoad =
        CaptureRuntimeRendererResourceFingerprint();
}

RuntimeResult<void> EngineLoop::BindActiveWorldRuntimeObjects(const WorldHandle& worldHandle)
{
    if (!worldHandle.IsValid())
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.InvalidWorldHandle",
            "Cannot bind runtime objects because the loaded World handle is invalid."));
    }

    const auto& activeWorld = GetSubsystems().GetWorldManager().GetActiveWorld();
    if (!activeWorld)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.MissingActiveWorld",
            "Cannot bind runtime objects because WorldManager has no active World.",
            worldHandle.scenePath));
    }

    RenderSystem::GetInstance().SetActiveWorld(activeWorld);

    std::shared_ptr<SceneNode> viewTarget = worldHandle.viewTarget.lock();
    if (!viewTarget)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.MissingViewTarget",
            "Loaded world has no controller view target.",
            worldHandle.scenePath));
    }

    if (!controller)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.MissingController",
            "Cannot bind loaded world because the player controller has not been created.",
            worldHandle.scenePath));
    }

    controller->SetViewTarget(viewTarget);
    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Active world bound: " + worldHandle.scenePath);
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> EngineLoop::RecreateRendererForWindowResize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportWarning(
            "Window resize ignored because the drawable size is zero.");
        return RuntimeResult<void>::Success();
    }

    if (rendererBackend == nullptr || !rendererBackendInitialized)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.ResizeBeforeRendererInit",
            "Cannot resize renderer resources before the renderer backend is initialized."));
    }

    bool liveResizeMutationStarted = false;
    try
    {
        WaitForRenderThreadIdle();
        if (shouldClose)
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EngineLoop.RenderThreadFailedBeforeResize",
                "Cannot resize renderer resources because the render thread failed."));
        }

        rendererBackend->WaitIdle();

        RenderGraph& renderGraph = RenderGraph::GetInstance();
        const uint64_t graphOwnerGeneration =
            renderGraph.GetOwnerGeneration();
        auto passMaterialSnapshot = renderGraph.CapturePassMaterialInstances();

        liveResizeMutationStarted = true;
        thumbnailSession.reset();
        renderGraph.Shutdown(*rendererBackend);
        RenderSystem::GetInstance().ReleaseSwapchainDependentResources();
        rendererBackend->RecreateSwapchain(
            static_cast<int>(width),
            static_cast<int>(height));
        if (worldGraphTransactionTestFaultInjection
                .failResizeAfterSwapchainRecreate)
        {
            throw std::runtime_error(
                "Injected resize failure after swapchain recreation");
        }
        renderGraph.LoadRenderGraph(
            GetRuntimeConfig().GetRenderGraphJson(),
            *rendererBackend);
        renderGraph.RestorePassMaterialInstances(passMaterialSnapshot);
        renderGraph.SetOwnerGeneration(
            graphOwnerGeneration);
        RenderSystem::GetInstance().RebuildSwapchainDependentResources();
    }
    catch (const std::exception& exception)
    {
        if (liveResizeMutationStarted)
        {
            shouldClose = true;
        }
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.ResizeRecreateFailed",
            exception.what()));
    }

    return RuntimeResult<void>::Success();
}

RuntimeResult<void> EngineLoop::ReloadRenderGraphResources(VL::RenderGraphReleaseMode releaseMode)
{
    if (rendererBackend == nullptr || !rendererBackendInitialized)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.RenderGraphReloadBeforeRendererInit",
            "Cannot reload render graph resources before the renderer backend is initialized."));
    }

    try
    {
        WaitForRenderThreadIdle();
        if (shouldClose)
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EngineLoop.RenderThreadFailedBeforeRenderGraphReload",
                "Cannot reload render graph resources because the render thread failed."));
        }

        (void)releaseMode;
        RenderGraph& renderGraph =
            RenderGraph::GetInstance();
        auto candidateGraph =
            std::make_shared<
                PreparedRenderGraphState>(
                *rendererBackend);
        candidateGraph->GetGraph()
            .SetTestFaultInjection(
                BuildRenderGraphTestFaultInjection(
                    worldGraphTransactionTestFaultInjection));
        candidateGraph->Load(
            GetRuntimeConfig()
                .GetRenderGraphJson());
        candidateGraph->GetGraph()
            .RestorePassMaterialInstances(
                renderGraph
                    .CapturePassMaterialInstances());
        candidateGraph->GetGraph()
            .SetOwnerGeneration(
                renderGraph
                    .GetOwnerGeneration());
        RenderSystem::GetInstance()
            .PrepareRenderGraphReload(
                candidateGraph->GetGraph());
        if (worldGraphTransactionTestFaultInjection
                .failBeforeCommit)
        {
            throw std::runtime_error(
                "Injected failure after RenderGraph reload prepare");
        }

        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const uint64_t ownerGeneration =
            renderGraph.GetOwnerGeneration();
        const uint64_t lastUsedEpoch =
            retireQueue.GetLastSubmittedEpoch();
        std::vector<RetiredResource>
            retirement;
        retirement.reserve(1);
        retirement.push_back({
            "RenderGraphReload:State",
            ownerGeneration,
            lastUsedEpoch,
            {}});
        PreparedResourceRetirements
            preparedRetirement =
                retireQueue.PrepareRetirements(
                    std::move(retirement));
        renderGraph.SwapState(
            candidateGraph->GetGraph());
        preparedRetirement
            .GetAdditionalResource(0)
            .resource =
            std::static_pointer_cast<void>(
                std::move(candidateGraph));
        retireQueue.CommitPreparedRetirements(
            std::move(preparedRetirement));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.RenderGraphReloadFailed",
            exception.what()));
    }

    return RuntimeResult<void>::Success();
}

RuntimeResult<void> EngineLoop::ReloadRenderGraphResources()
{
    return ReloadRenderGraphResources(VL::RenderGraphReleaseMode::Retire);
}

} // namespace VL
