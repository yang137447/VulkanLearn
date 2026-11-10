#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "animation/motionDebug.h"
#include "debug/drawDebug.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>

#include "core/renderSession.h"
#include "editor/brush/brushProfile.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/editorTextMatch.h"
#include "editor/ui/editorViewportAxisIndicator.h"
#include "editor/ui/editorActorVisualization.h"
#include "editor/ui/scriptComponentControls.h"
#include "editor/ui/tressfxGroupEditor.h"
#include "hair/tressfxGroupBinding.h"
#include "editor/ui/tressfxHairSimulationControls.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupPreviewState.h"
#include "editor/ui/scriptPhysicsControls.h"
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptTypeIdentity.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"
#include "ui/widgets/navigation/tabWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include "ui/uiColorTransform.h"


namespace VL::EditorUi
{
using namespace Panel::Detail;

void MaterialInstanceAssetEditorPanel::RequestScriptEdit(UiActionType type, nlohmann::json payload)
{
    const auto* actor = SelectedActor();
    if (!runtimeActionSink || !actor || actor->type != EditorSceneActorType::Script || actor->scriptSpawned ||
        runtimeView.scripts.state != ScriptPlayState::Editing || runtimeView.offlineMode) return;
    if (!actor->scriptBlueprintPath.empty() && (type == UiActionType::SetScriptPhysics ||
        type == UiActionType::AddScriptComponent || type == UiActionType::RemoveScriptComponent)) return;
    UiAction request;
    request.type = type;
    request.actorProperty.worldGeneration = actor->worldGeneration;
    request.actorProperty.identity = actor->identity;
    // RmlUi 的 select 可在一次 SetValue 中嵌套派发两个相同 change；
    // 只在这轮冻结快照内去重，下一轮允许对被拒绝的请求重试。
    const auto signature = std::to_string(static_cast<int>(type)) + "|" + actor->identity + "|" + payload.dump();
    if (!scriptEditRequests.insert(signature).second) return;
    request.script = std::move(payload);
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

void MaterialInstanceAssetEditorPanel::OpenBrowserAsset(const std::string& path)
{
    const auto className = contentBrowser.BlueprintClass(path);
    if (!className.empty())
    {
        OpenBlueprintClassSource(className);
        return;
    }
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    if (snapshot.assetCatalog != nullptr)
    {
        bool found = false;
        for (const auto& asset : snapshot.assetCatalog->assets)
        {
            if (asset.assetPath != path) continue;
            kind = asset.kind;
            found = true;
            break;
        }
        if (!found) return;
    }
    FlushPendingEdits();
    if (kind == EditorAssetKind::MaterialInstance)
    {
        ShowMaterialDetails();
        EditorCommandEnvelope command;
        command.type = EditorCommandType::OpenMaterialInstanceAsset;
        command.payload = OpenMaterialInstanceAssetPayload{path, std::nullopt};
        Submit(std::move(command));
    }
    else if (kind == EditorAssetKind::Mesh || kind == EditorAssetKind::Tressfx || kind == EditorAssetKind::Texture ||
        kind == EditorAssetKind::Terrain || kind == EditorAssetKind::Blueprint ||
        kind == EditorAssetKind::Retarget)
    {
        OpenResourceAsset(path, kind);
    }
    else
    {
        resourceEditorVisible = false;
        if (kind == EditorAssetKind::Scene && runtimeActionSink != nullptr)
        {
            // 关卡双击复用运行时场景选择器的 LoadWorld 事务；UI 不直接触碰 World，
            // 同时继续请求 JSON inspector，让用户能查看本次切换目标的元数据。
            UiAction request;
            request.type = UiActionType::LoadWorld;
            request.stringValue = path;
            levelWorldPath = path;
            requestedPreviewPath.clear();
            runtimeActionSink->RequestRuntimeAction(std::move(request));
            // 场景双击切换 World 后，关卡编辑标签是当前主工作区；Details 只保留资产元数据。
            docking.SetPanelVisible("viewport", true);
            docking.Activate("viewport");
        }
        // 非关卡 JSON 只读查看，不导入模型或贴图；MI 文档事务保持独立。
        if (jsonAssetRequestSink == nullptr) return;
        viewingJson = true;
        requestedJsonPath = path;
        localStatus.clear();
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
        if (kind != EditorAssetKind::Scene) docking.Activate("details");
    }
}

void MaterialInstanceAssetEditorPanel::OpenBlueprintClassSource(const std::string& className)
{
    for (const auto& type : runtimeView.scripts.classes)
    {
        if (type.at("name") != className) continue;
        if (!runtimeActionSink) return;
        UiAction action;
        action.type = UiActionType::OpenScriptSource;
        action.stringValue = className;
        runtimeActionSink->RequestRuntimeAction(std::move(action));
        return;
    }
    localStatus = "The C# class is no longer registered.";
}

void MaterialInstanceAssetEditorPanel::BeginBlueprintClassAsset(const std::string& className)
{
    for (const auto& type : runtimeView.scripts.classes)
    {
        if (type.at("name") != className) continue;
        scriptPlacementVisible = scriptPlacementCreateAsset = true;
        docking.SetPanelVisible("outliner", true);
        docking.Activate("outliner");
        RefreshScriptControls();
        const auto separator = className.rfind('.');
        const auto name = className.substr(separator == std::string::npos ? 0 : separator + 1);
        SetControlValue("script-class", className);
        SetControlValue("script-placement-mode", "create");
        SetControlValue("script-name", name);
        SetControlValue("script-new-path", "Common/Blueprints/BP_" + name + ".json");
        return;
    }
    localStatus = "The C# class is no longer registered.";
}

bool MaterialInstanceAssetEditorPanel::ProcessBlueprintAssetDrag(Rml::Event& event)
{
    const auto& type = event.GetType();
    if (type == "keydown" && event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
        actorDragPath.clear();
    if (type == "dragend") { actorDragPath.clear(); return true; }
    if (type != "dragstart" && type != "dragdrop") return false;
    if (HasModal() || !runtimeView.scripts.available || runtimeView.offlineMode || IsAssetEditorVisible() ||
        runtimeView.scripts.state != ScriptPlayState::Editing)
    {
        actorDragPath.clear();
        return true;
    }
    if (type == "dragstart")
    {
        const auto path = contentBrowser.BlueprintDragPath(event.GetTargetElement());
        actorDragPath = path.value_or("");
        actorDragGeneration = runtimeView.scripts.worldGeneration;
    }
    else
    {
        const auto path = std::exchange(actorDragPath, {});
        if (path.empty() || actorDragGeneration != runtimeView.scripts.worldGeneration ||
            !IsSceneViewportElement(event.GetTargetElement()) || runtimeActionSink == nullptr) return true;
        // 原生 RmlUi dragdrop 只携带指针供本次事件使用；投递给 GT 的 payload 只保留路径和坐标。
        const auto source = contentBrowser.BlueprintDragPath(
            static_cast<Rml::Element*>(event.GetParameter<void*>("drag_element", nullptr)));
        if (!source || *source != path) return true;
        const auto rect = GetSceneViewportRect();
        const float x = event.GetParameter<float>("mouse_x", -1);
        const float y = event.GetParameter<float>("mouse_y", -1);
        if (rect.width <= 0 || rect.height <= 0 || x < rect.x || y < rect.y ||
            x >= rect.x + rect.width || y >= rect.y + rect.height) return true;
        UiAction request;
        request.type = UiActionType::AddScriptBlueprint;
        request.actorProperty.worldGeneration = actorDragGeneration;
        request.script = {{"blueprintPath", path},
            {"name", std::filesystem::path(path).stem().string()},
            {"viewport", {(x - rect.x) / rect.width, (y - rect.y) / rect.height}}};
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    return true;
}


} // namespace VL::EditorUi
