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
namespace
{
using namespace Panel::Detail;
} // namespace

void MaterialInstanceAssetEditorPanel::CloseDocument(
    const std::string& path, EditorDirtyDocumentPolicy policy)
{
    if (ParticleDocument(path))
    {
        if (policy == EditorDirtyDocumentPolicy::DiscardChanges)
            SubmitParticleAssetCommand(
                EditorCommandType::RevertParticleDocument, path);
        SubmitParticleAssetCommand(
            EditorCommandType::CloseParticleDocument, path);
        resourceEditor.Close(path);
        resourceEditorVisible = resourceEditor.ActiveTab() != nullptr;
        RefreshResourceEditor();
        if (!resourceEditorVisible) ReturnToLevel();
        return;
    }
    if (TressfxDocument(path))
    {
        if (policy == EditorDirtyDocumentPolicy::DiscardChanges)
            SubmitResourceDocumentCommand(EditorCommandType::RevertTressfxDocument, path);
        CloseResource(path);
        return;
    }
    if (BlueprintDocument(path))
    {
        if (policy == EditorDirtyDocumentPolicy::DiscardChanges)
            SubmitBlueprintAssetCommand(EditorCommandType::RevertBlueprintDocument, path);
        CloseResource(path);
        return;
    }
    closingMaterialPath = path;
    panelAfterMaterialClose = docking.PanelAfterClose(MaterialPanelId(path));
    EditorCommandEnvelope command;
    command.type = EditorCommandType::CloseMaterialInstanceAsset;
    command.payload = CloseMaterialInstanceAssetPayload{path, policy};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::SelectModelMaterial(
    const MaterialInstanceModelSnapshot& modelSnapshot, size_t index)
{
    ShowMaterialDetails();
    if (sceneSelectionSink == nullptr || index >= modelSnapshot.materials.size()) return;
    FlushPendingEdits();
    const auto model = SelectionModel(modelSnapshot);
    const auto& slot = model.materials[index];
    Editor::Selection::MaterialInstanceSelection selection;
    selection.worldGeneration = model.worldGeneration;
    selection.scenePath = model.scenePath;
    selection.objectId = slot.objectId;
    selection.objectIdentity = model.objectIdentity;
    selection.materialSlotIndex = slot.materialSlotIndex;
    selection.materialSlotName = slot.materialSlotName;
    selection.materialInstancePath = slot.materialInstancePath;
    selection.modelContext = model;
    sceneSelectionSink->RequestMaterialSelection(selection);
}

void MaterialInstanceAssetEditorPanel::HandleClick(Rml::Element& element, const std::string& action)
{
    Panel::HandlePanelClick(*this, element, action);
}

void MaterialInstanceAssetEditorPanel::HandleChange(Rml::Element& element, const std::string& action,
    const Rml::Event* event)
{
    Panel::HandlePanelChange(*this, element, action, event);
}

void MaterialInstanceAssetEditorPanel::ProcessEvent(Rml::Event& event)
{
    if (refreshingControls || document == nullptr) return;
    const auto& eventType = event.GetType();
    if (eventType == "mousescroll")
    {
        // 滚轮默认滚动发生在捕获之后；这里只标脏，下一次 Build 再按新的 scrollTop 重算窗口。
        auto* layerScroll = document->GetElementById("terrain-layer-scroll");
        for (auto* element = event.GetTargetElement(); element != nullptr;
            element = element->GetParentNode())
        {
            if (element == layerScroll)
            {
                terrainLayerWindowDirty = true;
                break;
            }
        }
    }
    if (ProcessViewportShortcut(event))
    {
        event.StopPropagation();
        return;
    }
    if (eventType == "mousemove")
        RefreshTerrainLayerHoverPreview(event.GetUnprojectedMouseScreenPos(), event.GetTargetElement());
    auto* sceneImage = document->GetElementById("editor-viewport-image");
    auto* sceneSurface = document->GetElementById("editor-viewport-surface");
    if (IsTerrainSculptActive() && sceneImage != nullptr &&
        (event.GetTargetElement() == sceneImage ||
         event.GetTargetElement() == sceneSurface) &&
        (eventType == "mousedown" || eventType == "mousemove" || eventType == "mouseup"))
    {
        const int button = eventType == "mousemove" ? -1 :
            event.GetParameter<int>("button", -1);
        if (ProcessTerrainSculptPointer(eventType, button,
            event.GetParameter<float>("mouse_x", 0.0f),
            event.GetParameter<float>("mouse_y", 0.0f)))
        {
            event.StopPropagation();
            return;
        }
    }
    if (event.GetType() == "keydown" && !HasModal())
    {
        const int key = event.GetParameter<int>("key_identifier", 0);
        const bool ctrl = event.GetParameter<int>("ctrl_key", 0) != 0;
        if (ctrl && key == Rml::Input::KI_Z && IsActiveHairPreview() &&
            !hairPaintUndo.empty())
        {
            UndoLastHairBrushCommit();
            event.StopPropagation();
            return;
        }
        if (key == Rml::Input::KI_ESCAPE &&
            (hairGroupPaintActive || hairBrushStrokeActive))
        {
            CancelHairBrushStroke(true);
            event.StopPropagation();
            return;
        }
        if (hairGroupPaintActive && IsActiveHairPreview())
        {
            if (key == Rml::Input::KI_RETURN && hairBrushStrokeActive)
            {
                if (hairLassoDragging) CommitHairLassoStroke();
                else CommitHairBrushStroke();
                event.StopPropagation();
                return;
            }
            if (key == Rml::Input::KI_OEM_4 || key == Rml::Input::KI_OEM_6)
            {
                const float delta = key == Rml::Input::KI_OEM_6 ? 2.0f : -2.0f;
                hairBrushRadiusPx = std::clamp(hairBrushRadiusPx + delta, 2.0f, 500.0f);
                SetControlValue("hair-brush-radius", SceneActorNumber(hairBrushRadiusPx));
                hairBrushStatus = "Brush radius: " +
                    SceneActorNumber(hairBrushRadiusPx) + " px.";
                if (hairBrushStamp.valid)
                {
                    if (hairLassoDragging) SampleHairBrushStamp(hairBrushCursorX, hairBrushCursorY);
                    else UpdateHairBrushSample(hairBrushCursorX, hairBrushCursorY,
                        hairBrushStrokeActive);
                }
                event.StopPropagation();
                return;
            }
        }
    }
    // Groups 的行内表单是面板本地状态：Esc 只取消当前表单，不触发文档/World 命令。
    if (event.GetType() == "keydown" && !HasModal() &&
        event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
    {
        if (hairGroupCreateActive || hairGroupAddRangeActive)
        {
            hairGroupCreateActive = false;
            hairGroupCreateName.clear();
            hairGroupCreateStart.clear();
            hairGroupCreateEnd.clear();
            hairGroupCreateError.clear();
            hairGroupAddRangeActive = false;
            hairGroupRangeStart.clear();
            hairGroupRangeEnd.clear();
            hairGroupRangeError.clear();
            hairGroupStatus = "Group form cancelled.";
            event.StopPropagation();
            return;
        }
    }
    if (ProcessSceneGizmoInput(event))
    {
        event.StopPropagation();
        return;
    }
    if (ProcessBlueprintAssetDrag(event)) return;
    if (ProcessParticleKeyboard(event))
    {
        event.StopPropagation();
        return;
    }
    if (ProcessBlueprintViewportInput(event) || ProcessTexturePreviewInput(event) || ProcessModelPreviewInput(event))
    {
        event.StopPropagation();
        return;
    }
    // 属性行的文本框较窄，用户点标签/行空白时也应该进入编辑，而不是只选中行。
    Rml::Element* eventTarget = event.GetTargetElement();
    if (event.GetType() == "click" && eventTarget != nullptr)
    {
        Rml::Element* row = eventTarget;
        while (row != nullptr && row != document)
        {
            const std::string id = row->GetId();
            if (id.size() > 4 && id.compare(id.size() - 4, 4, "-row") == 0)
            {
                if (Rml::Element* input = FirstActorTextInput(*row))
                    input->Focus();
                break;
            }
            row = row->GetParentNode();
        }
    }
    if (event.GetType() == "change") runtimeControlsCurrent = false;
    // 点击菜单栏以外的任何位置都收起已展开的菜单；菜单项自身的收起在下面按 action 处理。
    if (event.GetType() == "click" && !openMenu.empty() && !IsInsideMenuBar(event.GetTargetElement()))
        CloseMenus();
    // 上下文菜单同一规则：点弹层以外收起，条目自身的收起在动作派发之后。
    if (event.GetType() == "click" && IsContextMenuOpen() &&
        !IsInsideContextMenu(event.GetTargetElement()))
        CloseContextMenu();
    if (docking.ProcessEvent(event, HasModal()))
    {
        const std::string activatedPanel = docking.ConsumeActivatedPanel();
        ActivateWorkspacePanel(activatedPanel);
        event.StopPropagation();
        return;
    }
    // 右键抬起在 RmlUi 里只是带 button=1 的 mouseup（没有 contextmenu 事件），
    // 因此必须在这里拦下来：下面的分支只处理 click/change/dblclick，右键会直接掉出去。
    if (event.GetType() == "mouseup" && event.GetParameter<int>("button", 0) == 1)
    {
        // 有模态弹窗时不打开菜单：About 与脏关闭弹窗存在期间工作区命令一律不可用。
        if (!HasModal()) OpenContextMenu(event);
        event.StopPropagation();
        return;
    }
    // 捕获阶段只处理工作区拖动；普通控件的鼠标默认行为必须继续到目标节点。
    if (event.GetType() != "click" && event.GetType() != "change" && event.GetType() != "dblclick") return;
    Rml::Element* element = event.GetTargetElement();
    // select 内部输入会先发 change，再由 select 发自己的 change；仅处理外部控件事件。
    if (event.GetType() == "change" && element && !element->HasAttribute("data-action")) return;
    while (element != nullptr && element != document && !element->HasAttribute("data-action"))
        element = element->GetParentNode();
    if (element == nullptr || element == document || element->HasAttribute("disabled")) return;
    const std::string action = element->GetAttribute<std::string>("data-action", "");
    if (HasModal() && action != "close-save" && action != "close-discard" &&
        action != "close-cancel" && action != "about-close" &&
        action != "particle-revert-confirm" &&
        action != "particle-revert-cancel" &&
        action != "save-file" && action != "save-confirm" && action != "save-cancel" &&
        action != "save-select-all")
        return;
    // checkbox 在目标节点的默认 click 中切换 checked；捕获阶段不能截断它。
    // 随后的 change 才把选择同步到弹窗状态，程序刷新仍由 WidgetRefreshScope 抑制。
    if (event.GetType() == "click" && (action == "save-file" || action == "script-physics-trigger" ||
        action == "blueprint-asset-instance" || action == "blueprint-asset-physics-trigger" ||
        action == "blueprint-asset-component-instance" || action == "blueprint-asset-snap-enabled" ||
        action == "viewport-snap-enabled" ||
        action == "log-autoscroll" || action == "retarget-all-bones" || action == "retarget-zoom" ||
        action == "animation-clip" || action == "animation-time" ||
        action == "motion-debug" || action == "debug-draw-enabled" || action == "debug-draw-category" ||
        action == "debug-draw-owner")) return;
    // 选中菜单条目后收起菜单；标题自己决定展开还是收起，不能在这里先清空。
    if (action != "menu-open" && action.rfind("debug-draw-", 0) != 0 && IsInsideMenuBar(element)) CloseMenus();
    try
    {
        std::optional<std::string> openAssetPath;
        const auto* activeResource = resourceEditor.ActiveTab();
        if (IsResourceEditorVisible() && activeResource &&
            activeResource->kind == EditorAssetKind::Retarget && retargetEditor.ProcessEvent(event))
        {
            RefreshResourceEditor();
            event.StopPropagation();
            return;
        }
        if (logPanel.ProcessEvent(event))
        {
            event.StopPropagation();
            return;
        }
        if (resourceJsonInspector.ProcessEvent(event))
        {
            event.StopPropagation();
            return;
        }
        if (jsonInspector.ProcessEvent(event))
        {
            event.StopPropagation();
            return;
        }
        if (contentBrowser.ProcessEvent(event, openAssetPath))
        {
            if (openAssetPath) OpenBrowserAsset(*openAssetPath);
        }
        else if (event.GetType() == "click") HandleClick(*element, action);
        else if (event.GetType() == "change") HandleChange(*element, action, &event);
        else if (event.GetType() == "dblclick" &&
            (action == "outliner-actor" || action == "viewport-actor") && runtimeActionSink != nullptr)
        {
            const size_t index = element->GetAttribute<size_t>("data-index", 0);
            if (index < OutlinerActors().size())
            {
                const auto& actor = OutlinerActors()[index];
                if (!SceneActorEditableFields(actor).empty())
                {
                    UiAction request;
                    request.type = UiActionType::FrameEditorActor;
                    request.actorProperty.worldGeneration = actor.worldGeneration;
                    request.actorProperty.identity = actor.identity;
                    runtimeActionSink->RequestRuntimeAction(std::move(request));
                }
            }
        }
        // 条目派发完成后才收起菜单：动作本身不读取"菜单是否打开"这个状态。
        // 禁用条目在上面就已返回，因此菜单保持打开（与菜单栏的禁用条目一致）。
        if (event.GetType() == "click" && IsInsideContextMenu(element)) CloseContextMenu();
    }
    catch (const std::exception& error)
    {
        localStatus = error.what();
    }
    event.StopPropagation();
}

} // namespace VL::EditorUi
