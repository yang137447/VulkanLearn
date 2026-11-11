#include "engine/frame/engineFramePhases.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "SDL3/SDL.h"
#include "audio/audioRuntime.h"
#include "controller.h"
#include "core/cpuFrameProfiler.h"
#include "core/performanceSnapshot.h"
#include "engine/destructibleRuntime.h"
#include "engine/editorHost.h"
#include "engine/modelPreviewCamera.h"
#include "engine/projectileRuntime.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeTestHooks.h"
#include "engine/runtimeConfig.h"
#include "engine/subsystemCollection.h"
#include "engine/terrainChunkStreamer.h"
#include "engine/testing/runtimeValidationServices.h"
#include "engine/ui/editorInteractionRuntime.h"
#include "input/inputSubsystem.h"
#include "physics/fracture/fractureBudget.h"
#include "physics/runtime/fractureImpactWorld.h"
#include "platform/platformWindow.h"
#include "profiler.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/renderThread.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "script/scriptRuntime.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/uiSubsystem.h"
#include "world/world.h"
#include "world/worldManager.h"

namespace VL
{

namespace
{

void RunEngineUpdatePhase(
    const EngineFrameContext& context,
    float deltaTime,
    float simulationDelta)
{
    SubsystemCollection& subsystems = *context.subsystems;
    AudioRuntime* const& audioRuntime = context.audioRuntime;
    Controller* const& controller = context.controller;
    DestructibleRuntimeRegistry* const& destructibleRuntimes =
        context.destructibleRuntimes;
    Physics::FractureImpactWorld* const& fractureWorld = context.fractureWorld;
    Physics::Fracture::FractureBudget* const& fractureBudget =
        context.fractureBudget;
    ProjectileRuntime* const& projectileRuntime = context.projectileRuntime;
    CpuFrameProfiler& cpuFrameProfiler = *context.cpuFrameProfiler;
    const RuntimeConfig& runtimeConfig = *context.runtimeConfig;
    uint64_t& autoPlayWorldGeneration = *context.autoPlayWorldGeneration;
    bool& destructibleSessionActive = *context.destructibleSessionActive;
    PROFILE_SCOPE("Update");
    auto& scripts = subsystems.GetScriptRuntime();
    const auto& world = subsystems.GetWorldManager().GetActiveWorld();
    if (world && audioRuntime && world->GetRenderCamera())
    {
        audioRuntime->SetListenerPosition(world->GetRenderCamera()->GetPosition());
        // pan 需要相机朝向；只读取现有渲染相机，不修改它。
        audioRuntime->SetListenerOrientation(
            world->GetRenderCamera()->GetForwardVector(),
            world->GetRenderCamera()->GetUpVector());
    }
    {
        // scripts 覆盖蓝图资产轮询、自动 Play 判定、编辑控制器与脚本 Tick；
        // DebugDraw 的帧边界清理也随脚本状态一起计入这里。
        CpuFrameStageScope scriptsStage(cpuFrameProfiler, CpuFrameStage::Scripts);
        // 命令先决定 Pause/World，再清理上一完整帧；Console/BeginPlay 的新请求仍保留。
        if (world && !scripts.IsPaused()) world->GetDebugDraw().BeginFrame();
        TickEngineBlueprintAssets(context.scriptBuild);
        // 故障 Stop 后先收回动态模板，再构造程序集候选，避免把上一轮资产收录为常驻模板。
        TickEngineScriptBuild(context.scriptBuild, deltaTime);
        if (world && world->GetGeneration() != autoPlayWorldGeneration &&
            runtimeConfig.GetScriptSettings().autoPlay &&
            !RenderSystem::GetInstance().IsOfflineMode() &&
            std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json")
        {
            // 导出的游戏在正式World提交后进入同一Play入口；每代只启动一次，Stop不会自行重启。
            autoPlayWorldGeneration = world->GetGeneration();
            if (!world->GetScriptBlueprints().empty())
            {
                std::string error;
                if (!scripts.Control(*world, ScriptPlayState::Playing, error))
                    subsystems.GetDiagnosticsSubsystem().ReportError("Game auto-play failed: " + error);
            }
            // 自动 Play 与手动 Play 共用同一套交互隔离：参考网格、选择描边与
            // 编辑器观察相机在同一帧退出。
            if (context.editorSceneInteractionWasEnabled != nullptr)
                SynchronizeEditorInteractionState(
                    subsystems, context.uiSubsystem, context.controller,
                    *context.editorSceneInteractionWasEnabled);
        }
        // 离线渲染消费作者场景；模式切换先恢复作者 TRS，避免隐藏 Play 控件后仍继续游戏。
        if (world && RenderSystem::GetInstance().IsOfflineMode() && !scripts.IsEditing())
        {
            std::string error;
            scripts.Control(*world, ScriptPlayState::Editing, error);
        }
        if (scripts.IsEditing())
            controller->Update(simulationDelta, subsystems.GetInputSubsystem().GetActionState());
        // 读回走"渲染线程权威值 -> GT 快照 -> C#"：GT 只取走变化的快照。
        if (world && world->GetScriptContext() && !scripts.IsEditing() &&
            context.lastScriptMaterialState != nullptr &&
            context.lastScriptMaterialStateWorldGeneration != nullptr)
        {
            nlohmann::json updatedState;
            const bool updated =
                RenderSystem::GetInstance().TakeScriptMaterialState(updatedState);
            if (updated) *context.lastScriptMaterialState = updatedState.dump();
            // 新 World 的托管上下文是全新的；即使数值相同也必须重新推一次快照。
            const bool newWorld =
                world->GetGeneration() != *context.lastScriptMaterialStateWorldGeneration;
            if (newWorld)
                *context.lastScriptMaterialStateWorldGeneration = world->GetGeneration();
            if (!context.lastScriptMaterialState->empty() && (updated || newWorld))
                scripts.PushMaterialState(
                    *world, nlohmann::json::parse(*context.lastScriptMaterialState));
        }
        scripts.Tick(world, simulationDelta,
            subsystems.GetInputSubsystem().GetActionState(),
            subsystems.GetDiagnosticsSubsystem());
        if (world)
        {
            // GT 只把命令交给渲染线程；MaterialInstance 由 RT 独占，避免参数写入与 UBO 上传并发。
            RenderSystem::GetInstance().ApplyScriptMaterialCommands(
                world->TakeScriptMaterialCommands());
            std::vector<std::string> materialErrors;
            RenderSystem::GetInstance().TakeScriptMaterialErrors(materialErrors);
            for (const std::string& error : materialErrors)
                subsystems.GetDiagnosticsSubsystem().ReportWarning(
                    error, "Blueprint Material");
        }
        // 粒子模拟跟随脚本时钟：Pause 冻结，Playing 使用 timeScale 后的 delta，
        // Editing 保持编辑器时间。必须在 Tick 之后、发布快照之前写入。
        RenderSystem::GetInstance().SetPendingSimulationDelta(
            scripts.ParticleDeltaOr(simulationDelta));
    }
    {
        // 可破坏物会话跟着 Play/Editing 状态切换；先恢复作者场景再步进共享冲击世界。
        const bool offline = RenderSystem::GetInstance().IsOfflineMode();
        const bool nativeGameplay = world && world->IsNativeGameplay();
        const bool sessionActive = world && !offline &&
            (nativeGameplay || !scripts.IsEditing());
        const bool gameplayActive = sessionActive &&
            (nativeGameplay || !scripts.IsPaused());
        const bool editingNow =
            world && !offline && !nativeGameplay && scripts.IsEditing();
        if (editingNow && destructibleSessionActive)
        {
            // Stop 进入 Editing 时恢复作者场景；Pause 只冻结，不重置。
            if (destructibleRuntimes) destructibleRuntimes->Reset(*world);
            if (fractureBudget) fractureBudget->Reset();
            if (projectileRuntime) projectileRuntime->Clear(*world);
        }
        destructibleSessionActive = sessionActive;

        std::vector<Physics::FractureImpact> impacts;
        if (gameplayActive && fractureWorld)
        {
            try
            {
                // 共享冲击世界每帧只步进一次；事件再分发给 registry。
                impacts = fractureWorld->Step(simulationDelta);
            }
            catch (const std::exception& error)
            {
                subsystems.GetDiagnosticsSubsystem().ReportError(
                    std::string("Fracture physics step failed: ") +
                    error.what());
            }
            const auto& input = subsystems.GetInputSubsystem().GetActionState();
            if (projectileRuntime && world->GetRenderCamera() &&
                input.gameplay.contains("actions") &&
                input.gameplay["actions"].contains("Fire") &&
                input.gameplay["actions"]["Fire"].value("pressed", false))
            {
                projectileRuntime->Fire(*world, *world->GetRenderCamera());
            }
            if (destructibleRuntimes)
            {
                destructibleRuntimes->HandleImpacts(*world, impacts);
                destructibleRuntimes->Tick(*world, simulationDelta);
            }
            if (projectileRuntime)
                projectileRuntime->Tick(*world, simulationDelta, impacts);
        }
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
    // 风使用 RuntimeClock 的原始 deltaTime，而不是已夹到 0.1 s 的 simulationDelta：
    // master SpeedTree 用连续墙钟，风自身的 1/60 固定步长和 0.25 s 追帧上限会消化
    // 长帧；这里若用夹取后的值，hitch 时风会永久变慢。
    RenderSystem::GetInstance().SetSimulationFrameDeltas(
        simulationDelta, gameSimulationDelta, hairSimulationDelta, deltaTime);
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
            context.callbacks.queueRuntimeCommand(std::move(command));
            scripts.SetFeedback("Actor asset changed; reloading scene");
        }
    }
}

void RunEngineRenderSubmitPhase(const EngineFrameContext& context)
{
    CpuFrameProfiler& cpuFrameProfiler = *context.cpuFrameProfiler;
    RenderThread* const& renderThread = context.renderThread;
    bool& shouldClose = *context.shouldClose;
    PROFILE_SCOPE("RenderLoop");
    if (renderThread && renderThread->IsRunning())
    {
        // 这里是多线程模式。GT 只发布世界快照并唤醒渲染线程；RT 随后
        // 完成消费、命令录制、提交与 present。GT 只在“上一帧仍未归还
        // 帧槽”时阻塞（最多领先一帧），随后立即发布本帧并继续 Tick，
        // 不再等待本帧录制完成。
        {
            CpuFrameStageScope waitStage(cpuFrameProfiler, CpuFrameStage::SubmitPresent);
            WaitForEngineRenderFrameSlot(context.windowLifecycle);
        }
        if (!shouldClose)
        {
            {
                CpuFrameStageScope snapshotStage(cpuFrameProfiler, CpuFrameStage::WorldSnapshot);
                RenderSystem::GetInstance().PublishSnapshotFromActiveWorld();
            }
            {
                CpuFrameStageScope recordStage(cpuFrameProfiler, CpuFrameStage::RenderRecord);
                renderThread->SubmitFrame();
            }
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

} // namespace

void TickEngineFrame(EngineFrameContext& context)
{
    SubsystemCollection& subsystems = *context.subsystems;
    CpuFrameProfiler& cpuFrameProfiler = *context.cpuFrameProfiler;
    bool& shouldClose = *context.shouldClose;
    int& exitCode = *context.exitCode;
    RenderThread* const& renderThread = context.renderThread;
    RuntimeCommandExecutor* const& runtimeCommandExecutor =
        context.runtimeCommandExecutor;
    RuntimeValidationServices* const& runtimeValidationServices =
        context.runtimeValidationServices;
    IEditorHost* const& editorHost = context.editorHost;
    UiSubsystem* const& uiSubsystem = context.uiSubsystem;
    ModelPreviewCamera& modelPreviewCamera = *context.modelPreviewCamera;
    TerrainChunkStreamer* const& terrainChunkStreamer =
        context.terrainChunkStreamer;
    AudioRuntime* const& audioRuntime = context.audioRuntime;
    DestructibleRuntimeRegistry* const& destructibleRuntimes =
        context.destructibleRuntimes;
    ProjectileRuntime* const& projectileRuntime = context.projectileRuntime;
    RendererBackendVulkan* const& rendererBackend = context.rendererBackend;
    PlatformWindow* const& window = context.window;
    const RuntimeConfig& runtimeConfig = *context.runtimeConfig;
    std::vector<PlatformEvent>& platformEvents = *context.platformEvents;
    const bool& exitAfterRuntimeTests = *context.exitAfterRuntimeTests;

    PROFILE_SCOPE("Frame");

    // CPU 阶段按 GT Tick 归零；早退帧不 Commit，下一帧 BeginFrame 会丢弃残余累加值。
    cpuFrameProfiler.BeginFrame();

    // GT 可能退休仍被“已发布但 RT 尚未解析”的快照引用的资源。把待渲染帧数
    // 计入退休下界，确保这些引用不会在当前帧被提前释放。
    VL::ResourceRetireQueue::GetInstance().SetPendingRenderFrames(
        renderThread && renderThread->IsRunning()
            ? renderThread->GetPendingFrameCount()
            : 0);

    subsystems.GetConsoleSubsystem().Update();

    ApplyQueuedEngineUiActions(context.uiActions, context.scriptBuild);
    UpdateEngineUiInputPolicy(context.uiActions);
    if (shouldClose)
    {
        return;
    }

    const EnvironmentUpdateDiagnostics environmentTestDiagnostics =
        RenderSystem::GetInstance().GetEnvironmentUpdateDiagnostics();
    RuntimeTestHooks& runtimeTests = subsystems.GetRuntimeTestHooks();
    runtimeTests.Update(
        subsystems.GetCommandBus(),
        subsystems.GetWorldManager(),
        environmentTestDiagnostics,
        *runtimeValidationServices,
        subsystems.GetDiagnosticsSubsystem());
    PollEngineRenderThreadFatalError(context.windowLifecycle);
    if (shouldClose)
    {
        return;
    }

    const WorldHandle activeWorldBeforeCommand =
        subsystems.GetWorldManager().GetActiveWorldHandle();

    RuntimeCommandExecutionResult commandResult = runtimeCommandExecutor->ExecuteQueuedCommands(
        subsystems.GetCommandBus(),
        RenderSystem::GetInstance(),
        subsystems.GetWorldManager(),
        runtimeTests,
        runtimeConfig,
        subsystems.GetDiagnosticsSubsystem(),
        &subsystems.GetInputSubsystem(),
        &subsystems.GetScriptRuntime(),
        editorHost,
        destructibleRuntimes,
        projectileRuntime);
    commandResult.activeWorldBeforeCommand = activeWorldBeforeCommand;

    if (commandResult.quitRequested)
    {
        exitCode = 0;
        shouldClose = true;
        return;
    }

    context.callbacks.processRequestedWorldTransition(commandResult);
    if (commandResult.terrainFeedbackStatisticsRequested)
    {
        RenderSystem::GetInstance().ReportTerrainFeedbackStatistics();
    }
    if (commandResult.terrainStatisticsRequested)
    {
        RenderSystem::GetInstance().ReportTerrainStatistics();
    }
    if (commandResult.terrainLayerStatisticsRequested)
    {
        RenderSystem::GetInstance().ReportTerrainLayerStatistics();
    }
    if (commandResult.terrainLodStatisticsRequested)
    {
        RenderSystem::GetInstance().ReportTerrainLodStatistics();
    }
    if (commandResult.terrainChunkStatisticsRequested)
    {
        RenderSystem::GetInstance().ReportTerrainChunkStatistics();
    }
    if (commandResult.terrainCollisionStatisticsRequested)
    {
        subsystems.GetScriptRuntime().ReportTerrainCollisionStatistics();
    }
    if (const auto& world = subsystems.GetWorldManager().GetActiveWorld())
    {
        const auto rect = uiSubsystem ? uiSubsystem->GetSceneViewportRect() : UiViewportRect{};
        const float aspect = rect.width && rect.height ? float(rect.width) / float(rect.height)
            : runtimeConfig.GetWindowAspectRatio();
        modelPreviewCamera.Synchronize(*world, aspect);
    }
    if (commandResult.loadWorldAttempted &&
        uiSubsystem != nullptr)
    {
        uiSubsystem->NotifyWorldLoadCompleted();
    }

    context.callbacks.processShaderRuntimeRequests(commandResult);
    if (shouldClose)
    {
        return;
    }
    context.callbacks.processAutomaticShaderReloads();
    if (shouldClose)
    {
        return;
    }

    commandResult.activeWorldAfterCommand =
        subsystems.GetWorldManager().GetActiveWorldHandle();

    if (uiSubsystem != nullptr &&
        (activeWorldBeforeCommand.generation !=
             commandResult.activeWorldAfterCommand.generation ||
         activeWorldBeforeCommand.scenePath !=
             commandResult.activeWorldAfterCommand.scenePath))
    {
        uiSubsystem->NotifyWorldChanged(
            commandResult.activeWorldAfterCommand.scenePath,
            commandResult.activeWorldAfterCommand.generation);
        if (editorHost)
        {
            const auto& world = subsystems.GetWorldManager().GetActiveWorld();
            // 草稿跨 World 保留；只在换代后重放，不重复入文档，不修改观察相机。
            if (world)
                editorHost->ReplaySceneEditsAfterWorldChange(
                    commandResult.activeWorldAfterCommand.scenePath,
                    commandResult.activeWorldAfterCommand.generation,
                    *world);
            else
                editorHost->ResetAuthoringSessions();
        }
    }

    runtimeTests.NotifyCommandResult(
        commandResult,
        subsystems.GetDiagnosticsSubsystem());

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

    const float deltaTime = subsystems.GetRuntimeClock().TickDeltaSeconds();
    // UE 用 MaxUndilatedFrameTime/MaxSimulationTimeStep 防止 hitch 一帧推进整段：
    // 游戏/动画只消费上限 0.1 s 的模拟时间，FPS 与资产轮询仍用原始帧间隔。
    constexpr float MaxSimulationDelta = 0.1f;
    const float simulationDelta = std::min(deltaTime, MaxSimulationDelta);

    // input 由两段不连续区间组成（平台事件轮询 + 动作状态更新），其余 UI 策略
    // 与命令执行归入 other；同一阶段的多个区间在累加器里相加。
    {
        CpuFrameStageScope inputStage(cpuFrameProfiler, CpuFrameStage::Input);
        PumpEnginePlatformEvents(context.uiActions, context.windowLifecycle);
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
            subsystems.GetDiagnosticsSubsystem().ReportError(
                std::string("Cannot query drawable size after swapchain out-of-date: ") +
                SDL_GetError());
            exitCode = 2;
            shouldClose = true;
            return;
        }
        if (drawableWidth > 0 && drawableHeight > 0)
        {
            // RT 已在上一帧归还所有权；SDL resize 事件可晚于 present 的失效结果。
            auto resizeResult = RecreateEngineRendererForWindowResize(
                context.windowLifecycle,
                static_cast<uint32_t>(drawableWidth),
                static_cast<uint32_t>(drawableHeight));
            if (resizeResult.IsFailure())
            {
                subsystems.GetDiagnosticsSubsystem().ReportRuntimeError(
                    "Swapchain out-of-date recovery failed", resizeResult.Error());
                exitCode = 2;
                shouldClose = true;
                return;
            }
        }
    }
    UpdateEngineUiInputPolicy(context.uiActions);

    runtimeTests.UpdateEngineLoopTests(
        *runtimeValidationServices,
        subsystems.GetDiagnosticsSubsystem());
    if (shouldClose)
    {
        return;
    }

    {
        CpuFrameStageScope inputStage(cpuFrameProfiler, CpuFrameStage::Input);
        subsystems.GetInputSubsystem().UpdateActionState();
    }

    RunEngineUpdatePhase(context, deltaTime, simulationDelta);

    // 即使当前没有 World，也要持续补音频流，避免设备欠载造成爆音。
    if (audioRuntime) audioRuntime->Update();

    if (editorHost != nullptr)
    {
        // 先在 GT 稳定帧边界处理 editor/preview，再生成 UI 与 World snapshot。
        const auto& activeWorld =
            subsystems.GetWorldManager().GetActiveWorld();
        editorHost->Tick(simulationDelta, activeWorld.get());
    }
    {
        CpuFrameStageScope uiStage(cpuFrameProfiler, CpuFrameStage::UiViewModel);
        // UI 视图模型包含编辑器快照收集与 SetPerformanceSnapshot；面板自身按 5 Hz
        // 读冻结副本，不在这里重写 DOM。
        context.callbacks.updateUiViewModel(simulationDelta, commandResult);
    }
    context.callbacks.synchronizeSceneViewport();
    if (editorHost != nullptr)
        editorHost->TickThumbnails(platformEvents);

    if (terrainChunkStreamer != nullptr && rendererBackend != nullptr)
    {
        const auto& streamingWorld =
            subsystems.GetWorldManager().GetActiveWorld();
        if (streamingWorld)
        {
            if (const auto camera = streamingWorld->GetRenderCamera())
            {
                PROFILE_SCOPE("TerrainChunkStreaming");
                terrainChunkStreamer->Update(
                    *streamingWorld,
                    *rendererBackend,
                    RendererResourceCache::GetInstance(),
                    camera->GetPosition());
            }
        }
    }

    RunEngineRenderSubmitPhase(context);

    // 事件回读放在 Render 之后：GPU 事件缓冲来自刚提交的帧，且分线程模式
    // 已在上面的 WaitForRenderThreadIdle 归还所有权。10 Hz 节流只在 Play
    // 且有 C# 粒子订阅时生效，避免普通帧进入 WaitIdle。
    if (const auto& particleWorld =
            subsystems.GetWorldManager().GetActiveWorld())
    {
        const bool playing = subsystems.GetScriptRuntime().IsPlaying();
        if (playing)
            *context.particleEventTimeSeconds += simulationDelta;
        else
            *context.particleEventTimeSeconds = 0.0;
        const bool interested = particleWorld->IsParticleEventCaptureActive();
        if (context.particleEventPollClock->Consume(
                deltaTime, playing, interested))
            context.pollParticleRuntimeBridge(*particleWorld);
    }
    else
    {
        context.particleEventPollClock->Reset();
        *context.particleEventTimeSeconds = 0.0;
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
        subsystems.GetFpsTool().Calculate(deltaTime);
        window->SetTitle(subsystems.GetFpsTool().getTitle());
    }

    context.callbacks.tickPerformanceJsonl();
    context.callbacks.tickUiSectionStats();

    PROFILE_FRAME();

    // 即使关闭 UI 也推进帧边界；Motion/Console/脚本共用同一生命周期。
    if (const auto& world = subsystems.GetWorldManager().GetActiveWorld())
        world->GetDebugDraw().EndFrame();
}

} // namespace VL
