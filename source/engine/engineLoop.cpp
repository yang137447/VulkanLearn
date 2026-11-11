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
#include "engine/nullEditorHost.h"
#include "engine/modelPreviewRequest.h"
#include "engine/modelPreviewRotation.h"
#include "script/scriptBuildService.h"
#include "script/scriptBuildDiagnostics.h"
#include "pipeline/pipelineFactory.h"
#include "engine/blueprintAssetLoadSession.h"
#include "input/mouseMotionBatch.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeTestHooks.h"
#include "audio/audioRuntime.h"
#include "engine/destructibleRuntime.h"
#include "engine/softBodyRuntime.h"
#include "engine/glassRuntime.h"
#include "engine/projectileRuntime.h"
#include "physics/runtime/fractureImpactWorld.h"
#include "engine/subsystemCollection.h"
#include "engine/testing/runtimeValidationServices.h"
#include "engine/editorHost.h"
#include "engine/perf/enginePerformance.h"
#include "engine/frame/engineFramePhases.h"
#include "engine/script/engineScriptBuild.h"
#include "engine/ui/engineUiActions.h"
#include "engine/window/engineWindowLifecycle.h"
#include "engine/ui/engineUiViewModel.h"
#include "render/frontend/scenePicking.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "material/generator/materialDefinitionReloadBatch.h"
#include "engine/terrainChunkStreamer.h"
#include "platform/platformApplication.h"
#include "platform/platformWindow.h"
#include "profiler.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/renderThread.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "render/resource/rendererResourceLoadCoordinator.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainPageBakeGpu.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "shaderCompiler.h"
#include "terrain/terrainPageManager.h"
#include "engine/worldCandidatePreparer.h"
#include "world/world.h"
#include "ui/uiRenderSnapshot.h"

namespace VL
{

namespace
{

nlohmann::json BuildRuntimeRenderGraphJson(
    const nlohmann::json& source,
    bool includeEditorPasses)
{
    if (includeEditorPasses)
    {
        return source;
    }

    nlohmann::json filtered = source;
    auto passes = filtered.find("passes");
    if (passes == filtered.end() || !passes->is_array())
    {
        return filtered;
    }
    for (auto pass = passes->begin(); pass != passes->end();)
    {
        if (pass->value("type", std::string()) == "editorGrid")
        {
            pass = passes->erase(pass);
        }
        else
        {
            ++pass;
        }
    }
    return filtered;
}

} // namespace

EngineLoop::EngineLoop() = default;
EngineLoop::~EngineLoop()
{
    // The callback captures this; never leave it installed after destruction.
    RenderSystem::GetInstance().SetRendererMutationSafePoint({});
    RenderSystem::GetInstance().SetEditorViewportOverlay(nullptr);
}

RuntimeResult<void> EngineLoop::Init(
    PlatformApplication& platformApplication,
    const RuntimeConfig& runtimeConfig,
    const LaunchOptions& launchOptions,
    std::unique_ptr<IEditorHost> host)
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
    editorHost = host ? std::move(host) : std::make_unique<NullEditorHost>();
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
    SetExitAfterFrames(launchOptions.exitAfterFrames);

    auto inputResult = GetSubsystems().GetInputSubsystem().Initialize(platformWindow);
    if (inputResult.IsFailure())
    {
        return inputResult;
    }

    runtimeRenderGraphJson = BuildRuntimeRenderGraphJson(
        GetRuntimeConfig().GetRenderGraphJson(),
        editorHost->IsEditorFlavor());

    auto runtimeResult = InitializeRuntimeSystems(
        platformWindow,
        platformApplication.GetVulkanExtensions(),
        launchOptions.developerUiMode,
        launchOptions.presentMode,
        launchOptions.forceShaderRebuild);
    if (runtimeResult.IsFailure())
    {
        return runtimeResult;
    }

    // 编辑器组合根在 runtime 系统就绪后接管所有编辑器状态；game 构建传入
    // NullEditorHost，后续调用点不需要特性判断。
    EditorHostInit editorHostInit;
    editorHostInit.window = &platformWindow;
    editorHostInit.runtimeConfig = &runtimeConfig;
    editorHostInit.commandBus = &GetSubsystems().GetCommandBus();
    editorHostInit.diagnostics = &GetSubsystems().GetDiagnosticsSubsystem();
    editorHostInit.worldManager = &GetSubsystems().GetWorldManager();
    editorHostInit.scripts = &GetSubsystems().GetScriptRuntime();
    editorHostInit.renderSystem = &RenderSystem::GetInstance();
    editorHostInit.rendererBackend = rendererBackend.get();
    editorHostInit.pipelineFactory = pipelineFactory.get();
    editorHostInit.ui = uiSubsystem.get();
    editorHostInit.waitForRenderThreadIdle =
        [this]() { WaitForRenderThreadIdle(); };
    auto editorHostResult = editorHost->Initialize(editorHostInit);
    if (editorHostResult.IsFailure())
    {
        return editorHostResult;
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
    if (launchOptions.perfJsonlPath.has_value() || launchOptions.gpuStats)
    {
        // 计时读数只在优化构建里有意义：Debug（-O0）会把数毫秒的一帧放大到
        // 数百毫秒，而页流式吞吐 = 每帧上传页数 × 帧率，于是会被误读成
        // "页加载慢"。这里必须在启动期点明，避免再拿非优化构建的结论做架构决策。
#if !defined(NDEBUG)
        std::cout
            << "[Diagnostics][Warning] This build is not optimized (NDEBUG is "
               "undefined). Frame timings and streaming throughput are not "
               "representative; use a Release build for performance work."
            << std::endl;
#endif
    }
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
        ++completedFrames;

        // --exit-after-frames：烟测/打包验收需要进程自然结束；外部强制终止
        // 不构成 teardown 证据。
        if (exitAfterFrames > 0 && completedFrames >= exitAfterFrames &&
            !shouldClose)
        {
            exitCode = 0;
            shouldClose = true;
        }

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

    // The async render thread owns GPU state until it stops. Stop it before any
    // renderer-dependent teardown (shader reload, fracture, caches, graph) so
    // pipelines/materials/descriptors can never be destroyed under a live frame.
    if (renderThread)
    {
        // Drop the mutation callback first: a late safe-point call must not wait
        // on a worker that is already shutting down.
        RenderSystem::GetInstance().SetRendererMutationSafePoint({});
        renderThread->Stop();
        renderThread.reset();
    }
    if (rendererBackendInitialized && rendererBackend)
    {
        rendererBackend->WaitIdle();
    }
    if (editorHost)
    {
        editorHost->Shutdown();
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
    if (fractureWorld)
    {
        fractureWorld->Clear();
        fractureWorld.reset();
    }
    if (fractureBudget)
    {
        fractureBudget->Reset();
        fractureBudget.reset();
    }
    fractureBudgetGeneration = 0;
    if (audioRuntime)
    {
        audioRuntime->Shutdown();
        audioRuntime.reset();
    }
    if (shaderReloadRuntime)
    {
        shaderReloadRuntime->Shutdown();
    }

    if (rendererBackendInitialized)
    {
        // 碎片池/箭矢网格可能仍被上一帧 command buffer 引用；必须等 WaitIdle
        // 之后再销毁，不能跟随 CPU/物理对象一起提前释放。
        if (destructibleRuntimes)
        {
            destructibleRuntimes->Shutdown();
            destructibleRuntimes.reset();
        }
        if (projectileRuntime)
        {
            if (const auto activeWorld =
                    GetSubsystems().GetWorldManager().GetActiveWorld())
                projectileRuntime->Clear(*activeWorld);
            projectileRuntime->Shutdown();
            projectileRuntime.reset();
        }
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

void EngineLoop::SetExitAfterFrames(int frames)
{
    exitAfterFrames = frames > 0 ? frames : 0;
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
    PresentModePreference presentMode,
    bool forceShaderRebuild)
{
    // Material parameter includes are generated before shader compilation so
    // GLSL sees the latest JSON-driven parameter layout.
    MaterialParameterIncludeGenerator::GenerateAllIncludes();
    std::string shaderFolderPath = GetRuntimeConfig().ResolvePath("shader");
    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Shader folder path: " + shaderFolderPath);
    shaderCompiler = std::make_unique<ShaderCompiler>();
    if (!editorHost->IsEditorFlavor())
    {
        // P4b keeps editor-only shader pairs out of the game build surface;
        // P6a replaces this explicit list with a manifest-derived closure.
        shaderCompiler->SetExcludedShaderNames({"pass/editorGrid"});
    }
    shaderCompiler->StartCompile(
        shaderFolderPath,
        forceShaderRebuild);

    rendererBackend = std::make_unique<RendererBackendVulkan>();
    rendererBackend->Initialize(
        vulkanExtensions, window.GetNativeHandle(), presentMode);
    rendererBackendInitialized = true;
    terrainChunkStreamer = std::make_unique<TerrainChunkStreamer>();
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
        runtimeRenderGraphJson,
        *rendererBackend);
    audioRuntime = std::make_unique<AudioRuntime>(
        GetSubsystems().GetDiagnosticsSubsystem(),
        CommonFunction::GetResourcePath());
    // 气球、玻璃和可见箭矢共用一个 Jolt 冲击世界：同一支箭可以命中任意目标。
    fractureWorld = std::make_unique<Physics::FractureImpactWorld>();
    // 全场景碎片预算也是单实例：玻璃/气球通过它统一降级，World 换代归零。
    fractureBudget = std::make_unique<Physics::Fracture::FractureBudget>();
    destructibleRuntimes = std::make_unique<DestructibleRuntimeRegistry>(
        GetSubsystems().GetDiagnosticsSubsystem());
    destructibleRuntimes->Add(std::make_unique<SoftBodyRuntime>(
        *rendererBackend,
        GetSubsystems().GetDiagnosticsSubsystem(),
        audioRuntime.get(),
        *fractureWorld,
        *fractureBudget,
        *destructibleRuntimes));
    destructibleRuntimes->Add(std::make_unique<GlassRuntime>(
        *rendererBackend,
        GetSubsystems().GetDiagnosticsSubsystem(),
        audioRuntime.get(),
        *fractureWorld,
        *fractureBudget,
        *destructibleRuntimes));
    projectileRuntime = std::make_unique<ProjectileRuntime>(
        *rendererBackend,
        GetSubsystems().GetDiagnosticsSubsystem(),
        *fractureWorld);

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
    worldCandidatePreparer = std::make_unique<WorldCandidatePreparer>(
        GetSubsystems().GetWorldManager(),
        resourceLoadCoordinator,
        GetRuntimeConfig().GetWindowAspectRatio(),
        GetRuntimeConfig().GetShadowCascadeCount());
    worldCandidatePreparer->SetScriptRuntime(&GetSubsystems().GetScriptRuntime());
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
            *worldCandidatePreparer,
            GetSubsystems().GetWorldManager(),
            *controller,
            GetRuntimeConfig(),
            runtimeRenderGraphJson,
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
        // Structural renderer mutations (offline mode, MI preview commit) are
        // still serialized against RT; they ask for this safe point explicitly.
        renderSystem.SetRendererMutationSafePoint(
            [this]() { WaitForRenderThreadIdle(); });
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
        editorHost ? editorHost->ConsoleSink() : nullptr);
    if (consoleResult.IsFailure())
    {
        return consoleResult;
    }
    return RuntimeResult<void>::Success();
}

void EngineLoop::Tick()
{
    EngineFrameContext context;
    context.subsystems = &GetSubsystems();
    context.runtimeCommandExecutor = runtimeCommandExecutor.get();
    context.runtimeValidationServices = runtimeValidationServices.get();
    context.editorHost = editorHost.get();
    context.uiSubsystem = uiSubsystem.get();
    context.modelPreviewCamera = &modelPreviewCamera;
    context.renderThread = renderThread.get();
    context.terrainChunkStreamer = terrainChunkStreamer.get();
    context.rendererBackend = rendererBackend.get();
    context.window = window;
    context.audioRuntime = audioRuntime.get();
    context.destructibleRuntimes = destructibleRuntimes.get();
    context.fractureWorld = fractureWorld.get();
    context.fractureBudget = fractureBudget.get();
    context.projectileRuntime = projectileRuntime.get();
    context.controller = controller.get();
    context.cpuFrameProfiler = &cpuFrameProfiler;
    context.runtimeConfig = &GetRuntimeConfig();
    context.platformEvents = &platformEvents;
    context.autoPlayWorldGeneration = &autoPlayWorldGeneration;
    context.destructibleSessionActive = &destructibleSessionActive;
    context.shouldClose = &shouldClose;
    context.exitCode = &exitCode;
    context.exitAfterRuntimeTests = &exitAfterRuntimeTests;
    context.editorSceneInteractionWasEnabled = &editorSceneInteractionWasEnabled;
    context.lastScriptMaterialState = &lastScriptMaterialState;
    context.lastScriptMaterialStateWorldGeneration =
        &lastScriptMaterialStateWorldGeneration;

    context.uiActions = MakeUiActionContext();
    context.scriptBuild = MakeScriptBuildContext();
    context.windowLifecycle = MakeWindowLifecycleContext();

    context.callbacks.processRequestedWorldTransition =
        [this](RuntimeCommandExecutionResult& result)
    { ProcessRequestedWorldTransition(result); };
    context.callbacks.processShaderRuntimeRequests =
        [this](const RuntimeCommandExecutionResult& result)
    { ProcessShaderRuntimeRequests(result); };
    context.callbacks.processAutomaticShaderReloads =
        [this] { ProcessAutomaticShaderReloads(); };
    context.callbacks.updateUiViewModel =
        [this](float dt, const RuntimeCommandExecutionResult& result)
    { UpdateUiViewModel(dt, result); };
    context.callbacks.synchronizeSceneViewport =
        [this] { SynchronizeSceneViewport(); };
    context.callbacks.queueRuntimeCommand =
        [this](RuntimeCommand command)
    { QueueRuntimeCommand(std::move(command)); };
    context.callbacks.tickPerformanceJsonl =
        [this] { TickPerformanceJsonl(); };
    context.callbacks.tickUiSectionStats =
        [this] { TickUiSectionStats(); };

    VL::TickEngineFrame(context);
}

void EngineLoop::TickPerformanceJsonl()
{
    EnginePerformanceContext context;
    context.perfJsonl = &perfJsonl;
    context.nextPerfJsonlWrite = &nextPerfJsonlWrite;
    context.renderSystem = &RenderSystem::GetInstance();
    VL::TickPerformanceJsonl(context);
}

void EngineLoop::TickUiSectionStats()
{
    EnginePerformanceContext context;
    context.nextUiSectionStats = &nextUiSectionStats;
    context.gpuStatsEnabled = gpuStatsEnabled;
    context.uiSubsystem = uiSubsystem.get();
    VL::TickUiSectionStats(context);
}

void EngineLoop::TickScriptBuild(float deltaTime)
{
    VL::TickEngineScriptBuild(MakeScriptBuildContext(), deltaTime);
}

void EngineLoop::TickBlueprintAssets()
{
    VL::TickEngineBlueprintAssets(MakeScriptBuildContext());
}

void EngineLoop::ApplyQueuedUiActions()
{
    VL::ApplyQueuedEngineUiActions(
        MakeUiActionContext(), MakeScriptBuildContext());
}

void EngineLoop::UpdateUiInputPolicy()
{
    VL::UpdateEngineUiInputPolicy(MakeUiActionContext());
}

EngineUiViewModelContext EngineLoop::MakeUiViewModelContext()
{
    EngineUiViewModelContext context;
    context.worldManager = &GetSubsystems().GetWorldManager();
    context.scriptRuntime = &GetSubsystems().GetScriptRuntime();
    context.scriptBuild = scriptBuild.get();
    context.controller = controller.get();
    context.renderSystem = &RenderSystem::GetInstance();
    context.autoCompileScripts = autoCompileScripts;
    context.scriptBuildOutput = &scriptBuildOutput;
    context.animationPreviewCache = &uiAnimationPreviewCache;
    return context;
}

void EngineLoop::FillUiViewModelLiveFrame(
    UiViewModelFrameValues& frame,
    UiViewModelBuildMode buildMode,
    const WorldHandle& activeWorld)
{
    VL::FillUiViewModelLiveFrame(
        frame, buildMode, activeWorld, MakeUiViewModelContext());
}

void EngineLoop::BuildUiScriptsSection(
    UiScriptsSection& section,
    const WorldHandle& activeWorld)
{
    EngineUiViewModelContext context = MakeUiViewModelContext();
    if (editorHost)
    {
        // 撤销状态仍由编辑器 host 提供；这里只把调用收成一个中性回调，
        // 让分节构建器不依赖 IEditorHost。
        IEditorHost* host = editorHost.get();
        context.canUndo = [host](const std::string& scenePath, bool redo)
        {
            return host->CanUndo(scenePath, redo);
        };
    }
    VL::BuildUiScriptsSection(section, activeWorld, context);
}

void EngineLoop::BuildUiAnimationSection(
    UiAnimationPreviewSection& section,
    const WorldHandle& activeWorld)
{
    VL::BuildUiAnimationSection(section, activeWorld, MakeUiViewModelContext());
}

void EngineLoop::BuildUiPerformanceSection(UiPerformanceSection& section)
{
    VL::BuildUiPerformanceSection(section, MakeUiViewModelContext());
}

void EngineLoop::BuildUiViewportCameraSection(
    UiViewportCameraSection& section,
    const UiViewModelFrameValues& frameValues)
{
    VL::BuildUiViewportCameraSection(
        section, frameValues, MakeUiViewModelContext());
}

void EngineLoop::BuildUiEnvironmentLightingSection(
    UiEnvironmentLightingSection& section,
    const WorldHandle& activeWorld)
{
    VL::BuildUiEnvironmentLightingSection(
        section, activeWorld, MakeUiViewModelContext());
}

void EngineLoop::BuildUiOfflineSection(
    UiOfflineSection& section,
    const RuntimeCommandExecutionResult& commandResult)
{
    VL::BuildUiOfflineSection(
        section, commandResult, MakeUiViewModelContext());
}

void EngineLoop::BuildUiSceneOutlinerContent()
{
    // 大纲/MI 模型快照的构建与所有权都在编辑器 host；runtime 只触发一次发布。
    const std::shared_ptr<World>& world =
        GetSubsystems().GetWorldManager().GetActiveWorld();
    if (editorHost)
        editorHost->BuildUiSections(world.get());
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
    }
}

void EngineLoop::CaptureUiAnimationPreview(
    UiAnimationPreviewSection& section,
    const World& world,
    uint64_t generation)
{
    VL::CaptureUiAnimationPreview(
        section, world, generation, MakeUiViewModelContext());
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
    renderSystem.SetPickingSceneEnabled(
        uiSubsystem->IsDeveloperUiVisible());
    const auto& world = GetSubsystems().GetWorldManager().GetActiveWorld();
    if (aspect && world && world->GetRenderCamera())
    {
        // 在 World snapshot 冻结前更新投影；渲染、阴影视锥、离线相机与拾取共享同一相机。
        auto& camera = *world->GetRenderCamera();
        camera.SetProjection(camera.GetHFOV(), *aspect, camera.GetClipNear(), camera.GetClipFar());
    }
}

void EngineLoop::PumpPlatformEvents()
{
    VL::PumpEnginePlatformEvents(
        MakeUiActionContext(), MakeWindowLifecycleContext());
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
    VL::WaitForEngineRenderThreadIdle(MakeWindowLifecycleContext());
}

void EngineLoop::WaitForRenderFrameSlot()
{
    VL::WaitForEngineRenderFrameSlot(MakeWindowLifecycleContext());
}

void EngineLoop::PollRenderThreadFatalError()
{
    VL::PollEngineRenderThreadFatalError(MakeWindowLifecycleContext());
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
    if (editorHost)
    {
        std::string error;
        sceneDocument = editorHost->DraftSceneDocument(scenePath, error);
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
        const auto& activeWorld =
            GetSubsystems().GetWorldManager().GetActiveWorld();
        // 新 World 的代理和碎片不能继承上一代：先清空共享世界，再让各运行时注册。
        if (fractureWorld) fractureWorld->Clear();
        // 预算账本与碎片同属一个 World 代；只在 generation 真变化时归零，
        // 材质重载等同代事务不能抹掉仍在场上的碎片计数。
        if (activeWorld && fractureBudget &&
            fractureBudgetGeneration != activeWorld->GetGeneration())
        {
            fractureBudget->Reset();
            fractureBudgetGeneration = activeWorld->GetGeneration();
        }
        if (projectileRuntime && activeWorld) projectileRuntime->Clear(*activeWorld);
        if (activeWorld)
        {
            if (destructibleRuntimes)
                destructibleRuntimes->SynchronizeWorld(*activeWorld);
            if (projectileRuntime) projectileRuntime->SynchronizeWorld(*activeWorld);
        }
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

RuntimeResult<void> EngineLoop::RecreateRendererForWindowResize(
    uint32_t width,
    uint32_t height)
{
    return VL::RecreateEngineRendererForWindowResize(
        MakeWindowLifecycleContext(), width, height);
}

RuntimeResult<void> EngineLoop::ReloadRenderGraphResources(
    VL::RenderGraphReleaseMode releaseMode)
{
    return VL::ReloadEngineRenderGraphResources(
        MakeWindowLifecycleContext(), releaseMode);
}

RuntimeResult<void> EngineLoop::ReloadRenderGraphResources()
{
    return ReloadRenderGraphResources(VL::RenderGraphReleaseMode::Retire);
}

EngineScriptBuildContext EngineLoop::MakeScriptBuildContext()
{
    EngineScriptBuildContext context;
    context.subsystems = &GetSubsystems();
    context.scriptBuild = scriptBuild.get();
    context.rendererBackend = rendererBackend.get();
    context.pipelineFactory = pipelineFactory.get();
    context.autoCompileScripts = &autoCompileScripts;
    context.uiViewModelScriptBuildPending = &uiViewModelScriptBuildPending;
    context.scriptBuildOutput = &scriptBuildOutput;
    context.pendingBuiltAssembly = &pendingBuiltAssembly;
    context.pendingBuildWorldGeneration = &pendingBuildWorldGeneration;
    context.blueprintAssetLoad = &blueprintAssetLoad;
    context.blueprintAssetRequests = &blueprintAssetRequests;
    context.activeBlueprintAssetRequests = &activeBlueprintAssetRequests;
    context.loadedBlueprintAssets = &loadedBlueprintAssets;
    context.blueprintAssetContext = &blueprintAssetContext;
    context.blueprintAssetWorldGeneration = &blueprintAssetWorldGeneration;
    return context;
}

EngineUiActionContext EngineLoop::MakeUiActionContext()
{
    EngineUiActionContext context;
    context.subsystems = &GetSubsystems();
    context.editorHost = editorHost.get();
    context.uiSubsystem = uiSubsystem.get();
    context.controller = controller.get();
    context.modelPreviewCamera = &modelPreviewCamera;
    context.scriptBuild = scriptBuild.get();
    context.platformApplication = platformApplication;
    context.window = window;
    context.platformEvents = &platformEvents;
    context.autoCompileScripts = &autoCompileScripts;
    context.pendingBuiltAssembly = &pendingBuiltAssembly;
    context.shouldClose = &shouldClose;
    context.exitCode = &exitCode;
    context.editorSceneInteractionWasEnabled = &editorSceneInteractionWasEnabled;
    return context;
}

EngineWindowLifecycleContext EngineLoop::MakeWindowLifecycleContext()
{
    EngineWindowLifecycleContext context;
    context.subsystems = &GetSubsystems();
    context.rendererBackend = rendererBackend.get();
    context.renderThread = renderThread.get();
    context.editorHost = editorHost.get();
    context.runtimeRenderGraphJson = &runtimeRenderGraphJson;
    context.testFaultInjection = &worldGraphTransactionTestFaultInjection;
    context.rendererBackendInitialized = &rendererBackendInitialized;
    context.shouldClose = &shouldClose;
    context.exitCode = &exitCode;
    return context;
}

} // namespace VL
