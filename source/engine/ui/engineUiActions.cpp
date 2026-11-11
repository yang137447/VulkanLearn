#include "engine/ui/engineUiActions.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "controller.h"
#include "engine/editorHost.h"
#include "engine/modelPreviewCamera.h"
#include "engine/modelPreviewRotation.h"
#include "engine/runtimeCommand.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/script/engineScriptBuild.h"
#include "engine/subsystemCollection.h"
#include "engine/window/engineWindowLifecycle.h"
#include "input/mouseMotionBatch.h"
#include "platform/platformApplication.h"
#include "platform/platformWindow.h"
#include "profiler.h"
#include "render/frontend/scenePicking.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "script/scriptBuildService.h"
#include "script/scriptRuntime.h"
#include "ui/uiSubsystem.h"
#include "world/world.h"

namespace VL
{

void ApplyQueuedEngineUiActions(
    const EngineUiActionContext& context,
    const EngineScriptBuildContext& scriptBuildContext)
{
    SubsystemCollection& subsystems = *context.subsystems;
    IEditorHost* const& editorHost = context.editorHost;
    UiSubsystem* const& uiSubsystem = context.uiSubsystem;
    Controller* const& controller = context.controller;
    ModelPreviewCamera& modelPreviewCamera = *context.modelPreviewCamera;
    ScriptBuildService* const& scriptBuild = context.scriptBuild;
    bool& autoCompileScripts = *context.autoCompileScripts;
    std::optional<std::filesystem::path>& pendingBuiltAssembly =
        *context.pendingBuiltAssembly;
    bool& shouldClose = *context.shouldClose;
    std::vector<UiAction> actions = subsystems.GetCommandBus().DrainUiActions();
    // 同一 drain 内所有 ApplyBrush 先只改 CPU session；End/Cancel/Undo/Redo
    // 只置脏标记，循环结束后最多重建一次 GPU 网格。该累积状态归编辑器 host。
    RuntimeCommandExecutionResult editorActionResult;
    if (editorHost) editorHost->BeginUiActionBatch();
    for (const UiAction& action : actions)
    {
        World* activeWorld = subsystems.GetWorldManager().GetActiveWorld().get();
        if (editorHost && editorHost->ApplyUiAction(action, activeWorld, editorActionResult))
            continue;
        if (action.type == UiActionType::CompileScripts || action.type == UiActionType::SetAutoCompileScripts)
        {
            const auto& world = subsystems.GetWorldManager().GetActiveWorld();
            auto& scripts = subsystems.GetScriptRuntime();
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
            auto& scripts = subsystems.GetScriptRuntime();
            const auto& world = subsystems.GetWorldManager().GetActiveWorld();
            std::string error;
            if (!world || world->GetGeneration() != action.actorProperty.worldGeneration)
                error = "The World changed since this time request";
            else if (RenderSystem::GetInstance().IsOfflineMode() ||
                std::filesystem::path(world->GetScenePath()).filename() == "SC_model_preview.json")
                error = "Time Scale requires a realtime level";
            else scripts.SetTimeScale(action.floatValue, error);
            scripts.SetFeedback(error);
            if (!error.empty()) subsystems.GetDiagnosticsSubsystem().ReportWarning(error, "Script");
            continue;
        }
        if (action.type == UiActionType::ControlScripts ||
            action.type == UiActionType::ReloadScripts)
        {
            auto& scripts = subsystems.GetScriptRuntime();
            const auto& world = subsystems.GetWorldManager().GetActiveWorld();
            std::string error;
            if (!world || world->GetGeneration() != action.actorProperty.worldGeneration)
                error = "The World changed since this script request";
            else if (RenderSystem::GetInstance().IsOfflineMode() ||
                std::filesystem::path(world->GetScenePath()).filename() == "SC_model_preview.json")
                error = "C# Actors require a realtime level";
            else if (action.type == UiActionType::ControlScripts)
            {
                // 同批Stop/Play也必须先移除上一上下文的动态模板，不能把它们当成新场景常驻资产。
                if (scripts.IsEditing()) TickEngineBlueprintAssets(scriptBuildContext);
                scripts.Control(*world, static_cast<ScriptPlayState>(action.intValue), error);
                // Play/Stop 都会切换编辑上下文；CPU 高度副本不能跨状态继续用。
                if (editorHost) editorHost->ResetAuthoringSessions();
                if (scripts.IsEditing()) TickEngineBlueprintAssets(scriptBuildContext);
            }
            else if (action.type == UiActionType::ReloadScripts)
            {
                if (scripts.IsEditing()) TickEngineBlueprintAssets(scriptBuildContext);
                scripts.ReloadAssembly(*world, error, pendingBuiltAssembly ?
                    pendingBuiltAssembly->string() : std::string());
                pendingBuiltAssembly.reset();
                if (error.empty())
                {
                    subsystems.GetDiagnosticsSubsystem().ReportInfo("Gameplay assembly reloaded", "Script");
                    continue;
                }
            }
            scripts.SetFeedback(error);
            if (!error.empty()) subsystems.GetDiagnosticsSubsystem().ReportWarning(error);
            continue;
        }
        if (action.type == UiActionType::SetActorProperty && !subsystems.GetScriptRuntime().IsEditing())
        {
            subsystems.GetScriptRuntime().SetFeedback("Stop gameplay before editing the scene");
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
            subsystems.GetDiagnosticsSubsystem().ReportInfo(
                "Camera move speed set to " + std::to_string(action.floatValue) + " cm/s");
            continue;
        }

        if (action.type == UiActionType::NavigateModelPreview)
        {
            const auto& world = subsystems.GetWorldManager().GetActiveWorld();
            const auto rect = uiSubsystem ? uiSubsystem->GetSceneViewportRect() : UiViewportRect{};
            if (world && rect.width && rect.height)
                modelPreviewCamera.Apply(*world, action.previewCamera,
                    float(rect.width) / float(rect.height), float(rect.width));
            continue;
        }
        RuntimeCommand command;
        command.sourceText = "ui";
        switch (action.type)
        {
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
            const auto& world = subsystems.GetWorldManager().GetActiveWorld();
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
        case UiActionType::SetTressfxGroupPreview:
            command.type = RuntimeCommandType::SetTressfxGroupPreview;
            command.actorProperty.worldGeneration = action.previewTressfxGroup.worldGeneration;
            command.tressfxSelectedGroup = action.previewTressfxGroup.selectedGroup;
            command.tressfxGroupFlags = action.previewTressfxGroup.flags;
            break;
        case UiActionType::SetTressfxGroupBindingPreview:
            command.type = RuntimeCommandType::SetTressfxGroupBindingPreview;
            command.actorProperty.worldGeneration =
                action.previewTressfxGroupBinding.worldGeneration;
            command.tressfxGroupBindingPreview.objectName =
                action.previewTressfxGroupBinding.objectName;
            command.tressfxGroupBindingPreview.worldGeneration =
                action.previewTressfxGroupBinding.worldGeneration;
            command.tressfxGroupBindingPreview.draftRevision =
                action.previewTressfxGroupBinding.draftRevision;
            command.tressfxGroupBindingPreview.guideGroupIds =
                action.previewTressfxGroupBinding.guideGroupIds;
            command.tressfxGroupBindingPreview.groupNames =
                action.previewTressfxGroupBinding.groupNames;
            command.tressfxGroupBindingPreview.bindingKey =
                action.previewTressfxGroupBinding.bindingKey;
            break;
        case UiActionType::SetTressfxBrushPreview:
            command.type = RuntimeCommandType::SetTressfxBrushPreview;
            command.actorProperty.worldGeneration =
                action.previewTressfxBrush.worldGeneration;
            command.tressfxBrushPreview.objectName = action.previewTressfxBrush.objectName;
            command.tressfxBrushPreview.worldGeneration =
                action.previewTressfxBrush.worldGeneration;
            command.tressfxBrushPreview.targetGroupId =
                action.previewTressfxBrush.targetGroupId;
            command.tressfxBrushPreview.mode = action.previewTressfxBrush.mode;
            command.tressfxBrushPreview.weights = action.previewTressfxBrush.weights;
            command.tressfxBrushPreview.ringVisible = action.previewTressfxBrush.ringVisible;
            command.tressfxBrushPreview.ringCenter = action.previewTressfxBrush.ringCenter;
            command.tressfxBrushPreview.ringNormal = action.previewTressfxBrush.ringNormal;
            command.tressfxBrushPreview.ringRadiusWorld = action.previewTressfxBrush.ringRadiusWorld;
            break;
        default:
            continue;
        }
        subsystems.GetCommandBus().Queue(std::move(command));
    }

    if (editorHost)
        editorHost->EndUiActionBatch(
            subsystems.GetWorldManager().GetActiveWorld().get());

    // RmlUi 场景列表只提交值语义 selection 请求；在这里统一落到 renderer，
    // 确保选择描边与常规视口拾取走同一个稳定帧边界。
    if (uiSubsystem != nullptr)
    {
        const std::optional<VL::MaterialInstanceModelContext>
            modelSelection =
                uiSubsystem->ConsumeMaterialInstanceModelSelectionRequest();
        if (modelSelection.has_value())
        {
            RenderSystem::GetInstance().SetSelectedMaterialInstanceModel(
                modelSelection.value());
        }

        const std::optional<VL::MaterialInstanceSelection>
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

void UpdateEngineUiInputPolicy(const EngineUiActionContext& context)
{
    SubsystemCollection& subsystems = *context.subsystems;
    UiSubsystem* const& uiSubsystem = context.uiSubsystem;
    Controller* const& controller = context.controller;
    PlatformWindow* const& window = context.window;
    const auto& world = subsystems.GetWorldManager().GetActiveWorld();
    if (world && world->GetCamera())
    {
        // 资产预览已经有自己的 orbit 相机；只有关卡编辑使用独立观察相机。
        const bool editingLevel = uiSubsystem && uiSubsystem->IsDeveloperUiVisible() &&
            subsystems.GetScriptRuntime().IsEditing() &&
            std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json";
        world->SetEditorViewEnabled(editingLevel);
        controller->SetViewTarget(editingLevel ? world->GetRenderCamera() :
            world->GetViewTarget().lock(), world->GetGeneration());
    }
    auto& inputSubsystem = subsystems.GetInputSubsystem();
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

void PumpEnginePlatformEvents(
    const EngineUiActionContext& context,
    const EngineWindowLifecycleContext& windowLifecycle)
{
    SubsystemCollection& subsystems = *context.subsystems;
    IEditorHost* const& editorHost = context.editorHost;
    UiSubsystem* const& uiSubsystem = context.uiSubsystem;
    PlatformApplication* const& platformApplication = context.platformApplication;
    PlatformWindow* const& window = context.window;
    std::vector<PlatformEvent>& platformEvents = *context.platformEvents;
    bool& shouldClose = *context.shouldClose;
    int& exitCode = *context.exitCode;
    PROFILE_SCOPE("Events");

    platformApplication->PollEvents(platformEvents);
    // 高回报率鼠标不能让相机移动重复触发整棵 UI 命中测试；普通 UI 拖动保留原事件。
    if (subsystems.GetInputSubsystem().IsRelativeMouseModeEnabled())
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
                auto& inputSubsystem = subsystems.GetInputSubsystem();
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
                subsystems.GetDiagnosticsSubsystem().ReportInfo(message);
            }
            continue;
        }

        if (event.type == PlatformEventType::MouseButtonDown &&
            event.mouseButton == PlatformMouseButton::Left && uiSubsystem != nullptr &&
            !subsystems.GetInputSubsystem().IsRelativeMouseModeRequested() &&
            !subsystems.GetInputSubsystem().IsRelativeMouseModeEnabled() &&
            uiSubsystem->IsScenePickTarget(event.mouseX, event.mouseY))
        {
            const std::optional<VL::ScenePickRequest> pickRequest =
                VL::BuildScenePickRequest(
                    event,
                    uiSubsystem->GetViewportWidth(),
                    uiSubsystem->GetViewportHeight());
            if (pickRequest.has_value())
            {
                const auto point = uiSubsystem->GetSceneViewportPoint(pickRequest->mouseX, pickRequest->mouseY);
                if (!point) continue;
                const std::optional<VL::MaterialInstanceSelection>
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
                        editorHost != nullptr &&
                        editorHost->SelectSceneObjectFromPick(*selection);
                    subsystems.GetDiagnosticsSubsystem().ReportInfo(
                        selected
                            ? "Selected scene object '" + selection->objectIdentity + "'."
                            : "Selected scene object '" +
                                selection->objectIdentity +
                                "' but the editor could not update its selection.");
                }
                else
                {
                    RenderSystem::GetInstance().ClearSelectedMaterialInstance();
                    if (editorHost != nullptr)
                        editorHost->ClearMaterialInstanceSceneSelection();
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
            subsystems.GetDiagnosticsSubsystem().ReportInfo(
                "Window resized to " +
                std::to_string(event.width) +
                "x" +
                std::to_string(event.height) +
                " (drawable " +
                std::to_string(drawableWidth) +
                "x" +
                std::to_string(drawableHeight) +
                ")");
            auto resizeResult = RecreateEngineRendererForWindowResize(
                windowLifecycle,
                static_cast<int>(drawableWidth), static_cast<int>(drawableHeight));
            if (resizeResult.IsFailure())
            {
                subsystems.GetDiagnosticsSubsystem().ReportRuntimeError(
                    "Window resize failed",
                    resizeResult.Error());
                exitCode = 2;
                shouldClose = true;
            }
            continue;
        }

        if (event.type == PlatformEventType::Quit)
        {
            subsystems.GetDiagnosticsSubsystem().ReportInfo("Quit event received");
            shouldClose = true;
        }
    }
}

} // namespace VL
