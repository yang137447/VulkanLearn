#include "engine/engineLoop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <filesystem>
#include <string>
#include <utility>

#include "controller.h"
#include "engine/gameInstance.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeTestHooks.h"
#include "engine/subsystemCollection.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "pipeline/pipelineFactory.h"
#include "platform/platformApplication.h"
#include "platform/platformWindow.h"
#include "profiler.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/renderThread.h"
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

constexpr int RenderGraphRetireDrainFrameBudget = 180;

int CountRequestedEngineLoopTests(const RuntimeCommandExecutionResult& commandResult)
{
    int requestedCount = 0;
    if (commandResult.resizeStressRequested)
    {
        ++requestedCount;
    }
    if (commandResult.renderGraphReloadStressRequested)
    {
        ++requestedCount;
    }
    if (commandResult.frameSmokeRequested)
    {
        ++requestedCount;
    }
    return requestedCount;
}

std::string FormatRequestedEngineLoopTests(const RuntimeCommandExecutionResult& commandResult)
{
    std::string tests;
    if (commandResult.resizeStressRequested)
    {
        tests += tests.empty() ? "resizestress" : ", resizestress";
    }
    if (commandResult.renderGraphReloadStressRequested)
    {
        tests += tests.empty() ? "graphreloadstress" : ", graphreloadstress";
    }
    if (commandResult.frameSmokeRequested)
    {
        tests += tests.empty() ? "framesmoke" : ", framesmoke";
    }
    return tests;
}

}

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

    auto inputResult = GetSubsystems().GetInputSubsystem().Initialize(platformWindow);
    if (inputResult.IsFailure())
    {
        return inputResult;
    }

    auto runtimeResult = InitializeRuntimeSystems(
        platformWindow,
        platformApplication.GetVulkanExtensions(),
        launchOptions.developerUiMode);
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
}

void EngineLoop::QueueRuntimeCommand(RuntimeCommand command)
{
    GetSubsystems().GetCommandBus().Queue(std::move(command));
}

void EngineLoop::SetExitAfterRuntimeTests(bool enabled)
{
    exitAfterRuntimeTests = enabled;
}

void EngineLoop::StartResizeStress(int resizeCount)
{
    resizeStressTotal = std::max(0, resizeCount);
    resizeStressRemaining = resizeStressTotal;
    resizeStressCompletedCount = 0;
    resizeStressFailed = false;
    resizeStressCompleted = resizeStressTotal == 0;
    resizeStressActive = resizeStressTotal > 0;

    if (resizeStressActive)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Resize stress started: count=" + std::to_string(resizeStressTotal));
    }
}

void EngineLoop::StartRenderGraphReloadStress(int reloadCount)
{
    graphReloadStressTotal = std::max(0, reloadCount);
    graphReloadStressRemaining = graphReloadStressTotal;
    graphReloadStressCompletedCount = 0;
    graphReloadStressFailed = false;
    graphReloadStressWaitingForDrain = false;
    graphReloadRetireDrainFramesRemaining = 0;
    graphReloadMaxPendingRetiredResources = 0;
    graphReloadStressCompleted = graphReloadStressTotal == 0;
    graphReloadStressActive = graphReloadStressTotal > 0;

    if (graphReloadStressActive)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Render graph reload stress started: count=" +
            std::to_string(graphReloadStressTotal));
    }
}

void EngineLoop::StartFrameSmokeTest(int frameCount)
{
    frameSmokeTotal = std::max(0, frameCount);
    frameSmokeCompletedCount = 0;
    frameSmokeTotalMs = 0.0;
    frameSmokeMaxMs = 0.0;
    frameSmokeMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalFrameCount = 0;
    frameSmokeIntervalTotalMs = 0.0;
    frameSmokeIntervalMaxMs = 0.0;
    frameSmokeIntervalMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalRenderLoopTotalMs = 0.0;
    frameSmokeIntervalRenderLoopMaxMs = 0.0;
    frameSmokeFailed = false;
    frameSmokeCompleted = frameSmokeTotal == 0;
    frameSmokeActive = frameSmokeTotal > 0;

    if (frameSmokeActive)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Frame smoke test started: frames=" + std::to_string(frameSmokeTotal));
    }
}

RuntimeResult<void> EngineLoop::InitializeRuntimeSystems(
    PlatformWindow& window,
    std::vector<const char*>& vulkanExtensions,
    DeveloperUiLaunchMode developerUiMode)
{
    // Material parameter includes are generated before shader compilation so
    // GLSL sees the latest JSON-driven parameter layout.
    MaterialParameterIncludeGenerator::GenerateAllIncludes();
    ShaderCompiler shaderCompiler;
    std::string shaderFolderPath = GetRuntimeConfig().ResolvePath("shader");
    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Shader folder path: " + shaderFolderPath);
    shaderCompiler.StartCompile(shaderFolderPath);

    rendererBackend = std::make_unique<RendererBackendVulkan>();
    rendererBackend->Initialize(vulkanExtensions, window.GetNativeHandle());
    rendererBackendInitialized = true;
    RenderSystem::GetInstance().SetRendererBackend(rendererBackend.get());
    RenderSystem::GetInstance().SetCsmSettings(GetRuntimeConfig().GetCsmSettings());

    pipelineFactory = rendererBackend->CreatePipelineFactory();
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

    if (GetRuntimeConfig().ShouldUseRenderThread())
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

    GetSubsystems().GetRuntimeTestHooks().Update(
        GetSubsystems().GetCommandBus(),
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
        *worldTransitionCoordinator,
        GetSubsystems().GetRuntimeTestHooks(),
        GetRuntimeConfig(),
        GetSubsystems().GetDiagnosticsSubsystem());

    StartRequestedEngineLoopTests(commandResult);
    if (shouldClose)
    {
        return;
    }
    commandResult.activeWorldBeforeCommand = activeWorldBeforeCommand;

    if (commandResult.worldChanged)
    {
        commandResult.worldRuntimeBindingAttempted = true;

        auto bindResult = BindActiveWorldRuntimeObjects(commandResult.loadedWorld);
        if (bindResult.IsFailure())
        {
            GetSubsystems().GetDiagnosticsSubsystem().ReportRuntimeError(
                "World runtime binding failed",
                bindResult.Error());
        }
        else
        {
            auto graphReloadResult = ReloadRenderGraphResources(VL::RenderGraphReleaseMode::Retire);
            if (graphReloadResult.IsFailure())
            {
                GetSubsystems().GetDiagnosticsSubsystem().ReportRuntimeError(
                    "World render graph rebinding failed",
                    graphReloadResult.Error());
            }
            else
            {
                commandResult.worldRuntimeBindingSucceeded = true;
            }
        }
    }

    commandResult.activeWorldAfterCommand =
        GetSubsystems().GetWorldManager().GetActiveWorldHandle();

    GetSubsystems().GetRuntimeTestHooks().NotifyCommandResult(
        commandResult,
        GetSubsystems().GetDiagnosticsSubsystem());

    if (exitAfterRuntimeTests)
    {
        if (graphReloadStressCompleted)
        {
            exitCode = graphReloadStressFailed ? 2 : 0;
            shouldClose = true;
            return;
        }

        if (resizeStressCompleted)
        {
            exitCode = resizeStressFailed ? 2 : 0;
            shouldClose = true;
            return;
        }

        if (frameSmokeCompleted)
        {
            exitCode = frameSmokeFailed ? 2 : 0;
            shouldClose = true;
            return;
        }

        const RuntimeTestStatus runtimeTestStatus =
            GetSubsystems().GetRuntimeTestHooks().GetRuntimeTestStatus();
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

    UpdateResizeStress();
    if (shouldClose)
    {
        return;
    }

    UpdateRenderGraphReloadStress();
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
        const bool collectFrameSmokeTiming = frameSmokeActive && !frameSmokeCompleted;
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
            AddFrameSmokeRenderLoopTime(renderLoopTimeMs);
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

    UpdateFrameSmokeTest(frameTimeMs);
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
            if (ShouldSuppressResizeEvent(event.width, event.height))
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

void EngineLoop::StartRequestedEngineLoopTests(const RuntimeCommandExecutionResult& commandResult)
{
    const int requestedTestCount = CountRequestedEngineLoopTests(commandResult);
    if (requestedTestCount == 0)
    {
        return;
    }

    if (requestedTestCount > 1)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportError(
            "Runtime validation command rejected because multiple EngineLoop-owned tests were requested in one frame: " +
            FormatRequestedEngineLoopTests(commandResult) +
            ".");
        if (exitAfterRuntimeTests)
        {
            exitCode = 2;
            shouldClose = true;
        }
        return;
    }

    if (resizeStressActive || graphReloadStressActive || frameSmokeActive)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportError(
            "Runtime validation command rejected because an EngineLoop-owned runtime test is already running.");
        if (exitAfterRuntimeTests)
        {
            exitCode = 2;
            shouldClose = true;
        }
        return;
    }

    if (GetSubsystems().GetRuntimeTestHooks().GetRuntimeTestStatus() == RuntimeTestStatus::Running)
    {
        GetSubsystems().GetDiagnosticsSubsystem().ReportError(
            "Runtime validation command rejected because a runtime test hook is already running.");
        if (exitAfterRuntimeTests)
        {
            exitCode = 2;
            shouldClose = true;
        }
        return;
    }

    if (commandResult.resizeStressRequested)
    {
        StartResizeStress(commandResult.resizeStressCount);
    }
    if (commandResult.renderGraphReloadStressRequested)
    {
        StartRenderGraphReloadStress(commandResult.renderGraphReloadStressCount);
    }
    if (commandResult.frameSmokeRequested)
    {
        StartFrameSmokeTest(commandResult.frameSmokeCount);
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
        auto passMaterialSnapshot = renderGraph.CapturePassMaterialInstances();

        renderGraph.Shutdown(*rendererBackend);
        RenderSystem::GetInstance().ReleaseSwapchainDependentResources();
        rendererBackend->RecreateSwapchain(
            static_cast<int>(width),
            static_cast<int>(height));
        renderGraph.LoadRenderGraph(
            GetRuntimeConfig().GetRenderGraphJson(),
            *rendererBackend);
        renderGraph.RestorePassMaterialInstances(passMaterialSnapshot);
        RenderSystem::GetInstance().RebuildSwapchainDependentResources();
    }
    catch (const std::exception& exception)
    {
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

        RenderGraph& renderGraph = RenderGraph::GetInstance();
        auto passMaterialSnapshot = renderGraph.CapturePassMaterialInstances();

        renderGraph.Shutdown(*rendererBackend, releaseMode);
        renderGraph.LoadRenderGraph(
            GetRuntimeConfig().GetRenderGraphJson(),
            *rendererBackend);
        renderGraph.RestorePassMaterialInstances(passMaterialSnapshot);
        RenderSystem::GetInstance().RebuildRenderGraphDependentResources();
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.RenderGraphReloadFailed",
            exception.what()));
    }

    return RuntimeResult<void>::Success();
}

void EngineLoop::UpdateResizeStress()
{
    if (!resizeStressActive || resizeStressRemaining <= 0)
    {
        return;
    }

    const Eigen::Vector2f configuredWindowSize = GetRuntimeConfig().GetWindowSize();
    const int baseWidth = static_cast<int>(configuredWindowSize.x());
    const int baseHeight = static_cast<int>(configuredWindowSize.y());
    const bool useSmallerSize = (resizeStressCompletedCount % 2) == 0;
    const int targetWidth = useSmallerSize ? std::max(320, baseWidth - 160) : baseWidth;
    const int targetHeight = useSmallerSize ? std::max(240, baseHeight - 90) : baseHeight;

    window->SetSize(targetWidth, targetHeight);
    suppressNextResizeEvent = true;
    suppressedResizeWidth = static_cast<uint32_t>(targetWidth);
    suppressedResizeHeight = static_cast<uint32_t>(targetHeight);

    auto resizeResult = RecreateRendererForWindowResize(
        static_cast<uint32_t>(targetWidth),
        static_cast<uint32_t>(targetHeight));
    if (resizeResult.IsFailure())
    {
        resizeStressActive = false;
        resizeStressFailed = true;
        resizeStressCompleted = true;
        GetSubsystems().GetDiagnosticsSubsystem().ReportRuntimeError(
            "Resize stress failed",
            resizeResult.Error());
        return;
    }

    ++resizeStressCompletedCount;
    --resizeStressRemaining;
    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Resize stress step completed: " +
        std::to_string(resizeStressCompletedCount) +
        "/" +
        std::to_string(resizeStressTotal) +
        " size=" +
        std::to_string(targetWidth) +
        "x" +
        std::to_string(targetHeight));

    if (resizeStressRemaining <= 0)
    {
        resizeStressActive = false;
        resizeStressCompleted = true;
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Resize stress completed: " +
            std::to_string(resizeStressCompletedCount) +
            "/" +
            std::to_string(resizeStressTotal) +
            " resize transactions succeeded.");
    }
}

void EngineLoop::UpdateRenderGraphReloadStress()
{
    if (!graphReloadStressActive)
    {
        return;
    }

    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    if (graphReloadStressWaitingForDrain)
    {
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        graphReloadMaxPendingRetiredResources = std::max(
            graphReloadMaxPendingRetiredResources,
            pendingRetiredResources);

        if (pendingRetiredResources == 0)
        {
            graphReloadStressActive = false;
            graphReloadStressCompleted = true;
            GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
                "Render graph reload stress completed: " +
                std::to_string(graphReloadStressCompletedCount) +
                "/" +
                std::to_string(graphReloadStressTotal) +
                " reloads succeeded, retire queue max pending=" +
                std::to_string(graphReloadMaxPendingRetiredResources) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            return;
        }

        --graphReloadRetireDrainFramesRemaining;
        if (graphReloadRetireDrainFramesRemaining <= 0)
        {
            graphReloadStressActive = false;
            graphReloadStressCompleted = true;
            graphReloadStressFailed = true;
            GetSubsystems().GetDiagnosticsSubsystem().ReportError(
                "Render graph reload stress failed because retired graph resources did not drain before the frame budget expired. pending=" +
                std::to_string(pendingRetiredResources) +
                ", submittedEpoch=" +
                std::to_string(retireQueue.GetLastSubmittedEpoch()) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
        }
        return;
    }

    if (graphReloadStressRemaining <= 0)
    {
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        graphReloadMaxPendingRetiredResources = std::max(
            graphReloadMaxPendingRetiredResources,
            pendingRetiredResources);

        if (graphReloadStressTotal > 1 && graphReloadMaxPendingRetiredResources == 0)
        {
            graphReloadStressActive = false;
            graphReloadStressCompleted = true;
            graphReloadStressFailed = true;
            GetSubsystems().GetDiagnosticsSubsystem().ReportError(
                "Render graph reload stress failed because no retired graph resources were observed after repeated reloads.");
            return;
        }

        graphReloadStressWaitingForDrain = true;
        graphReloadRetireDrainFramesRemaining = RenderGraphRetireDrainFrameBudget;
        GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
            "Render graph reload stress waiting for retire queue drain: pending=" +
            std::to_string(pendingRetiredResources) +
            ", maxPending=" +
            std::to_string(graphReloadMaxPendingRetiredResources) +
            ".");
        return;
    }

    auto reloadResult = ReloadRenderGraphResources(VL::RenderGraphReleaseMode::Retire);
    if (reloadResult.IsFailure())
    {
        graphReloadStressActive = false;
        graphReloadStressCompleted = true;
        graphReloadStressFailed = true;
        GetSubsystems().GetDiagnosticsSubsystem().ReportRuntimeError(
            "Render graph reload stress failed",
            reloadResult.Error());
        return;
    }

    --graphReloadStressRemaining;
    ++graphReloadStressCompletedCount;

    const size_t pendingRetiredResources = retireQueue.GetPendingCount();
    graphReloadMaxPendingRetiredResources = std::max(
        graphReloadMaxPendingRetiredResources,
        pendingRetiredResources);
    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Render graph reload stress reloaded graph " +
        std::to_string(graphReloadStressCompletedCount) +
        "/" +
        std::to_string(graphReloadStressTotal) +
        ", pending retired resources=" +
        std::to_string(pendingRetiredResources) +
        ".");
}

void EngineLoop::UpdateFrameSmokeTest(double frameTimeMs)
{
    if (!frameSmokeActive || frameSmokeCompleted)
    {
        return;
    }

    if (frameTimeMs <= 0.0 || !std::isfinite(frameTimeMs))
    {
        frameSmokeFailed = true;
        frameSmokeCompleted = true;
        frameSmokeActive = false;
        GetSubsystems().GetDiagnosticsSubsystem().ReportError(
            "Frame smoke test failed because a measured frame time was invalid.");
        return;
    }

    ++frameSmokeCompletedCount;
    frameSmokeTotalMs += frameTimeMs;
    frameSmokeMaxMs = std::max(frameSmokeMaxMs, frameTimeMs);
    frameSmokeMinMs = std::min(frameSmokeMinMs, frameTimeMs);
    ++frameSmokeIntervalFrameCount;
    frameSmokeIntervalTotalMs += frameTimeMs;
    frameSmokeIntervalMaxMs = std::max(frameSmokeIntervalMaxMs, frameTimeMs);
    frameSmokeIntervalMinMs = std::min(frameSmokeIntervalMinMs, frameTimeMs);

    if (frameSmokeIntervalFrameCount >= frameSmokeIntervalSize)
    {
        ReportFrameSmokeInterval();
    }

    if (frameSmokeCompletedCount < frameSmokeTotal)
    {
        return;
    }

    if (frameSmokeIntervalFrameCount > 0)
    {
        ReportFrameSmokeInterval();
    }

    frameSmokeCompleted = true;
    frameSmokeActive = false;
    const double averageFrameMs = frameSmokeTotalMs /
        static_cast<double>(std::max(1, frameSmokeCompletedCount));
    const double averageFps = 1000.0 / averageFrameMs;

    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Frame smoke test completed: " +
        std::to_string(frameSmokeCompletedCount) +
        "/" +
        std::to_string(frameSmokeTotal) +
        " frames, avgFrameMs=" +
        std::to_string(averageFrameMs) +
        ", minFrameMs=" +
        std::to_string(frameSmokeMinMs) +
        ", maxFrameMs=" +
        std::to_string(frameSmokeMaxMs) +
        ", avgFps=" +
        std::to_string(averageFps));
}

void EngineLoop::ReportFrameSmokeInterval()
{
    const double averageFrameMs = frameSmokeIntervalTotalMs /
        static_cast<double>(std::max(1, frameSmokeIntervalFrameCount));
    const double averageFps = 1000.0 / averageFrameMs;
    const double averageRenderLoopMs = frameSmokeIntervalRenderLoopTotalMs /
        static_cast<double>(std::max(1, frameSmokeIntervalFrameCount));
    const ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();

    GetSubsystems().GetDiagnosticsSubsystem().ReportInfo(
        "Frame smoke interval: frame=" +
        std::to_string(frameSmokeCompletedCount) +
        "/" +
        std::to_string(frameSmokeTotal) +
        ", intervalFrames=" +
        std::to_string(frameSmokeIntervalFrameCount) +
        ", avgFrameMs=" +
        std::to_string(averageFrameMs) +
        ", minFrameMs=" +
        std::to_string(frameSmokeIntervalMinMs) +
        ", maxFrameMs=" +
        std::to_string(frameSmokeIntervalMaxMs) +
        ", avgFps=" +
        std::to_string(averageFps) +
        ", avgRenderLoopMs=" +
        std::to_string(averageRenderLoopMs) +
        ", maxRenderLoopMs=" +
        std::to_string(frameSmokeIntervalRenderLoopMaxMs) +
        ", retiredPending=" +
        std::to_string(retireQueue.GetPendingCount()) +
        ", submittedEpoch=" +
        std::to_string(retireQueue.GetLastSubmittedEpoch()) +
        ", completedEpoch=" +
        std::to_string(retireQueue.GetLastCompletedEpoch()));

    frameSmokeIntervalFrameCount = 0;
    frameSmokeIntervalTotalMs = 0.0;
    frameSmokeIntervalMaxMs = 0.0;
    frameSmokeIntervalMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalRenderLoopTotalMs = 0.0;
    frameSmokeIntervalRenderLoopMaxMs = 0.0;
}

void EngineLoop::AddFrameSmokeRenderLoopTime(double renderLoopTimeMs)
{
    if (!frameSmokeActive || frameSmokeCompleted)
    {
        return;
    }

    frameSmokeIntervalRenderLoopTotalMs += renderLoopTimeMs;
    frameSmokeIntervalRenderLoopMaxMs =
        std::max(frameSmokeIntervalRenderLoopMaxMs, renderLoopTimeMs);
}

bool EngineLoop::ShouldSuppressResizeEvent(uint32_t width, uint32_t height)
{
    if (!suppressNextResizeEvent)
    {
        return false;
    }

    if (width == suppressedResizeWidth && height == suppressedResizeHeight)
    {
        suppressNextResizeEvent = false;
        return true;
    }

    return false;
}

} // namespace VL
