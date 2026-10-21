#include "engine/engineLoop.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "SDL3/SDL.h"
#include "controller.h"
#include "engine/gameInstance.h"
#include "engine/modelPreviewRequest.h"
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
#include "animation/skeletalPlayback.h"

namespace VL
{
namespace
{

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

    PumpPlatformEvents();
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

    GetSubsystems().GetInputSubsystem().UpdateActionState();

    {
        PROFILE_SCOPE("Update");
        auto& scripts = GetSubsystems().GetScriptRuntime();
        const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
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
        if (world && scripts.IsEditing()) world->GetDebugDraw().AdvanceGameTime(simulationDelta);
        if (world && !RenderSystem::GetInstance().IsOfflineMode())
            world->TickAnimations(simulationDelta);
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

    if (uiSubsystem != nullptr)
    {
        // 先在 GT 稳定帧边界处理 editor/preview，再生成 UI 与 World snapshot。
        uiSubsystem->TickMaterialInstanceEditor();
    }
    UpdateUiViewModel(simulationDelta, commandResult);
    SynchronizeSceneViewport();
    TickEditorThumbnails();

    {
        PROFILE_SCOPE("RenderLoop");
        if (renderThread && renderThread->IsRunning())
        {
            // 这里是多线程模式
            RenderSystem::GetInstance().PublishSnapshotFromActiveWorld();
            renderThread->SubmitFrame();
            // V1 keeps the GT/RT split deterministic instead of fully async:
            // RT owns frame recording, while GT waits before touching renderer
            // resources again.
            WaitForRenderThreadIdle();
        }
        else
        {
            // 这里是单线程模式
            RenderSystem::GetInstance().Render();
        }

    }

    {
        PROFILE_SCOPE("FPS");
        GetSubsystems().GetFpsTool().Calculate(deltaTime);
        window->SetTitle(GetSubsystems().GetFpsTool().getTitle());
    }

    PROFILE_FRAME();

    // 即使关闭 UI 也推进帧边界；Motion/Console/脚本共用同一生命周期。
    if (const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld())
        world->GetDebugDraw().EndFrame();
}

void EngineLoop::TickScriptBuild(float deltaTime)
{
    if (!scriptBuild) return;
    auto& scripts = GetSubsystems().GetScriptRuntime();
    const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
    bool newCandidate = false;
    if (auto result = scriptBuild->Poll())
    {
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
            // 厘米骨骼资产按原尺度预览，不继承旧静态模型的 43 倍注入尺度。
            try
            {
                if (MeshAssetLoader::Prepare(action.stringValue).buildPlan.IsSkeletal())
                    command.extraSceneObjects[0]["scale"] = {1.0, 1.0, 1.0};
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
        case UiActionType::SetSpeedTreeStrength:
            command.type = RuntimeCommandType::SetSpeedTreeStrength;
            command.floatValue = action.floatValue;
            break;
        case UiActionType::SetSpeedTreeGustingEnabled:
            command.type = RuntimeCommandType::SetSpeedTreeGustingEnabled;
            command.intValue = action.intValue;
            break;
        case UiActionType::ForceSpeedTreeGust:
            command.type = RuntimeCommandType::ForceSpeedTreeGust;
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

void EngineLoop::UpdateUiViewModel(
    float deltaTime,
    const RuntimeCommandExecutionResult& commandResult)
{
    if (uiSubsystem == nullptr || !uiSubsystem->IsInitialized())
    {
        return;
    }

    UiViewModelSnapshot snapshot;
    snapshot.scripts = GetSubsystems().GetScriptRuntime().View(
        GetSubsystems().GetWorldManager().GetActiveWorldHandle().generation);
    snapshot.scripts.buildAvailable = scriptBuild && scriptBuild->Available();
    snapshot.scripts.buildBusy = scriptBuild && scriptBuild->Busy();
    snapshot.scripts.autoCompile = autoCompileScripts;
    snapshot.scripts.buildOutput = scriptBuildOutput;
    const auto& scriptWorld = GetSubsystems().GetWorldManager().GetActiveWorld();
    if (scriptWorld && uiSubsystem->GetSceneDocuments())
    {
        snapshot.scripts.canUndo = uiSubsystem->GetSceneDocuments()->CanUndo(scriptWorld->GetScenePath());
        snapshot.scripts.canRedo = uiSubsystem->GetSceneDocuments()->CanUndo(scriptWorld->GetScenePath(), true);
    }
    snapshot.frameIndex = uiFrameIndex++;
    snapshot.deltaTimeSeconds = deltaTime;
    snapshot.framesPerSecond = GetSubsystems().GetFpsTool().getFPS();
    snapshot.cameraMoveSpeed = controller->GetMoveVelocity();
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    snapshot.debugViewMode = renderSystem.GetDebugViewMode();
    snapshot.toneMappingMode = renderSystem.GetToneMappingMode();
    snapshot.offlineMode = renderSystem.IsOfflineMode();
    snapshot.offlineState = static_cast<int>(renderSystem.GetOfflineState());
    snapshot.offlineSamples = renderSystem.GetOfflineSettings().samples;
    snapshot.offlineBounces = renderSystem.GetOfflineSettings().bounces;
    snapshot.offlineLightSamples = renderSystem.GetOfflineSettings().lightSamples;
    snapshot.offlineSeed = renderSystem.GetOfflineSettings().seed;
    snapshot.offlineAov = static_cast<int>(renderSystem.GetOfflineAov());
    snapshot.offlineExposure = renderSystem.GetOfflineExposure();
    snapshot.offlineError = renderSystem.GetOfflineError();
    snapshot.offlineModeFeedback = commandResult.offlineUiModeFeedback;
    if (const auto result = renderSystem.CaptureOfflineResult())
        snapshot.offlineCompletedSamples = result->film->completedSamples;
    snapshot.bloomStrength = renderSystem.GetBloomStrength();
    snapshot.bloomThreshold = renderSystem.GetBloomThreshold();
    snapshot.bloomKnee = renderSystem.GetBloomKnee();
    snapshot.bloomClamp = renderSystem.GetBloomClamp();
    const CsmSettings& csmSettings = renderSystem.GetCsmSettings();
    snapshot.csmCastShadows = csmSettings.castShadows;
    snapshot.csmDynamicShadowDistance =
        csmSettings.dynamicShadowDistance;
    snapshot.csmDynamicShadowCascades =
        csmSettings.cascadeCount;
    snapshot.csmCascadeDistributionExponent =
        csmSettings.cascadeDistributionExponent;
    snapshot.csmCascadeTransitionFraction =
        csmSettings.cascadeTransitionFraction;
    snapshot.csmShadowDistanceFadeoutFraction =
        csmSettings.shadowDistanceFadeoutFraction;
    snapshot.csmShadowBias = csmSettings.shadowBias;
    snapshot.csmShadowSlopeBias =
        csmSettings.shadowSlopeBias;
    snapshot.csmShadowCascadeBiasDistribution =
        csmSettings.shadowCascadeBiasDistribution;
    snapshot.environmentIntensity = renderSystem.GetEnvironmentIntensity();
    snapshot.sunIntensity = 1.0f;
    const std::shared_ptr<World>& uiActiveWorld =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    if (uiActiveWorld && uiActiveWorld->GetCamera())
    {
        snapshot.hasCamera = true;
        snapshot.cameraExposure = uiActiveWorld->GetCamera()->GetExposure();
    }
    if (uiActiveWorld && uiActiveWorld->GetPrimaryDirectionalLight())
    {
        snapshot.sunIntensity =
            uiActiveWorld->GetPrimaryDirectionalLight()->GetIntensity();
    }
    const EnvironmentUpdateDiagnostics environmentDiagnostics =
        renderSystem.GetEnvironmentUpdateDiagnostics();
    snapshot.environmentActiveGeneration = environmentDiagnostics.progress.activeGeneration;
    snapshot.environmentPendingGeneration = environmentDiagnostics.progress.pendingGeneration;
    snapshot.environmentUpdateStage = ToString(environmentDiagnostics.progress.stage);
    snapshot.environmentCubemapFacesCompleted =
        environmentDiagnostics.progress.cubemapFacesCompleted;
    snapshot.environmentCubemapFaceCount = environmentDiagnostics.progress.cubemapFaceCount;
    snapshot.environmentShUpdatesCompleted = environmentDiagnostics.progress.shUpdatesCompleted;
    snapshot.environmentPrefilterMipsCompleted =
        environmentDiagnostics.progress.prefilterMipsCompleted;
    snapshot.environmentPrefilterMipCount = environmentDiagnostics.progress.prefilterMipCount;
    snapshot.environmentUsesPreviousResources =
        environmentDiagnostics.progress.usingPreviousResources;
    snapshot.environmentGpuTimingSupported = environmentDiagnostics.gpuTiming.supported;
    snapshot.environmentCubemapGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Cubemap).lastMilliseconds;
    snapshot.environmentShGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::SphericalHarmonics).lastMilliseconds;
    snapshot.environmentPrefilterGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Prefilter).lastMilliseconds;
    snapshot.environmentCommitGpuMs = environmentDiagnostics.gpuTiming
        .Get(EnvironmentGpuProduct::Commit).lastMilliseconds;
    snapshot.speedTreeStrength = renderSystem.GetSpeedTreeStrength();
    snapshot.speedTreeGustingEnabled = renderSystem.GetSpeedTreeGustingEnabled();
    snapshot.speedTreeWindProfileCount = renderSystem.GetSpeedTreeWindProfileCount();
    const WorldHandle& activeWorld = GetSubsystems().GetWorldManager().GetActiveWorldHandle();
    snapshot.activeWorldPath = activeWorld.scenePath;
    if (uiActiveWorld && !snapshot.offlineMode)
    {
        snapshot.motionWorldGeneration = uiActiveWorld->GetGeneration();
        snapshot.motionDebug = uiActiveWorld->CaptureMotionDebug();
        snapshot.debugDraw = uiActiveWorld->CaptureDebugDraw(*snapshot.motionDebug);
        if (const auto& camera = uiActiveWorld->GetRenderCamera())
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                snapshot.motionCamera.position[axis] = camera->GetPosition()[axis];
                snapshot.motionCamera.forward[axis] = camera->GetForwardVector()[axis];
                snapshot.motionCamera.right[axis] = camera->GetRightVector()[axis];
                snapshot.motionCamera.up[axis] = camera->GetUpVector()[axis];
            }
            snapshot.motionCamera.horizontalFovDegrees = camera->GetHFOV();
            snapshot.motionCamera.clipNear = camera->GetClipNear();
            snapshot.motionCamera.clipFar = camera->GetClipFar();
        }
    }
    if (const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld())
        for (const auto& entry : world->GetMeshObjects())
            if (entry.second.animation && entry.second.sceneObjectIdentity == "preview_model")
            {
                const auto& player = *entry.second.animation;
                snapshot.animationWorldGeneration = world->GetGeneration();
                for (const auto& clip : player.Asset().clips) snapshot.animationClips.push_back(clip.id);
                snapshot.animationClip = player.SelectedClip();
                snapshot.animationTime = player.Time();
                snapshot.animationDuration = player.Duration();
                snapshot.animationPlaying = player.IsPlaying();
                snapshot.animationJointCount = player.Asset().mesh.joints.size();
                if (!player.Asset().clips.empty())
                {
                    const auto schema = Animation::PreviewFeatureSchema(player.Asset().mesh);
                    const auto& clip = player.Asset().clips.at(player.SelectedClip());
                    snapshot.animationFeatureChannels = Animation::FeatureChannelNames(schema);
                    if (Animation::CanExtractClipFeatures(clip, schema, player.Time(), player.IsLooping()))
                    {
                        snapshot.animationFeatures = Animation::ExtractClipFeatures(
                            player.Asset().mesh, clip, schema, player.Time(), player.IsLooping());
                        snapshot.animationFeatureStatus = "Root heading | cm, cm/s | pre-IK";
                    }
                    else snapshot.animationFeatureStatus = "Incomplete nonloop feature window";
                }
                break;
            }
    uiSubsystem->SetMaterialInstanceSceneModels(
        renderSystem.GetMaterialInstanceModelContexts());
    // 世界大纲的数据源是 active World 本身，而不是只含带 MI 网格的 RenderScene 子集。
    // 在同一个稳定帧边界拍出值语义副本，UI 回调里不再访问 World。
    uiSubsystem->SetSceneActors(uiActiveWorld != nullptr
        ? BuildEditorSceneActors(*uiActiveWorld)
        : std::vector<EditorUi::EditorSceneActorSnapshot>{});
    if (!pendingScriptBlueprintSelection.empty() &&
        uiSubsystem->RequestActorSelection(pendingScriptBlueprintSelection))
        pendingScriptBlueprintSelection.clear();
    uiSubsystem->Update(snapshot);
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
