#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
#include <cmath>
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include "engine/blueprintPreviewRequest.h"
#include "script/scriptComponents.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
using Json = nlohmann::json;

size_t ComponentIndex(const Json& draft, const std::string& id)
{
    if (!draft.contains("components")) return 0;
    const auto& nodes = draft.at("components");
    for (size_t index = 0; index < nodes.size(); ++index)
        if (ScriptComponentId(nodes[index]) == id) return index;
    return nodes.size();
}

bool InComponentSubtree(const Json& draft, const std::string& id, const std::string& root)
{
    auto current = id;
    while (!current.empty())
    {
        if (current == root) return true;
        const auto index = ComponentIndex(draft, current);
        if (index >= draft.at("components").size()) return false;
        current = draft.at("components")[index].value("parent", "");
    }
    return false;
}

BlueprintGizmoMode GizmoMode(const std::string& tool)
{
    return tool == "rotate" ? BlueprintGizmoMode::Rotate :
        tool == "scale" ? BlueprintGizmoMode::Scale : BlueprintGizmoMode::Translate;
}

const char* GizmoField(const std::string& tool)
{
    return tool == "rotate" ? "rotation" : tool == "scale" ? "scale" : "position";
}

BlueprintViewportGizmo ComponentGizmo(const Json& draft, size_t index,
    const EditorSceneCameraActor& camera, float width, float height, const std::string& tool)
{
    if (index >= draft.value("components", Json::array()).size())
    {
        if (tool == "move") return {};
        auto gizmo = BuildBlueprintViewportGizmo(camera, width, height, Eigen::Matrix4f::Identity(),
            Eigen::Quaternionf::Identity(), Eigen::Vector3f::Zero(), ScriptVector(draft, "rotation"),
            ScriptVector(draft, "scale", 1), GizmoMode(tool));
        if (tool == "rotate" && draft.contains("physics") &&
            draft["physics"].value("motion", "") == "character")
            for (size_t axis : {0u, 2u}) { gizmo.axes[axis].visible = false; gizmo.axes[axis].lines.clear(); }
        return gizmo;
    }
    const auto& node = draft.at("components").at(index);
    Eigen::Matrix4f parent = ScriptTransformMatrix(draft);
    Eigen::Quaternionf rotation = ScriptRotation(draft);
    const auto parentIndex = ComponentIndex(draft, node.value("parent", ""));
    if (parentIndex < draft.at("components").size())
    {
        parent = ScriptComponentMatrix(draft, parentIndex);
        rotation = ScriptComponentRotation(draft, parentIndex);
    }
    return BuildBlueprintViewportGizmo(camera, width, height, parent, rotation,
        ScriptVector(node, "position"), ScriptVector(node, "rotation"),
        ScriptVector(node, "scale", 1), GizmoMode(tool));
}

void BoundsLines(std::vector<EditorActorVisualLine>& lines, const EditorSceneCameraActor& camera,
    const EditorScriptComponentBounds& bounds, const Eigen::Matrix4f& delta, float width, float height)
{
    std::array<Eigen::Vector3f, 8> corners;
    for (int index = 0; index < 8; ++index)
        corners[index] = (delta * bounds.worldFromLocal * Eigen::Vector4f(
            index & 1 ? bounds.maximum.x() : bounds.minimum.x(),
            index & 2 ? bounds.maximum.y() : bounds.minimum.y(),
            index & 4 ? bounds.maximum.z() : bounds.minimum.z(), 1)).head<3>();
    for (int index = 0; index < 8; ++index)
        for (int bit : {1, 2, 4})
            if (!(index & bit))
                AddActorVisualLine(lines, camera, corners[index], corners[index | bit], width, height);
}
} // namespace

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::BlueprintPreviewActor() const
{
    const auto generation = CurrentBlueprintPreviewGeneration();
    const auto* tab = resourceEditor.ActiveTab();
    if (!generation || !tab) return nullptr;
    const auto identity = "blueprintInstance|" + BlueprintPreviewIdentity(tab->path, requestedBlueprintPreviewRevision);
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration == generation && actor.identity == identity) return &actor;
    return nullptr;
}

const EditorSceneCameraActor* MaterialInstanceAssetEditorPanel::BlueprintPreviewCamera() const
{
    const auto generation = CurrentBlueprintPreviewGeneration();
    for (const auto& actor : OutlinerActors())
        if (actor.type == EditorSceneActorType::Camera && actor.worldGeneration == generation)
            return actor.hasViewportCamera ? &actor.viewportCamera : &actor.camera;
    return nullptr;
}

bool MaterialInstanceAssetEditorPanel::BlueprintViewportEditable() const
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    const auto* actor = BlueprintPreviewActor();
    if (!document || !IsResourceEditorVisible() || !authored || !actor ||
        !runtimeView.scripts.available || runtimeView.scripts.state != ScriptPlayState::Editing ||
        runtimeView.offlineMode || HasModal() || docking.IsDragging() ||
        !pendingBlueprintPath.empty() || blueprintCommandAwaitingSnapshot) return false;
    // 图像可以短暂保留上一份权威几何，但此时禁止拾取/拖动，避免旧 bounds 写入新组件树。
    return actor->scriptComponents == authored->document.value("components", Json::array()) &&
        actor->scriptClass == authored->document.at("class") &&
        actor->rotation.isApprox(ScriptVector(authored->document, "rotation")) &&
        actor->scale.isApprox(ScriptVector(authored->document, "scale", 1));
}

void MaterialInstanceAssetEditorPanel::PublishBlueprintViewportPreview(bool reset)
{
    if (!blueprintGizmoDragging || !runtimeActionSink) return;
    UiAction action;
    action.type = UiActionType::TransformBlueprintPreview;
    auto& input = action.blueprintTransform;
    input.worldGeneration = blueprintGizmoGeneration;
    input.documentRevision = blueprintGizmoRevision;
    input.assetPath = blueprintGizmoPath;
    input.componentId = blueprintGizmoComponent;
    input.field = GizmoField(blueprintViewportTool);
    input.reset = reset;
    if (!reset)
    {
        const auto index = ComponentIndex(blueprintGizmoDraft, blueprintGizmoComponent);
        const auto& node = blueprintGizmoComponent.empty() ? blueprintGizmoDraft : blueprintGizmoDraft["components"][index];
        const auto value = ScriptVector(node, input.field.c_str(), input.field == "scale" ? 1 : 0);
        input.value = {value.x(), value.y(), value.z()};
    }
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::CancelBlueprintViewportDrag(bool restorePreview)
{
    if (restorePreview) PublishBlueprintViewportPreview(true);
    blueprintGizmoDragging = false;
    blueprintGizmoDraft = nullptr;
    blueprintGizmoPath.clear();
    blueprintGizmoComponent.clear();
    blueprintGizmoOriginalBounds.clear();
}

void MaterialInstanceAssetEditorPanel::RefreshBlueprintViewport()
{
    if (!document) return;
    if (auto* check = document->GetElementById("blueprint-snap-enabled"))
    {
        if (blueprintSnapEnabled && !check->HasAttribute("checked")) check->SetAttribute("checked", "checked");
        if (!blueprintSnapEnabled && check->HasAttribute("checked")) check->RemoveAttribute("checked");
    }
    SetControlValue("blueprint-snap-move", std::to_string(static_cast<int>(blueprintMoveSnap)));
    SetControlValue("blueprint-snap-rotate", std::to_string(static_cast<int>(blueprintRotateSnap)));
    SetControlValue("blueprint-snap-scale", blueprintScaleSnap == 0.05f ? "0.05" :
        blueprintScaleSnap == 0.1f ? "0.1" : blueprintScaleSnap == 0.25f ? "0.25" : "0.5");
    for (const char* tool : {"select", "move", "rotate", "scale"})
        widgetDocument.SetClass(std::string("blueprint-asset-tool-") + tool, "selected", blueprintViewportTool == tool);
    auto* surface = document->GetElementById("blueprint-asset-preview-surface");
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    const auto* actor = BlueprintPreviewActor();
    const auto* camera = BlueprintPreviewCamera();
    const bool editable = BlueprintViewportEditable();
    if (blueprintGizmoDragging && (!editable || authored->documentRevision != blueprintGizmoRevision ||
        tab->path != blueprintGizmoPath || CurrentBlueprintPreviewGeneration() != blueprintGizmoGeneration ||
        blueprintAssetSelectedComponent != blueprintGizmoComponent || !blueprintAssetSelectedLogicComponent.empty()))
        CancelBlueprintViewportDrag();
    blueprintViewportGizmo = {};
    std::vector<EditorActorVisualLine> boundsLines;
    std::vector<EditorActorVisualLine> helperLines;
    if (editable && surface && surface->IsVisible(true) && camera && actor)
    {
        const auto size = surface->GetBox().GetSize(Rml::BoxArea::Content);
        const auto& draft = blueprintGizmoDragging ? blueprintGizmoDraft : authored->document;
        const size_t index = ComponentIndex(draft, blueprintAssetSelectedComponent);
        // 逻辑组件没有空间变换；不能把它的空渲染 ID 当成 Blueprint Self 来显示/拖动 gizmo。
        if (blueprintAssetSelectedLogicComponent.empty() &&
            (index < draft.value("components", Json::array()).size() || blueprintAssetSelectedComponent.empty()))
        {
            if (blueprintViewportTool != "select")
                blueprintViewportGizmo = ComponentGizmo(draft, index, *camera, size.x, size.y, blueprintViewportTool);
            const auto originalIndex = ComponentIndex(authored->document, blueprintAssetSelectedComponent);
            const Eigen::Matrix4f delta = blueprintAssetSelectedComponent.empty() ?
                Eigen::Matrix4f(ScriptTransformMatrix(draft) * ScriptTransformMatrix(authored->document).inverse()) :
                Eigen::Matrix4f(ScriptComponentMatrix(draft, index) *
                    ScriptComponentMatrix(authored->document, originalIndex).inverse());
            const auto& originalBounds = blueprintGizmoDragging ? blueprintGizmoOriginalBounds : actor->scriptComponentBounds;
            for (const auto& bounds : originalBounds)
                if (blueprintAssetSelectedComponent.empty() ||
                    InComponentSubtree(draft, bounds.componentId, blueprintAssetSelectedComponent))
                    BoundsLines(boundsLines, *camera, bounds, delta, size.x, size.y);
        }
        for (const auto& helper : BuildBlueprintViewportHelpers(draft, *camera, size.x, size.y))
            helperLines.insert(helperLines.end(), helper.lines.begin(), helper.lines.end());
    }
    // 固定线段槽位只用绘制变换更新位置；环绕相机不能重新挂载整段 DOM 触发布局。
    std::string markup;
    const char* colors[] = {"x", "y", "z"};
    for (size_t axis = 0; axis < 3; ++axis)
        for (size_t index = 0; index < 48; ++index)
            markup += "<div id=\"blueprint-gizmo-" + std::to_string(axis) + "-" + std::to_string(index) +
                "\" class=\"blueprint-gizmo-line blueprint-gizmo-" + colors[axis] + "\"></div>";
    const size_t boundCapacity = actor ? actor->scriptComponentBounds.size() * 12 : 0;
    for (size_t index = 0; index < boundCapacity; ++index)
        markup += "<div id=\"blueprint-gizmo-bounds-" + std::to_string(index) +
            "\" class=\"blueprint-gizmo-line blueprint-gizmo-bounds\"></div>";
    // 各组件预留固定上界槽位，相机投影裁剪只改 visibility/transform。
    size_t helperCapacity = actor && !actor->scriptPhysics.is_null() ? 128 : 0;
    if (actor && !actor->scriptPhysics.is_null() && actor->scriptPhysics.value("shape", "") == "compound")
        helperCapacity += 128 * (actor->scriptPhysics.contains("shapes") ?
            actor->scriptPhysics.at("shapes").size() : actor->scriptComponents.size());
    if (actor)
        for (const auto& node : actor->scriptComponents)
            if (node.at("type") != "mesh") helperCapacity += 100;
    for (size_t index = 0; index < helperCapacity; ++index)
        markup += "<div id=\"blueprint-gizmo-helper-" + std::to_string(index) +
            "\" class=\"blueprint-gizmo-line blueprint-gizmo-helper\"></div>";
    SetMarkup("blueprint-asset-gizmo-overlay", std::move(markup));
    for (size_t axis = 0; axis < 5; ++axis)
    {
        const auto& lines = axis < 3 ? blueprintViewportGizmo.axes[axis].lines : axis == 3 ? boundsLines : helperLines;
        const size_t capacity = axis < 3 ? 48 : axis == 3 ? boundCapacity : helperCapacity;
        for (size_t index = 0; index < capacity; ++index)
        {
            const auto id = axis < 3 ? "blueprint-gizmo-" + std::to_string(axis) + "-" + std::to_string(index)
                : std::string(axis == 3 ? "blueprint-gizmo-bounds-" : "blueprint-gizmo-helper-") + std::to_string(index);
            const bool visible = index < lines.size();
            widgetDocument.SetProperty(id, "visibility", visible ? "visible" : "hidden");
            if (!visible) continue;
            const auto& line = lines[index];
            const Eigen::Vector2f delta = line.end - line.begin;
            widgetDocument.SetProperty(id, "transform", "translate(" +
                std::to_string(line.begin.x()) + "px, " + std::to_string(line.begin.y()) +
                "px) rotate(" + std::to_string(std::atan2(delta.y(), delta.x()) * 57.295779513f) +
                "deg) scaleX(" + std::to_string(delta.norm()) + ")");
        }
    }
}

bool MaterialInstanceAssetEditorPanel::ProcessBlueprintViewportInput(Rml::Event& event)
{
    const auto& type = event.GetType();
    if (type == "keydown" && event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
    {
        const bool active = blueprintGizmoDragging;
        CancelBlueprintViewportDrag();
        if (active) RefreshBlueprintViewport();
        return active;
    }
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::Blueprint || !IsResourceEditorVisible())
    {
        CancelBlueprintViewportDrag();
        return false;
    }
    const bool imageHit = event.GetTargetElement() == document->GetElementById("blueprint-asset-preview-image");
    const bool left = event.GetParameter<int>("button", -1) == 0;
    if (!BlueprintViewportEditable())
    {
        CancelBlueprintViewportDrag();
        return imageHit && left && (type == "mousedown" || type == "mouseup");
    }
    const auto* authored = BlueprintDocument(tab->path);
    auto* surface = document->GetElementById("blueprint-asset-preview-surface");
    const auto offset = surface->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Eigen::Vector2f pixel(event.GetParameter<float>("mouse_x", 0) - offset.x,
        event.GetParameter<float>("mouse_y", 0) - offset.y);
    if (blueprintGizmoDragging)
    {
        if (authored->documentRevision != blueprintGizmoRevision || tab->path != blueprintGizmoPath ||
            CurrentBlueprintPreviewGeneration() != blueprintGizmoGeneration)
        {
            CancelBlueprintViewportDrag();
            return true;
        }
        if (type == "mousemove" || (type == "mouseup" && left))
        {
            const auto size = surface->GetBox().GetSize(Rml::BoxArea::Content);
            if (size.x != blueprintDragGizmo.width || size.y != blueprintDragGizmo.height ||
                offset.x != blueprintGizmoOffset.x() || offset.y != blueprintGizmoOffset.y())
            {
                CancelBlueprintViewportDrag();
                RefreshBlueprintViewport();
                return true;
            }
            Eigen::Vector3f value;
            const auto index = ComponentIndex(blueprintGizmoDraft, blueprintGizmoComponent);
            const auto& previousNode = blueprintGizmoComponent.empty() ? blueprintGizmoDraft :
                blueprintGizmoDraft["components"][index];
            const float previousRotation = ScriptVector(previousNode, "rotation")[blueprintGizmoAxis];
            if (DragBlueprintViewportGizmo(blueprintDragGizmo, blueprintGizmoAxis, blueprintGizmoStart, pixel, value,
                previousRotation))
            {
                if (blueprintSnapEnabled)
                {
                    const float step = blueprintViewportTool == "move" ? blueprintMoveSnap :
                        blueprintViewportTool == "rotate" ? blueprintRotateSnap : blueprintScaleSnap;
                    Eigen::Vector3f snapped;
                    if (SnapBlueprintViewportValue(GizmoMode(blueprintViewportTool), blueprintGizmoAxis,
                        blueprintDragGizmo.initialValue, value, step, snapped)) value = snapped;
                    else value = blueprintDragGizmo.initialValue;
                }
                // 刚体根只能正统一缩放；任意轴手柄同步三个分量，组件局部缩放仍独立。
                if (blueprintGizmoComponent.empty() && blueprintViewportTool == "scale" &&
                    authored->document.contains("physics"))
                    value = Eigen::Vector3f::Constant(value[blueprintGizmoAxis]);
                blueprintGizmoDraft = authored->document;
                // 点按轴或拖回原点保持稀疏作者字段，不能把缺省零/一写成一次空编辑。
                if (!value.isApprox(blueprintDragGizmo.initialValue, 1e-6f))
                {
                    auto& node = blueprintGizmoComponent.empty() ? blueprintGizmoDraft :
                        blueprintGizmoDraft["components"][index];
                    node[GizmoField(blueprintViewportTool)] = Json::array({value.x(), value.y(), value.z()});
                }
                PublishBlueprintViewportPreview();
            }
            if (type == "mouseup")
            {
                // 手势只在释放时提交一次；失焦/Esc/切页此前都只丢弃 UI 值副本。
                Json candidate = std::move(blueprintGizmoDraft);
                CancelBlueprintViewportDrag(false);
                if (candidate != authored->document) QueueBlueprintAssetDraft(std::move(candidate));
            }
            RefreshBlueprintViewport();
            return true;
        }
        // 第二个按钮不能在变换手势中启动环绕相机。
        if (type == "mousedown" || type == "mouseup" || type == "mousescroll") return true;
    }
    if (type != "mousedown" || !imageHit || !left) return false;
    previewDragButton = -1;
    CloseMenus();
    CloseContextMenu();
    RefreshBlueprintViewport();
    if (blueprintViewportTool != "select")
        if (const auto axis = HitBlueprintViewportGizmo(blueprintViewportGizmo, pixel))
        {
            blueprintGizmoDragging = true;
            blueprintGizmoAxis = *axis;
            blueprintGizmoStart = pixel;
            blueprintGizmoOffset = Eigen::Vector2f(offset.x, offset.y);
            blueprintGizmoGeneration = CurrentBlueprintPreviewGeneration();
            blueprintGizmoRevision = authored->documentRevision;
            blueprintGizmoPath = tab->path;
            blueprintGizmoComponent = blueprintAssetSelectedComponent;
            blueprintGizmoDraft = authored->document;
            blueprintGizmoOriginalBounds = BlueprintPreviewActor()->scriptComponentBounds;
            blueprintDragGizmo = blueprintViewportGizmo;
            return true;
        }
    const auto* actor = BlueprintPreviewActor();
    const auto* camera = BlueprintPreviewCamera();
    if (!actor || !camera) return true;
    std::vector<BlueprintViewportBounds> bounds;
    for (const auto& section : actor->scriptComponentBounds)
        bounds.push_back({section.componentId, section.worldFromLocal, section.minimum, section.maximum});
    const auto size = surface->GetBox().GetSize(Rml::BoxArea::Content);
    const auto hit = PickBlueprintViewportComponent(bounds, *camera, size.x, size.y, pixel);
    blueprintAssetSelectedLogicComponent.clear();
    if (const auto helper = HitBlueprintViewportHelper(
        BuildBlueprintViewportHelpers(authored->document, *camera, size.x, size.y), pixel))
    {
        // 球/胶囊大圆的侧向投影会穿过网格中心；Self 碰撞线不抢占真实 mesh 的点击。
        blueprintAssetSelectedComponent = helper->empty() && hit ? hit->componentId : *helper;
        RefreshBlueprintAssetEditor();
        return true;
    }
    blueprintAssetSelectedComponent = hit ? hit->componentId : "";
    RefreshBlueprintAssetEditor();
    return true;
}

bool MaterialInstanceAssetEditorPanel::RequestBlueprintHistory(bool redo)
{
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::Blueprint || !IsResourceEditorVisible()) return false;
    const auto* authored = BlueprintDocument(tab->path);
    if (!authored || HasModal() || !runtimeView.scripts.available ||
        runtimeView.scripts.state != ScriptPlayState::Editing || runtimeView.offlineMode ||
        !pendingBlueprintPath.empty() || blueprintCommandAwaitingSnapshot ||
        blueprintGizmoDragging || !(redo ? authored->canRedo : authored->canUndo)) return true;
    SubmitBlueprintAssetCommand(redo ? EditorCommandType::RedoBlueprintDocument :
        EditorCommandType::UndoBlueprintDocument, tab->path);
    blueprintCommandAwaitingSnapshot = true;
    blueprintCommandRevision = authored->documentRevision;
    blueprintCommandPath = tab->path;
    for (const auto& feedback : snapshot.blueprintCommandFeedback)
        blueprintCommandFeedbackSequence = std::max(blueprintCommandFeedbackSequence, feedback.sequence);
    RefreshBlueprintHistoryControls();
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshBlueprintHistoryControls()
{
    if (!document) return;
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::Blueprint || !IsResourceEditorVisible()) return;
    const auto* authored = BlueprintDocument(tab->path);
    const bool enabled = authored && runtimeView.scripts.available && !runtimeView.offlineMode &&
        runtimeView.scripts.state == ScriptPlayState::Editing && pendingBlueprintPath.empty() &&
        !blueprintCommandAwaitingSnapshot && !blueprintGizmoDragging;
    SetDisabled("menu-edit-undo", !enabled || !authored->canUndo);
    SetDisabled("menu-edit-redo", !enabled || !authored->canRedo);
}
} // namespace VL::EditorUi
