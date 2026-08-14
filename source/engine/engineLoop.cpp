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
#include "shader/build/contentHash.h"
#include "shader/reload/shaderCompileWorker.h"
#include "shader/reload/shaderFileMonitor.h"
#include "shader/reload/shaderReloadCoordinator.h"
#include "world/loading/worldTransitionCoordinator.h"
#include "ui/uiRenderSnapshot.h"

namespace VL
{
namespace
{

MaterialDefinitionReloadBatch BuildMaterialDefinitionReloadBatch(
    uint64_t batchId,
    const std::set<std::string>& sourceIdentities,
    const std::filesystem::path& shaderRoot)
{
    MaterialDefinitionReloadBatch batch;
    batch.batchId = batchId;
    const std::filesystem::path glslRoot =
        shaderRoot / "glsl";
    for (const std::string& identity :
         sourceIdentities)
    {
        const std::filesystem::path materialPath =
            glslRoot / identity;
        auto candidate =
            MaterialParameterIncludeGenerator::
                BuildGeneratedIncludeContent(
                    materialPath);
        const std::string includeIdentity =
            std::filesystem::relative(
                candidate.outputPath,
                glslRoot)
                .lexically_normal()
                .generic_string();
        if (!batch.includeOverlays.emplace(
                includeIdentity,
                candidate.generatedBytes)
                 .second)
        {
            throw std::runtime_error(
                "Material definition reload batch contains duplicate generated include identity: " +
                includeIdentity);
        }
        batch.changedSources.push_back(identity);
        batch.sourceDigests.emplace(
            identity,
            ContentHasher::HashFile(
                materialPath).ToHex());
        batch.generatedIncludes.push_back(
            std::move(candidate));
    }
    return batch;
}

std::vector<AtomicFileWrite>
BuildGeneratedIncludeWrites(
    const MaterialDefinitionReloadBatch* batch)
{
    std::vector<AtomicFileWrite> writes;
    if (batch == nullptr)
    {
        return writes;
    }
    for (const auto& candidate :
         batch->generatedIncludes)
    {
        const std::vector<uint8_t> bytes(
            candidate.generatedBytes.begin(),
            candidate.generatedBytes.end());
        if (std::filesystem::is_regular_file(
                candidate.outputPath) &&
            ReadBinaryFile(candidate.outputPath) ==
                bytes)
        {
            continue;
        }
        writes.push_back({
            candidate.outputPath,
            bytes});
    }
    return writes;
}

void ValidateMaterialDefinitionSourcesStillCurrent(
    const MaterialDefinitionReloadBatch* batch,
    const std::filesystem::path& shaderRoot)
{
    if (batch == nullptr)
    {
        return;
    }
    const std::filesystem::path glslRoot =
        shaderRoot / "glsl";
    for (const auto& [identity, capturedDigest] :
         batch->sourceDigests)
    {
        const std::filesystem::path sourcePath =
            glslRoot / identity;
        if (!std::filesystem::is_regular_file(
                sourcePath))
        {
            throw std::runtime_error(
                "Material definition source disappeared before commit: " +
                identity);
        }
        const std::string currentDigest =
            ContentHasher::HashFile(
                sourcePath).ToHex();
        if (currentDigest != capturedDigest)
        {
            throw std::runtime_error(
                "Material definition source changed before commit: " +
                identity);
        }
    }
}

RenderGraph::TestFaultInjection
BuildRenderGraphTestFaultInjection(
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

    auto worldResult = LoadInitialWorldAndRenderer();
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
    RuntimeTestHooks& runtimeTests =
        GetSubsystems().GetRuntimeTestHooks();
    const DiagnosticsSubsystem& diagnostics =
        GetSubsystems().GetDiagnosticsSubsystem();
    if (shaderCompileWorker)
    {
        shaderCompileWorker->Shutdown();
        const ShaderCompileWorkerShutdownDiagnostics
            workerDiagnostics =
                shaderCompileWorker
                    ->GetShutdownDiagnostics();
        (void)runtimeTests
            .FinalizeShaderShutdownInflightTestAfterWorkerShutdown(
                *this,
                workerDiagnostics,
                diagnostics);
        pendingAutoReloadSources.clear();
    }
    shaderFileMonitor.reset();

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
    RenderSystem::GetInstance().SetCsmSettings(GetRuntimeConfig().GetCsmSettings());

    pipelineFactory = rendererBackend->CreatePipelineFactory();
    pipelineFactory->SetShaderCompiler(shaderCompiler.get());
    shaderReloadCoordinator =
        std::make_unique<ShaderReloadCoordinator>(
            *shaderCompiler,
            *pipelineFactory);
    RenderSystem::GetInstance().SetShaderReloadCoordinator(
        shaderReloadCoordinator.get());
    shaderFileMonitor = std::make_unique<ShaderFileMonitor>();
    shaderFileMonitor->Initialize(
        shaderCompiler->GetShaderRoot());
    shaderCompileWorker = std::make_unique<ShaderCompileWorker>();
    shaderCompileWorker->Start(*shaderCompiler);
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
        GetRuntimeConfig().GetWindowAspectRatio());
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> EngineLoop::LoadInitialWorldAndRenderer()
{
    // The initial scene is loaded through the World transition boundary so
    // renderer-facing code consumes a stable World generation.
    auto initialWorldResult = worldTransitionCoordinator->LoadInitialWorld(
        GetRuntimeConfig().ResolvePath(GetRuntimeConfig().GetInitialSceneRelativePath()));
    if (initialWorldResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(initialWorldResult.Error());
    }

    controller = std::make_unique<Controller>();
    controller->SetMoveVelocity(10.0f);
    controller->SetRotationSpeed(0.1f);

    auto bindResult = BindActiveWorldRuntimeObjects(initialWorldResult.Value().world);
    if (bindResult.IsFailure())
    {
        return bindResult;
    }

    RenderSystem::GetInstance().InitRenderObject();

    if (useRenderThread)
    {
        renderThread = std::make_unique<RenderThread>();
        renderThread->Start(RenderSystem::GetInstance());
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
    const auto frameStartTime = std::chrono::steady_clock::now();

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
        *this,
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

    UpdateUiViewModel(deltaTime);

    {
        PROFILE_SCOPE("RenderLoop");
        const bool collectFrameSmokeTiming = runtimeTests.ShouldCollectFrameTiming();
        std::chrono::steady_clock::time_point renderLoopStartTime;
        if (collectFrameSmokeTiming)
        {
            renderLoopStartTime = std::chrono::steady_clock::now();
        }

        if (renderThread && renderThread->IsRunning())
        {
            // 这里是多线程模式
            RenderSystem::GetInstance().PublishSnapshotFromActiveWorld();
            renderThread->SubmitFrame();
            // V1 keeps the GT/RT split deterministic instead of fully async:
            // RT owns frame recording, while GT waits before measuring frame
            // time or touching renderer resources again.
            WaitForRenderThreadIdle();
        }
        else
        {
            // 这里是单线程模式
            RenderSystem::GetInstance().Render();
        }

        if (collectFrameSmokeTiming)
        {
            const auto renderLoopEndTime = std::chrono::steady_clock::now();
            const double renderLoopTimeMs =
                std::chrono::duration<double, std::milli>(renderLoopEndTime - renderLoopStartTime).count();
            runtimeTests.RecordFrameRenderLoopTime(renderLoopTimeMs);
        }
    }

    {
        PROFILE_SCOPE("FPS");
        GetSubsystems().GetFpsTool().Calculate(deltaTime);
        window->SetTitle(GetSubsystems().GetFpsTool().getTitle());
    }

    PROFILE_FRAME();

    const auto frameEndTime = std::chrono::steady_clock::now();
    const double frameTimeMs =
        std::chrono::duration<double, std::milli>(frameEndTime - frameStartTime).count();

    runtimeTests.RecordFrameTime(
        frameTimeMs,
        GetSubsystems().GetDiagnosticsSubsystem());
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

        RuntimeCommand command;
        command.sourceText = "ui";
        switch (action.type)
        {
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
}

void EngineLoop::UpdateUiInputPolicy()
{
    auto& inputSubsystem = GetSubsystems().GetInputSubsystem();
    if (uiSubsystem == nullptr || !uiSubsystem->IsInitialized())
    {
        inputSubsystem.SetGameKeyboardEnabled(true);
        inputSubsystem.SetGamePointerEnabled(true);
        if (!inputSubsystem.IsRelativeMouseModeEnabled())
        {
            inputSubsystem.SetRelativeMouseModeEnabled(true);
        }
        return;
    }

    inputSubsystem.SetGameKeyboardEnabled(uiSubsystem->ShouldGameReceiveKeyboard());
    inputSubsystem.SetGamePointerEnabled(uiSubsystem->ShouldGameReceivePointer());
    const bool shouldUseRelativeMouseMode = uiSubsystem->ShouldUseRelativeMouseModeForGame();
    if (inputSubsystem.IsRelativeMouseModeEnabled() != shouldUseRelativeMouseMode)
    {
        inputSubsystem.SetRelativeMouseModeEnabled(shouldUseRelativeMouseMode);
    }
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
    const RenderSystem& renderSystem = RenderSystem::GetInstance();
    snapshot.debugViewMode = renderSystem.GetDebugViewMode();
    snapshot.toneMappingMode = renderSystem.GetToneMappingMode();
    snapshot.bloomStrength = renderSystem.GetBloomStrength();
    snapshot.bloomThreshold = renderSystem.GetBloomThreshold();
    snapshot.bloomKnee = renderSystem.GetBloomKnee();
    snapshot.bloomClamp = renderSystem.GetBloomClamp();
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
    uiSubsystem->Update(snapshot);
}

void EngineLoop::PumpPlatformEvents()
{
    PROFILE_SCOPE("Events");

    platformApplication->PollEvents(platformEvents);
    for (const PlatformEvent& event : platformEvents)
    {
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
            if (GetSubsystems().GetRuntimeTestHooks().ShouldSuppressResizeEvent(
                    event.width,
                    event.height))
            {
                continue;
            }

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
    const DiagnosticsSubsystem& diagnostics =
        GetSubsystems().GetDiagnosticsSubsystem();
    if (commandResult.shaderCacheStatisticsRequested)
    {
        const ShaderBuildManifestSnapshot manifest =
            shaderCompiler->CaptureManifestSnapshot();
        diagnostics.ReportInfo(
            ShaderCompiler::FormatStatistics(
                shaderCompiler->GetLastStatistics()) +
            ", manifestArtifacts=" +
            std::to_string(manifest.artifacts.size()));
    }

    if (!commandResult.shaderReloadRequested)
    {
        return;
    }

    const ShaderReloadScope scope =
        commandResult.shaderReloadScope ==
                RuntimeShaderReloadScope::All
            ? ShaderReloadScope::All
            : ShaderReloadScope::Changed;
    const uint64_t generation =
        nextShaderReloadGeneration++;
    latestManualShaderReloadGeneration = generation;
    const uint64_t worldGeneration =
        GetSubsystems().GetWorldManager()
            .GetActiveWorldHandle().generation;

    try
    {
        ShaderReloadPlan plan =
            shaderReloadCoordinator->CaptureGraphicsPlan(
                scope,
                generation,
                worldGeneration);
        diagnostics.ReportInfo(
            "Shader reload batch " +
            std::to_string(generation) +
            " prepared: changedSources=" +
            std::to_string(plan.changedSources.size()) +
            ", affectedBuilds=" +
            std::to_string(
                plan.builds.size() +
                plan.computeBuilds.size() +
                (plan.uiBuild.has_value() ? 1u : 0u)) +
            ", liveMaterials=" +
            std::to_string(plan.materials.size()));

        ShaderReloadCandidateBatch batch =
            shaderReloadCoordinator->CompileGraphicsCandidates(
                std::move(plan));

        WaitForRenderThreadIdle();
        if (shouldClose)
        {
            return;
        }

        const uint64_t currentWorldGeneration =
            GetSubsystems().GetWorldManager()
                .GetActiveWorldHandle().generation;
        const ShaderReloadCommitStatistics statistics =
            shaderReloadCoordinator->CommitGraphicsCandidates(
                batch,
                currentWorldGeneration);
        RenderSystem::GetInstance()
            .RefreshResolvedSceneAfterShaderReload();

        diagnostics.ReportInfo(
            "Shader reload batch " +
            std::to_string(statistics.generation) +
            ": changedSources=" +
            std::to_string(statistics.changedSourceCount) +
            ", affectedBuilds=" +
            std::to_string(statistics.affectedBuildCount) +
            ", liveMaterials=" +
            std::to_string(statistics.liveMaterialCount) +
            ", compiled=" +
            std::to_string(statistics.compiledBuildCount) +
            ", shaderc=" +
            std::to_string(statistics.shadercInvocations) +
            ", pipelinesCreated=" +
            std::to_string(statistics.pipelinesCreated) +
            ", committed=" +
            std::string(statistics.committed ? "true" : "false") +
            ", retiredPipelines=" +
            std::to_string(statistics.pipelinesRetired) +
            ", retirePending=" +
            std::to_string(
                ResourceRetireQueue::GetInstance()
                    .GetPendingCount()));
        latestManualShaderReloadCommittedGeneration = generation;
    }
    catch (const std::exception& exception)
    {
        latestManualShaderReloadFailedGeneration = generation;
        diagnostics.ReportError(
            "Shader reload batch " +
            std::to_string(generation) +
            " rejected; current pipelines and formal artifacts remain active: " +
            exception.what());
    }

    // A synchronous reload supersedes older asynchronous work. The monitor
    // baseline is intentionally left untouched: a save that raced with the
    // manual capture must still become a stable automatic event.
}

void EngineLoop::ProcessAutomaticShaderReloads()
{
    if (!shaderFileMonitor || !shaderCompileWorker ||
        !shaderCompileWorker->IsRunning())
    {
        return;
    }

    const DiagnosticsSubsystem& diagnostics =
        GetSubsystems().GetDiagnosticsSubsystem();

    const std::optional<ShaderFileMonitor::ChangeBatch> changeBatch =
        shaderFileMonitor->Poll();
    if (changeBatch)
    {
        if (!changeBatch->observedSources.empty())
        {
            ++latestObservedSourceEpoch;
            if (!pendingAutoReloadSources.empty())
            {
                pendingAutoReloadSourceEpoch =
                    latestObservedSourceEpoch;
            }
            diagnostics.ReportInfo(
                "Shader source epoch " +
                std::to_string(latestObservedSourceEpoch) +
                " observed content transitions=" +
                std::to_string(
                    changeBatch->observedSources.size()));
        }

        std::vector<std::string> shaderSources;
        std::vector<std::string> materialDefinitionSources;
        for (const std::string& source :
             changeBatch->changedSources)
        {
            const std::filesystem::path sourcePath(source);
            const bool isMaterialDefinition =
                sourcePath.extension() == ".json" &&
                sourcePath.filename().string().rfind("M_", 0) == 0;
            if (isMaterialDefinition)
            {
                materialDefinitionSources.push_back(source);
            }
            else
            {
                shaderSources.push_back(source);
            }
        }

        if (!shaderSources.empty())
        {
            MergePendingAutomaticShaderSources(
                shaderSources,
                latestObservedSourceEpoch);
            diagnostics.ReportInfo(
                "Shader source epoch " +
                std::to_string(latestObservedSourceEpoch) +
                " accepted stable shader sources=" +
                std::to_string(shaderSources.size()) +
                ", pendingUnion=" +
                std::to_string(
                    pendingAutoReloadSources.size()));
        }

        if (!materialDefinitionSources.empty())
        {
            pendingMaterialDefinitionSources.insert(
                materialDefinitionSources.begin(),
                materialDefinitionSources.end());
            pendingMaterialDefinitionSourceEpoch =
                std::max(
                    pendingMaterialDefinitionSourceEpoch,
                    latestObservedSourceEpoch);
            if (failedPendingMaterialDefinitionSourceEpoch != 0 &&
                pendingMaterialDefinitionSourceEpoch >
                    failedPendingMaterialDefinitionSourceEpoch)
            {
                failedPendingMaterialDefinitionSourceEpoch = 0;
            }
            diagnostics.ReportInfo(
                "Shader source epoch " +
                std::to_string(
                    latestObservedSourceEpoch) +
                " accepted stable material definitions=" +
                std::to_string(
                    materialDefinitionSources.size()) +
                ", pendingM_Union=" +
                std::to_string(
                    pendingMaterialDefinitionSources.size()));
        }
    }

    if (shaderCompileWorker->HasCompletedResult())
    {
        ShaderCompileWorkerResult result =
            shaderCompileWorker->TakeCompletedResult();
        totalAutoReloadShadercInvocations +=
            result.shadercInvocations;
        const uint64_t resultSourceEpoch =
            result.sourceEpoch;
        const bool supersededByObservedSource =
            resultSourceEpoch < latestObservedSourceEpoch;
        const bool supersededByManualReload =
            result.generation <
                latestManualShaderReloadGeneration;
        if (result.generation !=
                inFlightAutoReloadGeneration ||
            supersededByObservedSource ||
            supersededByManualReload)
        {
            latestAutoReloadStaleDiscardGeneration =
                result.generation;
            lastStaleAutoReloadSources =
                result.changedSources;
            MergePendingAutomaticShaderSources(
                result.changedSources,
                latestObservedSourceEpoch);
            diagnostics.ReportInfo(
                "Shader auto reload batch " +
                std::to_string(result.generation) +
                " discarded as stale: capturedSourceEpoch=" +
                std::to_string(resultSourceEpoch) +
                ", latestObservedSourceEpoch=" +
                std::to_string(latestObservedSourceEpoch) +
                ", latestManualGeneration=" +
                    std::to_string(
                    latestManualShaderReloadGeneration) +
                ", discardedSources=" +
                std::to_string(result.changedSources.size()) +
                ", pendingUnion=" +
                std::to_string(pendingAutoReloadSources.size()));
        }
        else if (!result.succeeded)
        {
            latestAutoReloadFailedGeneration =
                result.generation;
            MergePendingAutomaticShaderSources(
                result.changedSources,
                resultSourceEpoch);
            failedPendingAutoReloadSourceEpoch =
                pendingAutoReloadSourceEpoch;
            diagnostics.ReportError(
                "Shader auto reload batch " +
                std::to_string(result.generation) +
                " compile failed; current pipelines remain active: " +
                result.errorMessage);
        }
        else
        {
            try
            {
                // Vulkan pipeline creation and live-reference replacement only
                // happen at the render-thread safe point.
                WaitForRenderThreadIdle();
                if (shouldClose)
                {
                    return;
                }
                const uint64_t currentWorldGeneration =
                    GetSubsystems().GetWorldManager()
                        .GetActiveWorldHandle().generation;
                const ShaderReloadCommitStatistics statistics =
                    shaderReloadCoordinator->CommitGraphicsCandidates(
                        result.batch,
                        currentWorldGeneration);
                latestAutoReloadCommittedGeneration =
                    result.generation;
                latestAutoReloadShadercInvocations =
                    statistics.shadercInvocations;
                lastCommittedAutoReloadSources =
                    result.changedSources;
                RenderSystem::GetInstance()
                    .RefreshResolvedSceneAfterShaderReload();
                diagnostics.ReportInfo(
                    "Shader auto reload batch " +
                    std::to_string(statistics.generation) +
                    ": changedSources=" +
                    std::to_string(statistics.changedSourceCount) +
                    ", affectedBuilds=" +
                    std::to_string(statistics.affectedBuildCount) +
                    ", liveMaterials=" +
                    std::to_string(statistics.liveMaterialCount) +
                    ", compiled=" +
                    std::to_string(statistics.compiledBuildCount) +
                    ", shaderc=" +
                    std::to_string(statistics.shadercInvocations) +
                    ", pipelinesCreated=" +
                    std::to_string(statistics.pipelinesCreated) +
                    ", committed=" +
                    std::string(statistics.committed ? "true" : "false") +
                    ", retiredPipelines=" +
                    std::to_string(statistics.pipelinesRetired) +
                    ", compileMs=" +
                    std::to_string(result.elapsedMilliseconds) +
                    ", retirePending=" +
                    std::to_string(
                        ResourceRetireQueue::GetInstance()
                            .GetPendingCount()) +
                ", sourceEpoch=" +
                std::to_string(resultSourceEpoch) +
                ", pendingUnion=" +
                std::to_string(pendingAutoReloadSources.size()));
            }
            catch (const std::exception& exception)
            {
                latestAutoReloadFailedGeneration =
                    result.generation;
                MergePendingAutomaticShaderSources(
                    result.changedSources,
                    std::max(
                        resultSourceEpoch,
                        latestObservedSourceEpoch));
                failedPendingAutoReloadSourceEpoch =
                    pendingAutoReloadSourceEpoch;
                diagnostics.ReportError(
                    "Shader auto reload batch " +
                    std::to_string(result.generation) +
                    " rejected; current pipelines and formal artifacts remain active: " +
                    exception.what());
            }
        }

        inFlightAutoReloadGeneration = 0;
        inFlightAutoReloadSourceEpoch = 0;
    }

    ProcessPendingMaterialDefinitionReload();
    SubmitPendingAutomaticShaderReload();
}

void EngineLoop::MergePendingAutomaticShaderSources(
    const std::vector<std::string>& sourceIdentities,
    uint64_t sourceEpoch)
{
    pendingAutoReloadSources.insert(
        sourceIdentities.begin(),
        sourceIdentities.end());
    pendingAutoReloadSourceEpoch =
        std::max(
            pendingAutoReloadSourceEpoch,
            sourceEpoch);
    if (failedPendingAutoReloadSourceEpoch != 0 &&
        pendingAutoReloadSourceEpoch >
            failedPendingAutoReloadSourceEpoch)
    {
        failedPendingAutoReloadSourceEpoch = 0;
    }
}

void EngineLoop::SubmitPendingAutomaticShaderReload()
{
    if (pendingAutoReloadSources.empty() ||
        !shaderCompileWorker->IsIdle() ||
        shaderFileMonitor->HasUnstableSourceChanges() ||
        failedPendingAutoReloadSourceEpoch ==
            pendingAutoReloadSourceEpoch)
    {
        return;
    }

    const DiagnosticsSubsystem& diagnostics =
        GetSubsystems().GetDiagnosticsSubsystem();
    const std::vector<std::string> shaderSources(
        pendingAutoReloadSources.begin(),
        pendingAutoReloadSources.end());
    const uint64_t generation =
        nextShaderReloadGeneration++;
    const uint64_t worldGeneration =
        GetSubsystems().GetWorldManager()
            .GetActiveWorldHandle().generation;
    try
    {
        ShaderReloadPlan plan =
            shaderReloadCoordinator->CaptureGraphicsPlanForSources(
                shaderSources,
                generation,
                worldGeneration);
        plan.sourceEpoch =
            pendingAutoReloadSourceEpoch;
        diagnostics.ReportInfo(
            "Shader auto reload batch " +
            std::to_string(generation) +
            " prepared: sourceEpoch=" +
            std::to_string(plan.sourceEpoch) +
            ", changedSources=" +
            std::to_string(plan.changedSources.size()) +
            ", affectedBuilds=" +
            std::to_string(
                plan.builds.size() +
                plan.computeBuilds.size() +
                (plan.uiBuild.has_value() ? 1u : 0u)) +
            ", liveMaterials=" +
            std::to_string(plan.materials.size()));

        if (!shaderCompileWorker->Submit(std::move(plan)))
        {
            return;
        }

        latestSubmittedAutoReloadGeneration = generation;
        inFlightAutoReloadGeneration = generation;
        inFlightAutoReloadSourceEpoch =
            pendingAutoReloadSourceEpoch;
        lastSubmittedAutoReloadSources = shaderSources;
        for (const std::string& source : shaderSources)
        {
            pendingAutoReloadSources.erase(source);
        }
        if (pendingAutoReloadSources.empty())
        {
            pendingAutoReloadSourceEpoch = 0;
        }
    }
    catch (const std::exception& exception)
    {
        failedPendingAutoReloadSourceEpoch =
            pendingAutoReloadSourceEpoch;
        diagnostics.ReportError(
            "Shader auto reload batch " +
            std::to_string(generation) +
            " plan capture failed; current pipelines remain active: " +
            exception.what());
    }
}

RuntimeResult<WorldHandle>
EngineLoop::ExecuteWorldGraphTransaction(
    const std::string& scenePath,
    const MaterialDefinitionReloadBatch*
        materialDefinitionReload)
{
    if (rendererBackend == nullptr ||
        !rendererBackendInitialized)
    {
        return RuntimeResult<WorldHandle>::Failure(
            MakeRuntimeError(
                "EngineLoop.WorldTransactionBeforeRendererInit",
                "Cannot prepare a World/RenderGraph transaction before the renderer backend is initialized.",
                scenePath));
    }
    if (!controller)
    {
        return RuntimeResult<WorldHandle>::Failure(
            MakeRuntimeError(
                "EngineLoop.MissingController",
                "Cannot commit a World/RenderGraph transaction without a Controller.",
                scenePath));
    }

    try
    {
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

        auto preparedWorldResult =
            worldTransitionCoordinator
                ->PrepareWorldLoad(
                    scenePath,
                    candidateGraph->GetGraph(),
                    materialDefinitionReload);
        if (preparedWorldResult.IsFailure())
        {
            return RuntimeResult<WorldHandle>::Failure(
                preparedWorldResult.Error());
        }
        PreparedWorldTransition preparedWorld =
            std::move(
                preparedWorldResult.Value());
        if (worldGraphTransactionTestFaultInjection
                .failAfterCandidateWorldBuilt)
        {
            throw std::runtime_error(
                "Injected failure after candidate World build");
        }

        candidateGraph->GetGraph()
            .RestorePassMaterialInstances(
                preparedWorld.passMaterialBindings);
        candidateGraph->GetGraph()
            .SetOwnerGeneration(
                preparedWorld.activation
                    .handle.generation);

        std::shared_ptr<SceneNode> viewTarget =
            preparedWorld.activation
                .handle.viewTarget.lock();
        if (worldGraphTransactionTestFaultInjection
                .failViewTargetPrecheck ||
            !viewTarget)
        {
            return RuntimeResult<WorldHandle>::Failure(
                MakeRuntimeError(
                    "EngineLoop.MissingViewTarget",
                    "Candidate World has no controller view target.",
                    scenePath));
        }

        PreparedRuntimeBinding preparedRuntime =
            RenderSystem::GetInstance()
                .PrepareRuntimeBinding(
                    preparedWorld.world,
                    *preparedWorld.resourceCache,
                    candidateGraph->GetGraph());
        if (worldGraphTransactionTestFaultInjection
                .failAfterRuntimeBindingPrepared)
        {
            throw std::runtime_error(
                "Injected failure after runtime binding prepare");
        }
        PipelineFactory::
            PreparedGraphicsCandidateCommit
                preparedPipelineCommit =
                    pipelineFactory
                        ->PrepareCandidateCommit(
                            preparedWorld
                                .graphicsCandidateState);

        std::vector<ShaderBuildArtifact>
            artifactsToCommit;
        artifactsToCommit.reserve(
            preparedWorld.graphicsCandidateState
                .shaderBuildArtifacts.size());
        for (auto& preparedBuild :
             preparedWorld.graphicsCandidateState
                 .shaderBuildArtifacts)
        {
            const ShaderCompiler::
                CandidateSourceValidationResult
                    sourceValidation =
                        shaderCompiler
                            ->ValidateCandidateSourcesStillCurrent(
                                preparedBuild.request,
                                preparedBuild.artifact);
            if (!sourceValidation.current)
            {
                throw std::runtime_error(
                    "World transaction shader candidate source validation failed: " +
                    sourceValidation.reason +
                    "; source=" +
                    sourceValidation.sourceIdentity +
                    "; capturedDigest=" +
                    sourceValidation.capturedDigest +
                    "; currentDigest=" +
                    sourceValidation.currentDigest);
            }
            artifactsToCommit.push_back(
                std::move(
                    preparedBuild.artifact));
        }
        ValidateMaterialDefinitionSourcesStillCurrent(
            materialDefinitionReload,
            shaderCompiler->GetShaderRoot());
        std::vector<AtomicFileWrite>
            generatedIncludeWrites =
                BuildGeneratedIncludeWrites(
                    materialDefinitionReload);

        WaitForRenderThreadIdle();
        if (shouldClose)
        {
            return RuntimeResult<WorldHandle>::Failure(
                MakeRuntimeError(
                    "EngineLoop.RenderThreadFailedBeforeWorldTransaction",
                    "Cannot commit the World/RenderGraph transaction because the render thread failed.",
                    scenePath));
        }

        // Recheck immediately before the final fallible publication step.
        for (size_t buildIndex = 0;
             buildIndex <
                 artifactsToCommit.size();
             ++buildIndex)
        {
            const auto& preparedBuild =
                preparedWorld
                    .graphicsCandidateState
                    .shaderBuildArtifacts[
                        buildIndex];
            const ShaderCompiler::
                CandidateSourceValidationResult
                    sourceValidation =
                        shaderCompiler
                            ->ValidateCandidateSourcesStillCurrent(
                                preparedBuild.request,
                                artifactsToCommit[
                                    buildIndex]);
            if (!sourceValidation.current)
            {
                throw std::runtime_error(
                    "World transaction shader candidate became stale before commit: " +
                    sourceValidation.reason +
                    "; source=" +
                    sourceValidation.sourceIdentity);
            }
        }
        ValidateMaterialDefinitionSourcesStillCurrent(
            materialDefinitionReload,
            shaderCompiler->GetShaderRoot());
        if (worldGraphTransactionTestFaultInjection
                .failBeforeCommit)
        {
            throw std::runtime_error(
                "Injected failure after all World/graph/runtime prepare steps");
        }

        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        constexpr size_t MaximumRetirementCount = 4;
        std::vector<RetiredResource>
            retirementResources;
        retirementResources.reserve(
            MaximumRetirementCount);
        const uint64_t oldWorldGeneration =
            GetSubsystems()
                .GetWorldManager()
                .GetActiveWorldHandle()
                .generation;
        const uint64_t lastUsedEpoch =
            retireQueue.GetLastSubmittedEpoch();
        retirementResources.push_back({
            "WorldTransaction:World",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        retirementResources.push_back({
            "WorldTransaction:WorldLocalResources",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        retirementResources.push_back({
            "WorldTransaction:RenderGraph",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        retirementResources.push_back({
            "WorldTransaction:FrameLightBuffer",
            oldWorldGeneration,
            lastUsedEpoch,
            {}});
        PreparedResourceRetirements
            preparedRetirements =
                retireQueue.PrepareRetirements(
                    std::move(
                        retirementResources));
        const WorldHandle committedHandle =
            preparedWorld.activation.handle;
        RuntimeResult<WorldHandle> committedResult =
            RuntimeResult<WorldHandle>::Success(
                committedHandle);
        const std::string commitDiagnostic =
            "World/graph transaction committed: generation=" +
            std::to_string(committedHandle.generation) +
            ", scene=" + committedHandle.scenePath +
            ", graphGeneration=" +
            std::to_string(committedHandle.generation) +
            ", renderSystemGeneration=" +
            std::to_string(committedHandle.generation) +
            ", controllerGeneration=" +
            std::to_string(committedHandle.generation);

        shaderCompiler
            ->CommitArtifactsWithAdditionalFiles(
                artifactsToCommit,
                generatedIncludeWrites);

        // All work below is a prevalidated ownership swap. The complete
        // retirement queue state was prepared before file publication.
        RenderSystem::GetInstance()
            .ClearPendingWorldSnapshots();
        pipelineFactory->CommitPreparedCandidate(
            std::move(preparedPipelineCommit));

        RendererResourceCache::
            WorldLocalResourcePackageHandle
                retiredWorldResources =
                    RendererResourceCache::
                        GetInstance()
                            .CommitCandidate(
                                std::move(
                                    *preparedWorld
                                         .resourceCache));
        RenderGraph::GetInstance().SwapState(
            candidateGraph->GetGraph());
        std::shared_ptr<World> retiredWorld =
            GetSubsystems()
                .GetWorldManager()
                .CommitPreparedActivation(
                    std::move(
                        preparedWorld.activation));
        std::shared_ptr<void>
            retiredLightBuffer =
                RenderSystem::GetInstance()
                    .CommitPreparedRuntimeBinding(
                        std::move(
                            preparedRuntime));
        controller->SetViewTarget(
            std::move(viewTarget),
            GetSubsystems()
                .GetWorldManager()
                .GetActiveWorldHandle()
                .generation);

        preparedRetirements
            .GetAdditionalResource(0)
            .resource =
            std::static_pointer_cast<void>(
                std::move(retiredWorld));
        preparedRetirements
            .GetAdditionalResource(1)
            .resource =
            std::static_pointer_cast<void>(
                std::move(
                    retiredWorldResources));
        preparedRetirements
            .GetAdditionalResource(2)
            .resource =
            std::static_pointer_cast<void>(
                std::move(candidateGraph));
        preparedRetirements
            .GetAdditionalResource(3)
            .resource =
            std::move(retiredLightBuffer);
        retireQueue.CommitPreparedRetirements(
            std::move(
                preparedRetirements));

        try
        {
            GetSubsystems()
                .GetDiagnosticsSubsystem()
                .ReportInfo(commitDiagnostic);
        }
        catch (...)
        {
        }
        return committedResult;
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<WorldHandle>::Failure(
            MakeRuntimeError(
                "EngineLoop.WorldGraphTransactionFailed",
                exception.what(),
                scenePath));
    }
}

RuntimeResult<WorldHandle>
EngineLoop::ExecuteMaterialDefinitionWorldGraphTransactionForTest(
    const std::set<std::string>& sourceIdentities,
    uint64_t batchId)
{
    MaterialDefinitionReloadBatch batch =
        BuildMaterialDefinitionReloadBatch(
            batchId,
            sourceIdentities,
            shaderCompiler->GetShaderRoot());
    return ExecuteWorldGraphTransaction(
        GetSubsystems().GetWorldManager()
            .GetActiveWorldHandle().scenePath,
        &batch);
}

void EngineLoop::SetWorldGraphTransactionTestFaultInjection(
    WorldGraphTransactionTestFaultInjection injection) noexcept
{
    worldGraphTransactionTestFaultInjection =
        injection;
}

void EngineLoop::ProcessRequestedWorldTransition(
    RuntimeCommandExecutionResult& commandResult)
{
    if (!commandResult.worldLoadRequested)
    {
        return;
    }

    commandResult.worldRuntimeBindingAttempted =
        true;
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
    commandResult
        .worldRuntimeBindingSucceeded = true;
    commandResult.loadedWorld =
        transactionResult.Value();
    commandResult.rendererResourcesAfterLoad =
        CaptureRuntimeRendererResourceFingerprint();
}

void EngineLoop::ProcessPendingMaterialDefinitionReload()
{
    if (pendingMaterialDefinitionSources.empty() ||
        !shaderCompileWorker->IsIdle() ||
        shaderFileMonitor
            ->HasUnstableSourceChanges() ||
        failedPendingMaterialDefinitionSourceEpoch ==
            pendingMaterialDefinitionSourceEpoch)
    {
        return;
    }

    const WorldHandle& activeWorld =
        GetSubsystems()
            .GetWorldManager()
            .GetActiveWorldHandle();
    if (!activeWorld.IsValid())
    {
        return;
    }

    const uint64_t batchId =
        nextShaderReloadGeneration++;
    try
    {
        MaterialDefinitionReloadBatch batch =
            BuildMaterialDefinitionReloadBatch(
                batchId,
                pendingMaterialDefinitionSources,
                shaderCompiler->GetShaderRoot());
        auto transactionResult =
            ExecuteWorldGraphTransaction(
                activeWorld.scenePath,
                &batch);
        if (transactionResult.IsFailure())
        {
            throw std::runtime_error(
                FormatRuntimeError(
                    transactionResult.Error()));
        }

        latestMaterialDefinitionReloadCommittedGeneration =
            batchId;
        pendingMaterialDefinitionSources.clear();
        pendingMaterialDefinitionSourceEpoch = 0;
        failedPendingMaterialDefinitionSourceEpoch = 0;
        GetSubsystems()
            .GetDiagnosticsSubsystem()
            .ReportInfo(
                "Material definition reload batch " +
                std::to_string(batchId) +
                " committed all-or-nothing: changedM_=" +
                std::to_string(
                    batch.changedSources.size()) +
                ", worldGeneration=" +
                std::to_string(
                    transactionResult.Value()
                        .generation));
    }
    catch (const std::exception& exception)
    {
        latestMaterialDefinitionReloadFailedGeneration =
            batchId;
        failedPendingMaterialDefinitionSourceEpoch =
            pendingMaterialDefinitionSourceEpoch;
        GetSubsystems()
            .GetDiagnosticsSubsystem()
            .ReportError(
                "Material definition reload batch " +
                std::to_string(batchId) +
                " rejected; active World/graph/material resources and formal artifacts remain unchanged: " +
                exception.what());
    }
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
