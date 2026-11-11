#include "ui/uiSubsystem.h"
#include "ui/debugDrawOverlay.h"
#include "ui/gameplayHudView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <nlohmann/json.hpp>


#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommand.h"
#include "core/renderSession.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"
#include "ui/sceneObjectTypeSupport.h"
#include "ui/uiBindingUpdateScope.h"
#include "Profiler.h"
#include "ui/uiSubsystemInternal.h"

namespace VL
{

using namespace UiSubsystemDetail;

namespace
{

// UI 动作只声明它可能改变的视图模型分节。事件驱动路径仍然即时生效，
// 但不会让一次相机参数修改顺带重建资产/大纲/脚本三类昂贵数据。
UiSectionMask SectionsForUiAction(UiActionType type)
{
    switch (type)
    {
    case UiActionType::ToggleDeveloperUi:
    case UiActionType::SetLocale:
    case UiActionType::Quit:
        return UiSectionBit(UiSectionId::Status);
    case UiActionType::SetCameraMoveSpeed:
    case UiActionType::SetDebugViewMode:
    case UiActionType::SetToneMappingMode:
    case UiActionType::SetCameraExposure:
        return UiSectionBit(UiSectionId::ViewportCamera);
    case UiActionType::NavigateModelPreview:
    case UiActionType::ControlAnimationPreview:
    case UiActionType::ControlScriptAnimation:
    case UiActionType::ControlMotionDebug:
    case UiActionType::ControlMotionReplay:
    case UiActionType::ControlDebugDraw:
    case UiActionType::FrameEditorActor:
        return UiSectionBit(UiSectionId::ViewportCamera) |
            UiSectionBit(UiSectionId::Animation) |
            UiSectionBit(UiSectionId::SceneOutliner);
    case UiActionType::SetOfflineMode:
    case UiActionType::SetOfflineSetting:
    case UiActionType::SetOfflineAov:
    case UiActionType::SetOfflineExposure:
    case UiActionType::ControlOfflineSession:
    case UiActionType::ExportOffline:
        return UiSectionBit(UiSectionId::Offline);
    case UiActionType::SetBloomStrength:
    case UiActionType::SetBloomThreshold:
    case UiActionType::SetBloomKnee:
    case UiActionType::SetBloomClamp:
    case UiActionType::SetCsmCastShadows:
    case UiActionType::SetCsmDynamicShadowDistance:
    case UiActionType::SetCsmDynamicShadowCascades:
    case UiActionType::SetCsmCascadeDistributionExponent:
    case UiActionType::SetCsmCascadeTransitionFraction:
    case UiActionType::SetCsmShadowDistanceFadeoutFraction:
    case UiActionType::SetCsmShadowBias:
    case UiActionType::SetCsmShadowSlopeBias:
    case UiActionType::SetCsmShadowCascadeBiasDistribution:
    case UiActionType::SaveCsmSettingsToScene:
    case UiActionType::SetEnvironmentIntensity:
    case UiActionType::SetSunIntensity:
        return UiSectionBit(UiSectionId::EnvironmentLighting);
    case UiActionType::SetActorVisible:
    case UiActionType::SetActorProperty:
        return UiSectionBit(UiSectionId::SceneOutliner) |
            UiSectionBit(UiSectionId::EnvironmentLighting) |
            UiSectionBit(UiSectionId::ViewportCamera);
    case UiActionType::ControlScripts:
    case UiActionType::ReloadScripts:
    case UiActionType::CompileScripts:
    case UiActionType::SetAutoCompileScripts:
    case UiActionType::SetScriptTimeScale:
    case UiActionType::AddScriptBlueprint:
    case UiActionType::SetScriptProperty:
    case UiActionType::SetScriptComponent:
    case UiActionType::AddScriptComponent:
    case UiActionType::RemoveScriptComponent:
    case UiActionType::SetScriptLogicComponent:
    case UiActionType::AddScriptLogicComponent:
    case UiActionType::RemoveScriptLogicComponent:
    case UiActionType::RenameScriptBlueprint:
    case UiActionType::DuplicateScriptBlueprint:
    case UiActionType::DeleteScriptBlueprint:
    case UiActionType::SetScriptPhysics:
    case UiActionType::UndoSceneEdit:
    case UiActionType::RedoSceneEdit:
    case UiActionType::CreateScriptBlueprintAsset:
    case UiActionType::OpenScriptSource:
        return UiSectionBit(UiSectionId::Scripts) |
            UiSectionBit(UiSectionId::Assets) |
            UiSectionBit(UiSectionId::SceneOutliner);
    case UiActionType::LoadWorld:
    case UiActionType::PreviewModel:
    case UiActionType::PreviewRetarget:
    case UiActionType::PreviewMaterial:
    case UiActionType::PreviewBlueprint:
    case UiActionType::PreviewParticle:
    case UiActionType::UpdateParticlePreview:
    case UiActionType::TransformBlueprintPreview:
        // World/预览换代：世界路径、相机、资产与大纲都可能整体替换。
        return UiSectionAll;
    }
    return UiSectionAll;
}

} // namespace

void UiSubsystem::RequestOpenJsonAsset(const std::string& path)
{
    if (editorUiShell != nullptr)
        editorUiShell->RequestOpenJsonAsset(path);
}

bool UiSubsystem::CancelBlueprintAssetDrag()
{
    return editorUiShell != nullptr && editorUiShell->CancelBlueprintAssetDrag();
}

void UiSubsystem::RequestRefreshAssetCatalog()
{
    if (editorUiShell != nullptr)
        editorUiShell->RequestRefreshAssetCatalog();
}

void UiSubsystem::RequestRuntimeAction(UiAction action)
{
    // 工具条/参数控件产生的运行时请求只重脏它可能改变的节：相机参数不会
    // 顺带重建资产库/大纲/脚本快照。模式切换等事件仍在下一帧立即生效。
    RequestViewModelRefresh(SectionsForUiAction(action.type));
    if (action.type == UiActionType::LoadWorld)
    {
        // 冻结 World 快照保存物理绝对路径，目录条目使用资源相对路径。
        // 返回关卡仍须命中已知场景，再统一提交目录路径，不能因两种表示不同而静默丢弃。
        const std::string requestedPath = NormalizePathForComparison(action.stringValue);
        const bool immersiveRequest = !immersiveLevelPath.empty() &&
            action.stringValue == immersiveLevelPath;
        if (!immersiveRequest) immersiveLevelPath.clear();
        bool knownScene = false;
        for (const UiSceneOption& option : bindingData.sceneOptions)
        {
            if (option.path != action.stringValue && option.normalizedPhysicalPath != requestedPath)
                continue;
            if (immersiveRequest) immersiveLevelPath = option.normalizedPhysicalPath;
            action.stringValue = option.path;
            knownScene = true;
            break;
        }
        if (!knownScene || bindingData.sceneLoading)
        {
            immersiveLevelPath.clear();
            immersiveLoadCompleted = false;
            return;
        }
        bindingData.sceneLoading = true;
    }
    if (action.type == UiActionType::SetOfflineMode)
    {
        lastViewModel.offlineRequestedMode = action.intValue;
        lastViewModel.offlineUiNotice.clear();
    }
    QueueAction(std::move(action));
}
bool UiSubsystem::HandleGlobalShortcut(const PlatformEvent& event)
{
    if (event.type == PlatformEventType::KeyDown && !event.repeat)
    {
        if ((event.key == PlatformKey::F11 || event.key == PlatformKey::F1) && developerUiEnabled)
        {
            UiAction action;
            action.type = UiActionType::ToggleDeveloperUi;
            QueueAction(std::move(action));
            return true;
        }
    }

    return false;
}

RuntimeResult<void> UiSubsystem::SetEditorUiShell(IEditorUiShell* shell)
{
    if (editorUiShell == shell)
        return RuntimeResult<void>::Success();

    if (editorUiShell != nullptr)
        editorUiShell->Shutdown();
    editorUiShell = nullptr;
    // A replacement shell restarts its section counters at zero; never carry the
    // previous shell's applied versions across the detach/attach boundary.
    appliedEditorSectionVersions = UiSectionVersions{};

    if (shell == nullptr)
    {
        UpdateInputOwnership();
        return RuntimeResult<void>::Success();
    }
    if (rmlContext == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.EditorShellWithoutContext",
            "Editor UI shell requires an initialized runtime UI context."));
    }

    UiEditorShellInit init;
    init.context = rmlContext;
    init.renderInterface = &rmlRenderInterface;
    init.bridge = this;
    init.diagnostics = diagnostics;
    init.assetRoot = desc.assetRoot;
    init.sceneRoot = desc.sceneRoot;
    init.previewAdapter = desc.previewAdapter;
    init.enabled = developerUiEnabled;
    init.visible = developerUiVisible;

    RuntimeResult<void> result = shell->Initialize(init);
    if (result.IsFailure())
        return result;

    editorUiShell = shell;
    editorUiShell->Resize(desc.viewportWidth, desc.viewportHeight);
    UpdateInputOwnership();
    return RuntimeResult<void>::Success();
}

bool UiSubsystem::ProcessRmlEvent(const PlatformEvent& event)
{
    return ProcessRuntimeUiEvent(event);
}

void UiSubsystem::MarkPanelBuildMutation()
{
    uiUpdateScheduler.MarkMutation(UiMutationKind::PanelBuild);
}

void UiSubsystem::BeginDocumentValidation()
{
    rmlSystemInterface.BeginCandidateValidation();
}

bool UiSubsystem::EndDocumentValidation(std::string& outError)
{
    return rmlSystemInterface.EndCandidateValidation(outError);
}

void UiSubsystem::RecordSectionPublish(UiSectionId id)
{
    ++refreshStats.sectionPublishes;
    ++refreshStats.sectionPublishesById[static_cast<size_t>(id)];
}

void UiSubsystem::RecordPanelBuild()
{
    ++refreshStats.panelBuilds;
}

void UiSubsystem::RecordRuntimeViewUpdate()
{
    ++refreshStats.runtimeViewUpdates;
}

void UiSubsystem::ClearEditorUiShell(IEditorUiShell* shell)
{
    if (editorUiShell != shell)
        return;
    editorUiShell = nullptr;
    appliedEditorSectionVersions = UiSectionVersions{};
    UpdateInputOwnership();
}

void UiSubsystem::QueueAction(UiAction action)
{
    if (commandBus != nullptr)
    {
        commandBus->Queue(std::move(action));
    }
}

} // namespace VL
