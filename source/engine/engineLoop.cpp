#include "engine/engineLoop.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "controller.h"
#include "engine/gameInstance.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeTestHooks.h"
#include "engine/subsystemCollection.h"
#include "engine/testing/runtimeValidationServices.h"
#include "editor/selection/materialInstanceSelection.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "material/generator/materialDefinitionReloadBatch.h"
#include "pipeline/pipelineFactory.h"
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
#include "shaderCompiler.h"
#include "world/loading/worldTransitionCoordinator.h"
#include "ui/uiRenderSnapshot.h"

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

    auto worldResult = LoadInitialWorldAndRenderer(
        launchOptions.initialSceneOverride);
    if (worldResult.IsFailure())
    {
        return worldResult;
    }

    window->CenterOnScreen();
    window->Show();
    UpdateUiInputPolicy();
    GetSubsystems().GetRuntimeClock().Reset();
    return RuntimeResult<void>::Success();
}

int EngineLoop::Run()
{
    while (!shouldClose)
    {
        Tick();
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
    if (uiSettings.enabled)
    {
        UiSubsystemDesc uiDesc;
        uiDesc.viewportWidth = static_cast<uint32_t>(GetRuntimeConfig().GetWindowSize().x());
        uiDesc.viewportHeight = static_cast<uint32_t>(GetRuntimeConfig().GetWindowSize().y());
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
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> EngineLoop::LoadInitialWorldAndRenderer(
    const std::string& initialSceneOverride)
{
    controller = std::make_unique<Controller>();
    controller->SetMoveVelocity(10.0f);
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
    RendererResourceLoadCoordinator::GetInstance()
        .SetEyeComputeReloadParticipant(
            &renderSystem.GetEyeComputeReloadParticipant());    RendererResourceLoadCoordinator::GetInstance()
        .SetClothComputeReloadParticipant(
            &renderSystem.GetClothComputeReloadParticipant());

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

    auto consoleResult = GetSubsystems().GetConsoleSubsystem().Initialize(GetSubsystems().GetCommandBus());
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
        GetSubsystems().GetDiagnosticsSubsystem());
    commandResult.activeWorldBeforeCommand = activeWorldBeforeCommand;

    ProcessRequestedWorldTransition(commandResult);
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

    PumpPlatformEvents();
    if (shouldClose)
    {
        return;
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
        controller->Update(deltaTime, GetSubsystems().GetInputSubsystem().GetActionState());
    }

    if (uiSubsystem != nullptr)
    {
        // 先在 GT 稳定帧边界处理 editor/preview，再生成 UI 与 World snapshot。
        uiSubsystem->TickMaterialInstanceEditor();
    }
    UpdateUiViewModel(deltaTime);

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

}

void EngineLoop::ApplyQueuedUiActions()
{
    std::vector<UiAction> actions = GetSubsystems().GetCommandBus().DrainUiActions();
    for (const UiAction& action : actions)
    {
        if (action.type == UiActionType::ToggleRuntimePage ||
            action.type == UiActionType::CloseRuntimePage ||
            action.type == UiActionType::ToggleDeveloperUi ||
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
                "Camera move speed set to " + std::to_string(action.floatValue));
            continue;
        }

        RuntimeCommand command;
        command.sourceText = "ui";
        switch (action.type)
        {
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

    // ImGui 场景列表只提交值语义 selection 请求；在这里统一落到 renderer，
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
    }
}

void EngineLoop::UpdateUiInputPolicy()
{
    auto& inputSubsystem = GetSubsystems().GetInputSubsystem();
    if (uiSubsystem == nullptr || !uiSubsystem->IsInitialized())
    {
        inputSubsystem.SetGameKeyboardEnabled(true);
        inputSubsystem.SetGamePointerEnabled(true);
        inputSubsystem.SetRelativeMouseModeAllowed(true);
        return;
    }

    inputSubsystem.SetGameKeyboardEnabled(uiSubsystem->ShouldGameReceiveKeyboard());
    inputSubsystem.SetGamePointerEnabled(uiSubsystem->ShouldGameReceivePointer());
    inputSubsystem.SetRelativeMouseModeAllowed(
        uiSubsystem->ShouldUseRelativeMouseModeForGame());
}

void EngineLoop::UpdateUiViewModel(float deltaTime)
{
    if (uiSubsystem == nullptr || !uiSubsystem->IsInitialized())
    {
        return;
    }

    UiViewModelSnapshot snapshot;
    snapshot.frameIndex = uiFrameIndex++;
    snapshot.deltaTimeSeconds = deltaTime;
    snapshot.framesPerSecond = GetSubsystems().GetFpsTool().getFPS();
    snapshot.cameraMoveSpeed = controller->GetMoveVelocity();
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    snapshot.debugViewMode = renderSystem.GetDebugViewMode();
    snapshot.toneMappingMode = renderSystem.GetToneMappingMode();
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
    uiSubsystem->SetMaterialInstanceSceneModels(
        renderSystem.GetMaterialInstanceModelContexts());
    uiSubsystem->Update(snapshot);
}

void EngineLoop::PumpPlatformEvents()
{
    PROFILE_SCOPE("Events");

    platformApplication->PollEvents(platformEvents);
    for (const PlatformEvent& event : platformEvents)
    {
        if ((event.type == PlatformEventType::KeyDown ||
             event.type == PlatformEventType::KeyUp) &&
            event.key == PlatformKey::Escape)
        {
            if (event.type == PlatformEventType::KeyDown && !event.repeat)
            {
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

        if (uiSubsystem != nullptr &&
            uiSubsystem->IsScenePickTarget(event.mouseX, event.mouseY))
        {
            const std::optional<Editor::Selection::ScenePickRequest> pickRequest =
                Editor::Selection::BuildScenePickRequest(
                    event,
                    uiSubsystem->GetViewportWidth(),
                    uiSubsystem->GetViewportHeight());
            if (pickRequest.has_value())
            {
                const std::optional<Editor::Selection::MaterialInstanceSelection>
                    selection = RenderSystem::GetInstance().PickMaterialInstanceAt(
                        pickRequest->mouseX,
                        pickRequest->mouseY,
                        pickRequest->viewportWidth,
                        pickRequest->viewportHeight);
                if (selection.has_value())
                {
                    // 高亮是视口选择反馈，不依赖 MI 面板是否成功打开；
                    // 这样即使编辑器文档暂时不可用，点击结果仍立即可见。
                    RenderSystem::GetInstance().SetSelectedMaterialInstance(*selection);
                    const bool opened =
                        uiSubsystem->OpenMaterialInstanceFromSelection(*selection);
                    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
                        opened
                            ? "Selected Material Instance '" +
                                selection->materialInstancePath + "' from '" +
                                selection->objectIdentity + "'."
                            : "Selected scene object '" +
                                selection->objectIdentity +
                                "' but the MI editor could not open its document.");
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
            GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
                "Window resized to " +
                std::to_string(event.width) +
                "x" +
                std::to_string(event.height));
            auto resizeResult = RecreateRendererForWindowResize(event.width, event.height);
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
        materialDefinitionReload)
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

    return worldGraphTransactionCoordinator->Execute(
        scenePath,
        materialDefinitionReload);
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
                .loadWorldResolvedPath);
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
