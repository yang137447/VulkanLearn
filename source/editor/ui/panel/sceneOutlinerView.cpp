#include "editor/ui/panel/sceneOutlinerView.h"

#include <algorithm>
#include <cmath>

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include "editor/ui/editorActorVisualization.h"
#include "editor/ui/panel/actorMarkup.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi::Panel
{
namespace
{

using UiWidgets::Text;
using Panel::Number;

} // namespace

std::string OutlinerMountedVisuals(const OutlinerViewContext& context)
{
    const std::vector<EditorSceneActorSnapshot>& actors = OutlinerActors(*context.snapshot);
    std::string visuals = context.snapshot->selectedActorIdentity + "\n" +
        (context.snapshot->selectedMaterialSlotIndex
            ? std::to_string(*context.snapshot->selectedMaterialSlotIndex) : std::string());
    for (size_t index = context.mountedWindow->begin; index < context.mountedWindow->end; ++index)
    {
        if (index >= context.rows->size()) break;
        const EditorSceneActorRow& row = (*context.rows)[index];
        if (row.isSlot || row.actorIndex >= actors.size()) continue;
        const EditorSceneActorSnapshot& actor = actors[row.actorIndex];
        if (!SceneActorHasVisibilityToggle(actor.type)) continue;
        visuals += "\n" + actor.identity + (actor.visible ? "=1" : "=0");
    }
    return visuals;
}

void RefreshActorVisualization(const OutlinerViewContext& context)
{
    if (*context.document == nullptr) return;
    UiWidgets::WidgetDocument& widgetDocument = *context.widgetDocument;
    widgetDocument.SetClass("viewport-actor-visuals", "selected", context.showActorVisualization);
    auto* surface = (*context.document)->GetElementById("editor-viewport-surface");
    if (surface == nullptr || !surface->IsVisible(true)) return;
    const auto size = surface->GetBox().GetSize(Rml::BoxArea::Content);
    const auto& actors = OutlinerActors(*context.snapshot);
    const EditorSceneCameraActor* view = nullptr;
    for (const auto& actor : actors)
        if (actor.type == EditorSceneActorType::Camera)
        {
            view = actor.hasViewportCamera ? &actor.viewportCamera : &actor.camera;
            break;
        }
    std::string linesMarkup;
    std::string iconsMarkup;
    std::vector<EditorActorVisualLine> lines;
    if (context.showActorVisualization && context.sceneEditing &&
        view && size.x > 0 && size.y > 0)
    {
        // 图标是固定像素大小；只有选中对象才绘制辅助线，避免灯具多时淹没场景。
        // 网格/环境/场景风没有可投影的世界位置，不摆视口图标。
        for (size_t index = 0; index < actors.size(); ++index)
        {
            const auto& actor = actors[index];
            if (actor.type == EditorSceneActorType::Mesh ||
                actor.type == EditorSceneActorType::Environment ||
                actor.type == EditorSceneActorType::SceneWind)
                continue;
            const bool selected = actor.identity == context.snapshot->selectedActorIdentity;
            if (selected && actor.visible)
            {
                lines = BuildActorVisualLines(actor, *view, size.x, size.y);
                // 按未裁剪的图示拓扑挂载固定槽位；跨近面/屏幕边界只隐藏槽位，不重新布局。
                size_t capacity = 0;
                switch (actor.type)
                {
                case EditorSceneActorType::PointLight: capacity = 3 * 24; break;
                case EditorSceneActorType::SpotLight: capacity = 2 * (32 + 4); break;
                case EditorSceneActorType::DirectionalLight: capacity = 3; break;
                case EditorSceneActorType::Camera: capacity = 8; break;
                case EditorSceneActorType::SquareAreaLight: capacity = 5; break;
                case EditorSceneActorType::Script:
                    capacity = actor.scriptPhysics.is_null() ? 0 :
                        actor.scriptPhysics.value("shape", "box") == "box" ? 12 : 100;
                    break;
                default: break;
                }
                for (size_t line = 0; line < capacity; ++line)
                    linesMarkup += "<div id=\"actor-visual-line-" + std::to_string(line) +
                        "\" class=\"actor-visual-line\"></div>";
            }
            // 结构只含 actor 身份/事件地址；位置与屏幕内外状态绝不能进入 markup 缓存键。
            iconsMarkup += "<button id=\"actor-visual-icon-" + std::to_string(index) +
                "\" class=\"actor-visual-icon\" data-generation=\"" +
                std::to_string(actor.worldGeneration) + "\" data-action=\"viewport-actor\" data-index=\"" + std::to_string(index) +
                "\" title=\"" + Text(actor.displayName) + "\"><img src=\"" +
                Text(SceneActorGroupIcon(actor.type)) + "\"/></button>";
        }
    }
    // RmlUi 布局失效会遍历整份编辑器文档；固定盒只更新绘制 transform/visibility，
    // 既避免相机移动触发整页文字测量，也保留图标的指针双击目标身份。
    widgetDocument.SetMarkup("viewport-actor-lines", std::move(linesMarkup));
    widgetDocument.SetMarkup("viewport-actor-icons", std::move(iconsMarkup));
    auto* lineLayer = (*context.document)->GetElementById("viewport-actor-lines");
    for (int index = 0; index < lineLayer->GetNumChildren(); ++index)
    {
        const std::string id = "actor-visual-line-" + std::to_string(index);
        const bool visible = static_cast<size_t>(index) < lines.size();
        widgetDocument.SetProperty(id, "visibility", visible ? "visible" : "hidden");
        if (!visible) continue;
        const auto& line = lines[index];
        const Eigen::Vector2f delta = line.end - line.begin;
        const float angle = std::atan2(delta.y(), delta.x()) * 180.0f / 3.14159265358979323846f;
        widgetDocument.SetProperty(id, "transform",
            "translate(" + Number(line.begin.x()) + "px, " + Number(line.begin.y()) +
            "px) rotate(" + Number(angle) + "deg) scaleX(" + Number(delta.norm()) + ")");
    }
    Rml::ElementList icons;
    (*context.document)->GetElementById("viewport-actor-icons")->QuerySelectorAll(icons, "[data-action=viewport-actor]");
    for (auto* icon : icons)
    {
        const size_t index = icon->GetAttribute<size_t>("data-index", actors.size());
        const bool selected = index < actors.size() && actors[index].identity == context.snapshot->selectedActorIdentity;
        const std::string id = icon->GetId();
        widgetDocument.SetClass(id, "selected", selected);
        const auto& actor = actors[index];
        Eigen::Vector2f pixel;
        // Terrain 是编辑器身份，不是灯；World 里它只有 chunk mesh 根位置，
        // 不能用默认零值的 light.position 冒充地形坐标。
        const Eigen::Vector3f position = actor.type == EditorSceneActorType::Camera
            ? actor.camera.position
            : (actor.type == EditorSceneActorType::Script ||
                actor.type == EditorSceneActorType::Terrain) ? actor.position : actor.light.position;
        const bool visible = actor.visible && view &&
            ProjectActorVisualPoint(*view, position, size.x, size.y, pixel);
        widgetDocument.SetProperty(id, "visibility", visible ? "visible" : "hidden");
        if (visible)
            widgetDocument.SetProperty(id, "transform",
                "translate(" + Number(pixel.x() - 12) + "px, " + Number(pixel.y() - 12) + "px)");
    }
}

void RefreshOutlinerView(const OutlinerViewContext& context)
{
    // 面板可能在 RML 热重载里处于 Detach 状态，而 UiSubsystem 仍逐帧 Build：
    // 这里必须先确认文档存在，窗口计算会直接读元素几何。
    if (*context.document == nullptr) return;
    UiWidgets::WidgetDocument& widgetDocument = *context.widgetDocument;
    const std::vector<EditorSceneActorSnapshot>& actors = OutlinerActors(*context.snapshot);
    // World 换代时展开集合、搜索与选中态都不再指向同一个对象，统一回到初始状态。
    // 排序是面板偏好，与 Content Browser 的排序键一样跨目录/换代保留。
    const uint64_t generation = actors.empty() ? 0 : actors.front().worldGeneration;
    if (*context.treeWorldGeneration != generation)
    {
        context.expansion->expandedActors.clear();
        context.query->clear();
        *context.typeFilter = "all";
        *context.treeWorldGeneration = generation;
        // 换代后旧的稳定身份不再指向任何对象：已打开的右键菜单连同它的目标一起收起。
        if (context.closeContextMenu) context.closeContextMenu();
        widgetDocument.SetControlValue("outliner-search", std::string());
        widgetDocument.SetControlValue("outliner-type", *context.typeFilter);
    }
    widgetDocument.SetControlValue("outliner-sort", SceneActorSortOrderKey(*context.sortOrder));

    // 行集合只在真正变化时重建：actor 内容签名 + 搜索/过滤/排序/展开折叠状态。
    // 静止帧因此不重新排序，也不重新分配行标签。
    if (OutlinerRowsNeedRebuild(context, actors)) RebuildOutlinerRows(context, actors);

    // 行高先按上一帧已挂载的行实测（首帧读不到布局时保持 RCSS 兜底），
    // 再由滚动偏移与可视高度算出本帧窗口，与 Content Browser 的 VisibleWindow 同序。
    MeasureOutlinerRowHeight(context);
    const EditorSceneActorWindow window = OutlinerWindow(context);
    // 选择态不重挂 DOM：RmlUi 的双击要求两次 click 的目标节点身份保持不变。
    const std::string visuals = OutlinerMountedVisuals(context);
    if (!*context.mounted ||
        *context.mountedRevision != *context.rowsRevision ||
        !SameSceneActorWindow(*context.mountedWindow, window))
    {
        widgetDocument.SetMarkup("scene-models", OutlinerRowsMarkup(*context.rows, actors,
            context.snapshot->selectedActorIdentity,
            context.snapshot->selectedMaterialSlotIndex, window));
        *context.mounted = true;
        *context.mountedRevision = *context.rowsRevision;
        *context.mountedWindow = window;
        *context.mountedVisuals = visuals;
    }
    else if (*context.mountedVisuals != visuals)
    {
        RefreshOutlinerVisuals(context);
        *context.mountedVisuals = visuals;
    }

    // 计数只统计 actor；展开的材质槽不增加 actor 数。
    size_t visibleActors = 0;
    for (const EditorSceneActorRow& row : *context.rows)
        if (!row.isSlot) ++visibleActors;
    const size_t total = actors.size();
    widgetDocument.SetMarkup("outliner-count", Text(visibleActors == total
        ? ActorCountLabel(total)
        : std::to_string(visibleActors) + " / " + ActorCountLabel(total)));
    const std::string emptyMessage = context.rows->empty()
        ? (total == 0 ? "No actors in the active world."
                      : "No actors match the current filter.")
        : std::string();
    widgetDocument.SetMarkup("outliner-empty", Text(emptyMessage));
    // 空状态时收起树容器：flex:1 的空容器会把提示文字顶到面板底部。
    widgetDocument.SetProperty("outliner-empty", "display",
        emptyMessage.empty() ? "none" : "block");
    widgetDocument.SetProperty("scene-models", "display",
        emptyMessage.empty() ? "block" : "none");
}

void RefreshOutlinerVisuals(const OutlinerViewContext& context)
{
    const auto& actors = OutlinerActors(*context.snapshot);
    Rml::Element* list = (*context.document)->GetElementById("scene-models");
    size_t rowIndex = context.mountedWindow->begin;
    for (int index = 0; index < list->GetNumChildren(); ++index)
    {
        Rml::Element* element = list->GetChild(index);
        if (!element->IsClassSet("tree-row") || rowIndex >= context.rows->size()) continue;
        const auto& row = (*context.rows)[rowIndex++];
        const auto& actor = actors[row.actorIndex];
        const bool selected = actor.identity == context.snapshot->selectedActorIdentity &&
            (!row.isSlot || context.snapshot->selectedMaterialSlotIndex == row.materialSlotIndex);
        auto* label = element->QuerySelector(".list-item");
        if (label && label->IsClassSet("selected") != selected) label->SetClass("selected", selected);
        if (row.isSlot || !SceneActorHasVisibilityToggle(actor.type)) continue;
        if (element->IsClassSet("hidden-row") != !actor.visible)
            element->SetClass("hidden-row", !actor.visible);
        auto* eye = element->QuerySelector("[data-action=outliner-visible]");
        if (!eye) continue;
        if (eye->IsClassSet("alternate") == !actor.visible) continue;
        eye->SetClass("alternate", !actor.visible);
        const std::string hint = actor.visible ? "Hide in viewport" : "Show in viewport";
        eye->SetAttribute("title", hint);
        eye->SetAttribute("aria-label", hint);
        eye->GetChild(0)->SetAttribute("src", actor.visible ? "icons/eye.png" : "icons/eye-off.png");
    }
}

bool OutlinerRowsNeedRebuild(const OutlinerViewContext& context,
    const std::vector<EditorSceneActorSnapshot>& actors)
{
    return SceneActorRowsSignature(actors) != *context.rowSignature ||
        *context.query != *context.builtQuery ||
        *context.typeFilter != *context.builtTypeFilter ||
        *context.sortOrder != *context.builtSortOrder ||
        !SameSceneActorExpansion(*context.expansion, *context.builtExpansion);
}

void RebuildOutlinerRows(const OutlinerViewContext& context,
    const std::vector<EditorSceneActorSnapshot>& actors)
{
    *context.rows = BuildSceneActorRows(
        actors, *context.query, *context.typeFilter, *context.sortOrder, *context.expansion);
    *context.rowSignature = SceneActorRowsSignature(actors);
    *context.builtQuery = *context.query;
    *context.builtTypeFilter = *context.typeFilter;
    *context.builtSortOrder = *context.sortOrder;
    *context.builtExpansion = *context.expansion;
    ++*context.rowsRevision;
}

EditorSceneActorWindow OutlinerWindow(const OutlinerViewContext& context)
{
    auto* list = (*context.document)->GetElementById("scene-models");
    const auto& box = list->GetBox();
    const float paddingY = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) +
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom);
    const float clientHeight = list->GetClientHeight() - paddingY;
    const float scroll = list->GetScrollTop() -
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top);
    return ComputeSceneActorWindow(
        context.rows->size(), clientHeight, scroll, *context.rowHeight);
}

// 树行高度由 RCSS 统一决定（.tree-row 的内容是 .tree-toggle 与 .list-item），
// 这里读已挂载行的 margin box 校正兜底值——正如 Content Browser 读首个可见条目。
// 首帧或刚替换的 DOM 还没有布局，读到 0 时保持上一次的值。
void MeasureOutlinerRowHeight(const OutlinerViewContext& context)
{
    if (*context.document == nullptr) return;
    auto* list = (*context.document)->GetElementById("scene-models");
    for (int index = 0; index < list->GetNumChildren(); ++index)
    {
        Rml::Element* child = list->GetChild(index);
        if (!child->IsClassSet("tree-row")) continue;
        const auto measured = child->GetBox().GetSize(Rml::BoxArea::Margin);
        if (measured.y > 0.0f) *context.rowHeight = measured.y;
        return;
    }
}

const std::vector<EditorSceneActorSnapshot>& OutlinerActors(
    const MaterialInstanceEditorSnapshot& snapshot)
{
    return SceneActorList(snapshot.sceneActors);
}

const EditorSceneActorSnapshot* FindSelectedActor(const MaterialInstanceEditorSnapshot& snapshot)
{
    if (snapshot.selectedActorIdentity.empty()) return nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors(snapshot))
        if (actor.identity == snapshot.selectedActorIdentity) return &actor;
    return nullptr;
}

const EditorSceneActorSnapshot* FindPreviewWindActor(const MaterialInstanceEditorSnapshot& snapshot)
{
    for (const EditorSceneActorSnapshot& actor : OutlinerActors(snapshot))
        if (actor.type == EditorSceneActorType::SceneWind &&
            actor.identity == "wind|PreviewWind")
            return &actor;
    return nullptr;
}

const EditorSceneActorSnapshot* FindContextMenuActor(const std::string& target,
    const MaterialInstanceEditorSnapshot& snapshot)
{
    if (target.empty()) return nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors(snapshot))
        if (actor.identity == target) return &actor;
    return nullptr;
}

void ToggleContextActorVisibility(const std::string& target,
    const MaterialInstanceEditorSnapshot& snapshot,
    const std::function<void(UiAction)>& requestRuntimeAction,
    std::string& localStatus)
{
    if (!requestRuntimeAction) return;
    const EditorSceneActorSnapshot* actor = FindContextMenuActor(target, snapshot);
    if (actor == nullptr)
    {
        // 目标只在打开菜单时存在过（World 换代、对象改名）：拒绝执行并说明原因，
        // 不退回面板的当前选择，否则会悄悄改到另一个对象。
        localStatus = "The context menu target is no longer in the active world.";
        return;
    }
    UiAction request;
    request.type = UiActionType::SetActorVisible;
    request.stringValue = actor->identity;
    request.intValue = actor->visible ? 0 : 1;
    requestRuntimeAction(std::move(request));
}

void OpenContextActorMaterial(const std::string& target, size_t slotIndex,
    const MaterialInstanceEditorSnapshot& snapshot,
    const std::function<void(const MaterialInstanceModelSnapshot&, size_t)>& selectModelMaterial,
    std::string& localStatus)
{
    const EditorSceneActorSnapshot* actor = FindContextMenuActor(target, snapshot);
    if (actor == nullptr)
    {
        localStatus = "The context menu target is no longer in the active world.";
        return;
    }
    const MaterialInstanceModelSnapshot model = MeshSceneActorModel(*actor);
    if (slotIndex >= model.materials.size())
    {
        localStatus = "The context menu target no longer has that material slot.";
        return;
    }
    if (selectModelMaterial) selectModelMaterial(model, slotIndex);
}

void OpenContextBrowserAsset(const std::string& target,
    const MaterialInstanceEditorSnapshot& snapshot,
    const std::function<void(const std::string&)>& openBrowserAsset,
    std::string& localStatus)
{
    const std::string path = target;
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    std::string name;
    if (path.empty() || !FindCatalogAssetKind(snapshot, path, kind, name))
    {
        localStatus = "The context menu target is no longer in the asset catalog.";
        return;
    }
    if (openBrowserAsset) openBrowserAsset(path);
}

} // namespace VL::EditorUi::Panel
