#include "editor/ui/panel/actorMarkup.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "editor/command/editorCommand.h"
#include "editor/ui/editorDockingController.h"
#include "editor/ui/editorTextMatch.h"
#include "ui/uiColorTransform.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/navigation/tabWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"

namespace VL::EditorUi::Panel
{
namespace
{

using UiWidgets::MenuItem;
using UiWidgets::Option;
using UiWidgets::Text;

} // namespace

std::string Number(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

std::string TerrainNumber(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(6) << value;
    return stream.str();
}

std::string TerrainLayerRange(float minimum, float maximum)
{
    return TerrainNumber(minimum) + "-" + TerrainNumber(maximum);
}

std::string TerrainLayerColor(const TerrainLayerDesc& layer)
{
    const Eigen::Vector3f color = layer.avgColor;
    const float sum = std::abs(color.x()) + std::abs(color.y()) + std::abs(color.z());
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    if (std::isfinite(sum) && sum > 0.01f)
    {
        red = UiSrgbHexByte(UiLinearToSrgb(color.x()));
        green = UiSrgbHexByte(UiLinearToSrgb(color.y()));
        blue = UiSrgbHexByte(UiLinearToSrgb(color.z()));
    }
    else
    {
        // 没有可用平均色时，用层身份哈希生成稳定的低饱和色；不能每帧随机。
        const std::string& identity = layer.key.empty() ? layer.name : layer.key;
        uint32_t hash = 2166136261u;
        for (const unsigned char character : identity)
        {
            hash ^= character;
            hash *= 16777619u;
        }
        red = 72u + (hash & 63u);
        green = 78u + ((hash >> 8u) & 63u);
        blue = 68u + ((hash >> 16u) & 63u);
    }
    std::ostringstream stream;
    stream << '#' << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << red << std::setw(2) << green
        << std::setw(2) << blue << "FF";
    return stream.str();
}

std::string TerrainLayerSourceLabel(uint32_t sourceSet)
{
    return "set " + std::to_string(sourceSet);
}

bool TerrainLayerMatchesSearch(const TerrainLayerDesc& layer, const std::string& search)
{
    const std::string folded = FoldAsciiCase(search);
    if (folded.empty()) return true;
    const std::string haystack = FoldAsciiCase(layer.name + "\n" + layer.key + "\n" +
        TerrainLayerSourceLabel(layer.sourceSet) + "\nsourceSet " +
        std::to_string(layer.sourceSet));
    return haystack.find(folded) != std::string::npos;
}

std::string TerrainLayerCardMarkup(const TerrainLayerDesc& layer, const std::string& color,
    const std::string& spherePath, size_t index, bool selected, bool listView)
{
    const std::string name = layer.name.empty() ? "Unnamed Layer" : layer.name;
    const std::string meta = TerrainLayerSourceLabel(layer.sourceSet) +
        " &#183; uvScale " + TerrainNumber(layer.uvScale);
    std::string classes = "terrain-layer-card ";
    classes += listView ? "terrain-layer-list-card" : "terrain-layer-grid-card";
    if (selected) classes += " selected";
    std::string markup = "<button id=\"terrain-layer-card-" + std::to_string(index) +
        "\" class=\"" + classes + "\" data-action=\"terrain-layer-select\" data-index=\"" +
        std::to_string(index) + "\" title=\"" + Text(name) + "\">" +
        "<div class=\"terrain-layer-swatch";
    if (spherePath.empty())
    {
        markup += "\" style=\"background-color: " + color +
            ";\"><div class=\"terrain-layer-swatch-highlight\"></div>";
    }
    else
    {
        // 有真实球体图时不能保留色块背景，否则 PNG 透明角落会露出方块。
        markup += " has-image\"><img class=\"terrain-layer-swatch-image\" src=\"" +
            Text(spherePath) + "\"/>";
    }
    markup += std::string("</div>") +
        "<div class=\"terrain-layer-card-copy\">" +
        "<span class=\"terrain-layer-card-name\">" + Text(name) + "</span>" +
        "<span class=\"terrain-layer-card-meta\">" + meta + "</span>" +
        "</div></button>";
    return markup;
}

int TerrainLayerGridColumns(float viewportWidth)
{
    if (!std::isfinite(viewportWidth) || viewportWidth <= 0.0f) return 1;
    return std::max(1, static_cast<int>(std::floor(
        viewportWidth / TerrainLayerGridCellWidth)));
}

// 右键菜单的标题行与材质槽条目都只用资产文件名兜底，不把完整路径当标签。
std::string FileLabel(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string ActorPropertyRowMarkup(const EditorSceneActorEditableField& field)
{
    std::vector<UiWidgets::NumericComponent> components;
    for (uint32_t component = 0; component < field.components; ++component)
    {
        UiWidgets::NumericComponent entry;
        entry.id = SceneActorPropertyControlId(field.property, component);
        static const char* axes[] = {"X", "Y", "Z"};
        static const char* channels[] = {"R", "G", "B"};
        entry.label = field.components == 1 ? "" :
            (field.property == EditorActorProperty::Color ? channels[component] : axes[component]);
        entry.value = SceneActorNumber(field.value[component]);
        entry.change = {"actor-property", static_cast<size_t>(field.property), component};
        entry.hasRange = field.hasRange;
        entry.minValue = field.minValue;
        entry.maxValue = field.maxValue;
        components.push_back(std::move(entry));
    }
    return "<div class=\"actor-property-row\" id=\"" +
        SceneActorPropertyControlId(field.property, 0) + "-row\">" +
        UiWidgets::PropertyControlRow(field.label, UiWidgets::NumericComponents(components)) + "</div>";
}

std::string ActorCountLabel(size_t count)
{
    return std::to_string(count) + (count == 1 ? " actor" : " actors");
}

UiWidgets::TreeRowData OutlinerActorRow(size_t actorIndex, const EditorSceneActorSnapshot& actor,
    bool selected, bool expanded, bool hasChildren)
{
    UiWidgets::TreeRowData row;
    row.label = actor.displayName;
    row.select = {"outliner-actor", actorIndex};
    row.toggle = {"outliner-expand", actorIndex};
    row.selected = selected;
    row.hasChildren = hasChildren;
    row.expanded = expanded;
    row.icon = SceneActorGroupIcon(actor.type);
    row.rowClass = "actor-row";
    // 眼睛只出现在真正有渲染侧可见性状态的对象上；隐藏时行同时得到 hidden-row class，
    // 让"这一行代表不可见对象"在图标之外还有第二个可测信号。
    if (SceneActorHasVisibilityToggle(actor.type))
    {
        row.hiddenRow = !actor.visible;
        row.hasTrailing = true;
        row.trailing = {"outliner-visible", actorIndex};
        row.trailingIcon = actor.visible ? "icons/eye.png" : "icons/eye-off.png";
        row.trailingLabel = actor.visible ? "Hide in viewport" : "Show in viewport";
        row.trailingAlternate = !actor.visible;
    }
    return row;
}

UiWidgets::TreeRowData OutlinerSlotRow(size_t actorIndex, size_t slotIndex,
    const std::string& label, bool selected)
{
    UiWidgets::TreeRowData row;
    row.label = label;
    row.select = {"outliner-slot", actorIndex, static_cast<uint32_t>(slotIndex)};
    row.depth = 1;
    row.selected = selected;
    row.icon = "icons/material-instance.png";
    row.rowClass = "slot-row";
    return row;
}

std::string OutlinerRowsMarkup(const std::vector<EditorSceneActorRow>& rows,
    const std::vector<EditorSceneActorSnapshot>& actors,
    const std::string& selectedActorIdentity,
    std::optional<uint32_t> selectedMaterialSlotIndex,
    const EditorSceneActorWindow& window)
{
    std::string markup;
    if (window.top > 0.0f)
        markup += "<div class=\"outliner-spacer\" style=\"height: " +
            std::to_string(window.top) + "px;\"></div>";
    for (size_t index = window.begin; index < window.end && index < rows.size(); ++index)
    {
        const EditorSceneActorRow& row = rows[index];
        if (row.actorIndex >= actors.size()) continue;
        const EditorSceneActorSnapshot& actor = actors[row.actorIndex];
        const bool selected = actor.identity == selectedActorIdentity;
        if (row.isSlot)
        {
            markup += UiWidgets::TreeRow(OutlinerSlotRow(row.actorIndex, row.slotIndex, row.label,
                selected && selectedMaterialSlotIndex == row.materialSlotIndex));
            continue;
        }
        markup += UiWidgets::TreeRow(OutlinerActorRow(row.actorIndex, actor, selected,
            row.expanded, row.hasChildren));
    }
    if (window.bottom > 0.0f)
        markup += "<div class=\"outliner-spacer\" style=\"height: " +
            std::to_string(window.bottom) + "px;\"></div>";
    return markup;
}

const char* DocumentStatus(EditorDocumentStatus status)
{
    switch (status)
    {
    case EditorDocumentStatus::Clean: return "Saved";
    case EditorDocumentStatus::Dirty: return "Modified";
    case EditorDocumentStatus::Saving: return "Saving";
    case EditorDocumentStatus::SaveFailed: return "Save failed";
    case EditorDocumentStatus::SourceChanged: return "Source changed";
    }
    return "";
}

const char* PreviewStatus(EditorPreviewStatus status)
{
    switch (status)
    {
    case EditorPreviewStatus::Connected: return "Connected";
    case EditorPreviewStatus::Applying: return "Applying";
    case EditorPreviewStatus::Unavailable: return "Unavailable";
    case EditorPreviewStatus::Failed: return "Failed";
    case EditorPreviewStatus::Disconnected: return "Disconnected";
    }
    return "";
}

// Window 菜单沿用停靠标签的显示名；面板集合本身仍由 DockWorkspace::Panels() 定义。
const char* PanelLabel(const std::string& panel)
{
    if (panel == "viewport") return "Viewport";
    if (panel == "outliner") return "Scene Outliner";
    if (panel == "details") return "Details";
    if (panel == "content") return "Content Browser";
    if (panel == "renderer") return "Renderer";
    if (panel == "log") return "Log";
    if (panel == "terrain") return "Landscape";
    if (panel == "performance") return "Performance";
    return "Asset Editor";
}

std::string ResourcePanelId(const std::string& path)
{
    // 资源标签的身份在当前会话内必须稳定，关闭前一个标签时不能重命名仍停靠在其他区域的标签。
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char value : path)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    std::ostringstream stream;
    stream << "resource:" << std::hex << hash;
    return stream.str();
}

std::string MaterialPanelId(const std::string& path)
{
    return "resource:material:" + ResourcePanelId(path).substr(9);
}

std::string RenderStateOptions(const std::string& name)
{
    std::string result;
    if (name == "renderMode")
    {
        for (EditorMaterialRenderMode mode : {
            EditorMaterialRenderMode::Opaque, EditorMaterialRenderMode::OpaqueClip,
            EditorMaterialRenderMode::ForwardOpaque,
            EditorMaterialRenderMode::TransparentAlphaBlend,
            EditorMaterialRenderMode::TransparentAlphaBlendWriteDepth,
            EditorMaterialRenderMode::TransparentAdditive,
            EditorMaterialRenderMode::ThinTranslucent})
        {
            const std::string value(GetEditorMaterialRenderModeName(mode));
            result += Option(value, value);
        }
    }
    else if (name == "cullMode")
    {
        for (EditorMaterialCullMode mode : {
            EditorMaterialCullMode::Back, EditorMaterialCullMode::Front,
            EditorMaterialCullMode::None})
        {
            const std::string value(GetEditorMaterialCullModeName(mode));
            result += Option(value, value);
        }
    }
    else if (name == "shadingModel")
    {
        for (EditorMaterialShadingModel model : {
            EditorMaterialShadingModel::DefaultLit, EditorMaterialShadingModel::Unlit,
            EditorMaterialShadingModel::Subsurface, EditorMaterialShadingModel::PreintegratedSkin,
            EditorMaterialShadingModel::ClearCoat, EditorMaterialShadingModel::SubsurfaceProfile,
            EditorMaterialShadingModel::TwoSidedFoliage, EditorMaterialShadingModel::Hair,
            EditorMaterialShadingModel::Cloth, EditorMaterialShadingModel::Eye,
            EditorMaterialShadingModel::ThinTranslucent})
        {
            const std::string value(GetEditorMaterialShadingModelName(model));
            result += Option(value, value);
        }
    }
    return result;
}

std::string BuildWindowMenuMarkup(const EditorDockingController& docking)
{
    std::string markup;
    const auto& panels = UiWidgets::DockWorkspace::Panels();
    for (size_t index = 0; index < panels.size(); ++index)
    {
        markup += MenuItem("menu-window-" + panels[index], {"menu-window", index},
            PanelLabel(panels[index]), docking.IsPanelVisible(panels[index]), false);
    }
    return markup;
}

std::string ContextMenuItemsMarkup(const ContextMenuMarkupInput& input)
{
    std::string markup;
    if (input.kind == ContextMenuKind::Actor)
    {
        const EditorSceneActorSnapshot* actor = input.actor;
        if (actor == nullptr) return markup;
        // 可见性开关只对真正持有渲染侧可见性状态的对象给出命令；相机/环境/风剖面没有这一
        // 状态，因此与大纲行尾的眼睛同一规则：不显示条目，也不放一个点了没有反应的假条目。
        if (SceneActorHasVisibilityToggle(actor->type))
        {
            markup += MenuItem("context-outliner-visible",
                {"context-outliner-visible", 0},
                actor->visible ? "Hide in Viewport" : "Show in Viewport", false, false);
        }
        // mesh 的材质槽走与点击槽行完全相同的选择路由（打开 MI 文档并同步视口描边）。
        const MaterialInstanceModelSnapshot model = MeshSceneActorModel(*actor);
        for (size_t index = 0; index < model.materials.size(); ++index)
        {
            const auto& material = model.materials[index];
            markup += MenuItem("context-actor-material-" + std::to_string(index),
                {"context-actor-material", index},
                "Open " + (material.materialSlotName.empty()
                    ? FileLabel(material.materialInstancePath) : material.materialSlotName),
                false, false);
        }
    }
    else if (input.kind == ContextMenuKind::BlueprintClass)
    {
        markup += MenuItem("context-class-create", {"context-class-create", 0},
            "Create Blueprint Asset...", false, !input.scriptsAvailable ||
                input.scriptState != ScriptPlayState::Editing);
        markup += MenuItem("context-class-source", {"context-class-source", 0},
            "Open in VS Code", false, false);
    }
    else if (input.kind == ContextMenuKind::BrowserAsset)
    {
        markup += MenuItem("context-browser-open", {"context-browser-open", 0},
            "Open", false, false);
        if (input.target != nullptr && input.modifiedDocuments != nullptr)
            for (const auto& asset : *input.modifiedDocuments)
                if (asset.assetPath == *input.target)
                    markup += MenuItem("context-browser-save", {"context-browser-save", 0},
                        "Save...", false, false);
    }
    else if (input.kind == ContextMenuKind::Viewport)
    {
        // 与视口工具条共用 SetDebugViewMode：菜单只多给出勾选状态，没有第二条 UI→renderer 路径。
        // 条目下标就是 debug view 取值（工具条的 view mode 也这样寻址）。
        markup += MenuItem("context-view-lit", {"context-view-mode", 0},
            "Lit", input.debugViewMode == 0, false);
        markup += MenuItem("context-view-unlit", {"context-view-mode", 1},
            "Unlit", input.debugViewMode == 1, false);
        markup += "<div class=\"menu-separator\"></div>";
        markup += MenuItem("context-save-all", {"context-save-all", 0},
            "Save Modified Assets...", false,
            input.modifiedDocuments == nullptr || input.modifiedDocuments->empty());
    }
    return markup;
}

bool FindCatalogAssetKind(const MaterialInstanceEditorSnapshot& snapshot, const std::string& path,
    EditorAssetKind& kind, std::string& name)
{
    if (snapshot.assetCatalog != nullptr)
    {
        for (const auto& asset : snapshot.assetCatalog->assets)
        {
            if (asset.assetPath != path) continue;
            kind = asset.kind;
            name = asset.name.empty() ? FileLabel(path) : asset.name;
            return true;
        }
        return false;
    }
    for (const auto& asset : snapshot.assets)
    {
        if (asset.assetPath != path) continue;
        kind = EditorAssetKind::MaterialInstance;
        name = FileLabel(path);
        return true;
    }
    return false;
}

} // namespace VL::EditorUi::Panel
