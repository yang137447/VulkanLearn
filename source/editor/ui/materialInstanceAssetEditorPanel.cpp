#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "animation/motionDebug.h"
#include "debug/drawDebug.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
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
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/editorTextMatch.h"
#include "editor/ui/editorViewportAxisIndicator.h"
#include "editor/ui/editorActorVisualization.h"
#include "editor/ui/scriptComponentControls.h"
#include "editor/ui/tressfxHairSimulationControls.h"
#include "editor/ui/scriptPhysicsControls.h"
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptTypeIdentity.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"
#include "ui/widgets/navigation/tabWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"

namespace VL::EditorUi
{
namespace
{

using UiWidgets::Text;
using UiWidgets::Option;
using UiWidgets::WidgetAction;
using UiWidgets::WidgetRefreshScope;

// 目录条目给 UI 一个同步的“这是不是 tressfx、有没有 simulation.skinning”判定，
// 使打开资产时能立刻决定是否请求预览；快照到达后再作为权威来源复核。
const EditorJsonAssetEntry* FindCatalogEntry(
    const std::shared_ptr<const EditorJsonAssetCatalogSnapshot>& catalog,
    const std::string& assetPath)
{
    if (!catalog) return nullptr;
    for (const auto& entry : catalog->assets)
        if (entry.assetPath == assetPath) return &entry;
    return nullptr;
}

bool SameOfflineControls(const UiViewModelSnapshot& a, const UiViewModelSnapshot& b){
    return std::tie(a.offlineMode, a.offlineState, a.offlineSamples,
        a.offlineCompletedSamples, a.offlineBounces, a.offlineLightSamples,
        a.offlineSeed, a.offlineAov, a.offlineExposure, a.offlineError, a.offlineUiNotice) ==
        std::tie(b.offlineMode, b.offlineState, b.offlineSamples,
        b.offlineCompletedSamples, b.offlineBounces, b.offlineLightSamples,
        b.offlineSeed, b.offlineAov, b.offlineExposure, b.offlineError, b.offlineUiNotice);
}

bool SameTextureOptions(const std::vector<EditorTextureAssetSnapshot>& a,
    const std::vector<EditorTextureAssetSnapshot>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); ++index)
        if (a[index].assetPath != b[index].assetPath || a[index].valid != b[index].valid)
            return false;
    return true;
}

// 文档命令的可用性统一作用在所有发出该命令的控件上，避免出现两套互相矛盾的启用状态。
// 菜单和中央材质页使用同一份文档命令可用性。
constexpr const char* DocumentCommandIds[] = {
    "menu-file-save", "menu-file-revert", "menu-file-reload",
    "menu-edit-reset", "menu-edit-validate", "material-save"};

std::string Number(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

std::string MotionNumber(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(4) << value;
    return stream.str();
}

std::string MotionQueryReadout(const std::vector<float>& values)
{
    std::string text;
    for (size_t i = 0; i < std::min(size_t(3), values.size()); ++i)
    {
        if (i) text += ", ";
        text += MotionNumber(values[i]);
    }
    return "[" + text + (values.size() > 3 ? ", ...]" : "]");
}

// 弹层位置按整像素写入：RmlUi 的 left/top 会参与布局缓存比较，抖动的小数会让静止帧反复失效。
std::string Pixels(float value)
{
    return std::to_string(std::lround(value)) + "px";
}

// 右键菜单的标题行与材质槽条目都只用资产文件名兜底，不把完整路径当标签。
std::string FileLabel(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

template<class T>
bool ParseNumber(const std::string& text, T& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc() &&
        result.ptr == text.data() + text.size() && std::isfinite(static_cast<double>(value));
}

// 文本输入与范围控件都是 ElementFormControl，但取值路径不同：
// 范围控件的 GetValue() 走 RmlUi WidgetSlider 的 "%f" 序列化（6 位小数），
// 小于 1e-6 的拖动结果会被截成 0；精确值只保存在 value 属性里。
// 这里对范围控件直接读 value 属性，让两类控件都返回可解析的有限数，
// 也保证拖动不会落进 HandleChange 的 "A complete finite number is required." 分支。
std::string ControlValue(Rml::Element& element)
{
    auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    if (control == nullptr) return std::string();
    auto* input = dynamic_cast<Rml::ElementFormControlInput*>(&element);
    if (input != nullptr && input->GetAttribute<Rml::String>("type", "") == "range")
        return Number(input->GetAttribute<float>("value", 0.0f));
    return control->GetValue();
}

// HEAD(ImGui) 的范围解析语义，迁移后必须保持一致：通道范围 > 参数范围 > 按当前值推导的固定兜底。
struct SliderRange
{
    float minValue = -1.0f;
    float maxValue = 1.0f;
};

SliderRange ResolveSliderRange(const EditorParameterSnapshot& parameter,
    const EditorParameterChannel& channel, float currentValue)
{
    if (channel.hasRange && std::isfinite(channel.minValue) &&
        std::isfinite(channel.maxValue) && channel.minValue <= channel.maxValue)
    {
        return {channel.minValue, channel.maxValue};
    }
    if (parameter.hasRange && std::isfinite(parameter.minValue) &&
        std::isfinite(parameter.maxValue) && parameter.minValue <= parameter.maxValue)
    {
        return {parameter.minValue, parameter.maxValue};
    }

    // 旧 M_ 文件没有范围时按默认值生成固定兜底，避免错误输入继续扩大可调区间。
    // 基准是 defaultValue 而不是正在编辑的值：拖动过程中区间不能跟着漂移。
    const float magnitude = std::abs(currentValue);
    float extent = std::max(1.0f, magnitude * 2.0f);
    if (!std::isfinite(extent)) extent = 1.0f;
    return {-extent, extent};
}

// HEAD 的折叠分组名与默认展开状态：General / Render States / Parameters / Texture Bindings。
// 键同时是 DOM id 后缀，切换只改既有分组元素的 class，不重建 DOM。
constexpr const char* DetailsSectionKeys[] = {"general", "render-states", "parameters", "textures", "camera-exposure"};

bool CameraExposureFieldEditable(EditorActorProperty property, ExposureMode mode)
{
    if (property == EditorActorProperty::CameraEv100) return mode == ExposureMode::Ev100;
    if (property == EditorActorProperty::CameraExposureCompensation) return true;
    return mode == ExposureMode::Auto;
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

std::string DetailsSectionId(const std::string& key)
{
    return std::string("details-section-") + key;
}

// 大纲选中 actor 的详情分组：每个类型一个分组，键由 SceneActorGroupSpecs() 派生，
// 不在这里维护第二份类型清单。
std::string ActorSectionKey(const std::string& groupKey)
{
    return "actor-" + groupKey;
}

// 运行时命令驱动的数值只在真正变化时提交，与运行时页 QueueChangedFloatAction 的语义一致。
bool ChangedFloat(float value, float currentValue)
{
    return std::abs(value - currentValue) > 0.0001f;
}

// actor-property 的文本输入嵌在属性行里；点击行的空白/标签区域时把焦点交给
// 第一个文本输入，避免用户必须精确点中输入框才能编辑。
Rml::Element* FirstActorTextInput(Rml::Element& root)
{
    for (int index = 0; index < root.GetNumChildren(); ++index)
    {
        Rml::Element* child = root.GetChild(index);
        if (child == nullptr) continue;
        if (child->GetTagName() == "input" &&
            child->GetAttribute<Rml::String>("type", "") == "text")
            return child;
        if (Rml::Element* nested = FirstActorTextInput(*child)) return nested;
    }
    return nullptr;
}

// Details 强度以 lux 表示，保留物理光照迁移后的作者范围；不再受旧调试页滑条约束。
constexpr float SunIntensityMin = 0.0f;
constexpr float SunIntensityMax = 150000.0f;

std::string ActorCountLabel(size_t count)
{
    return std::to_string(count) + (count == 1 ? " actor" : " actors");
}

// 树行只携带展示数据与动作地址；动作索引是 sceneActors 的下标，过滤只改变可见性。
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

EditorMaterialParameterValue CommandValue(const EditorParameterValue& value)
{
    switch (value.type)
    {
    case EditorParameterType::Float: return value.values[0];
    case EditorParameterType::Vec2: return EditorVec2{value.values[0], value.values[1]};
    case EditorParameterType::Vec3: return EditorVec3{value.values[0], value.values[1], value.values[2]};
    case EditorParameterType::Vec4: return EditorVec4{value.values[0], value.values[1], value.values[2], value.values[3]};
    case EditorParameterType::Color: return EditorColor{value.values};
    }
    return 0.0f;
}

EditorMaterialParameterType CommandType(EditorParameterType type)
{
    switch (type)
    {
    case EditorParameterType::Float: return EditorMaterialParameterType::Float;
    case EditorParameterType::Vec2: return EditorMaterialParameterType::Vec2;
    case EditorParameterType::Vec3: return EditorMaterialParameterType::Vec3;
    case EditorParameterType::Vec4: return EditorMaterialParameterType::Vec4;
    case EditorParameterType::Color: return EditorMaterialParameterType::Color;
    }
    return EditorMaterialParameterType::Float;
}

Editor::Selection::MaterialInstanceModelContext SelectionModel(
    const MaterialInstanceModelSnapshot& model)
{
    Editor::Selection::MaterialInstanceModelContext result;
    result.worldGeneration = model.worldGeneration;
    result.scenePath = model.scenePath;
    result.objectId = model.objectId;
    result.displayName = model.displayName;
    result.objectIdentity = model.objectIdentity;
    for (const auto& material : model.materials)
    {
        Editor::Selection::MaterialInstanceModelMaterial item;
        item.objectId = material.objectId == 0 ? model.objectId : material.objectId;
        item.materialSlotIndex = material.materialSlotIndex;
        item.materialSlotName = material.materialSlotName;
        item.materialInstancePath = material.materialInstancePath;
        item.displayName = material.materialSlotName;
        result.materials.push_back(std::move(item));
    }
    return result;
}

} // namespace

MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel(IEditorCommandSink* sink)
    : commandSink(sink)
{
}

MaterialInstanceAssetEditorPanel::~MaterialInstanceAssetEditorPanel()
{
    Detach();
}

bool MaterialInstanceAssetEditorPanel::ValidateDocument(Rml::ElementDocument& candidate, std::string& error)
{
    for (const char* id : {
        "scene-models", "material-slots", "material-slots-section", "asset-list", "material-editor", "actor-details-content",
        "viewport-actor-overlay", "viewport-actor-lines", "viewport-actor-icons", "viewport-actor-visuals",
        "material-preview-image", "material-preview-status", "material-frame",
        "document-title", "document-status", "editor-notice", "details-content",
        "offline-status", "offline-notice", "close-dialog", "close-document-name",
        "mode-realtime", "mode-offline", "menu-file-save", "menu-file-revert",
        "menu-file-reload", "menu-edit-reset", "menu-edit-validate",
        "apply-preview", "restore-preview",
        "offline-samples", "offline-bounces", "offline-lightSamples", "offline-seed",
        "offline-aov", "offline-exposure", "offline-pause", "offline-resume",
        "offline-stop", "offline-restart", "offline-export", "asset-search",
        "close-document-status", "close-save", "editor-navigation",
        "editor-inspector", "editor-content", "workspace-content-tab", "workspace-renderer-tab",
        "content-browser-page", "renderer-page", "performance-page", "workspace-performance-tab",
        "performance-fps", "performance-cpu-ms", "performance-gpu-ms", "performance-status",
        "performance-passes", "editor-viewport-surface", "editor-viewport-image",
        "workspace-log-tab", "log-page", "log-scroll", "log-rows", "log-count",
        "log-output", "log-scripts", "log-output-tab", "log-scripts-tab",
        "log-severity", "log-source", "log-search", "log-autoscroll", "log-clear",
        "log-scripts-search", "log-scripts-summary", "log-scripts-rows",
        "log-scripts-page", "log-scripts-prev", "log-scripts-next",
        "workspace-viewport-tab", "workspace-outliner-tab", "workspace-details-tab",
        "workspace-resource-tab", "workspace-resource-icon", "workspace-resource-label",
        "resource-editor", "resource-editor-toolbar", "resource-reload", "resource-preview-panel",
        "blueprint-asset-editor", "blueprint-asset-save", "blueprint-asset-revert",
        "retarget-asset-editor", "retarget-title", "retarget-identity", "retarget-status",
        "retarget-bind", "retarget-calibrated", "retarget-front", "retarget-side", "retarget-top",
        "retarget-anchor", "retarget-surface", "retarget-lines", "retarget-mappings",
        "retarget-focus", "retarget-fit", "retarget-zoom", "retarget-all-bones",
        "retarget-selection", "retarget-measurement", "retarget-prev", "retarget-next", "retarget-page",
        "retarget-reference", "retarget-animation", "retarget-reference-toolbar", "retarget-reference-workspace",
        "retarget-hierarchy", "retarget-hierarchy-source", "retarget-hierarchy-target", "retarget-details",
        "retarget-editor-workspace", "retarget-center", "retarget-sidebar", "retarget-animation-assets",
        "retarget-animation-workspace", "retarget-preview-surface", "retarget-preview-image", "retarget-preview-status",
        "retarget-animation-tools", "retarget-animation-play", "retarget-animation-pause", "retarget-animation-clip",
        "retarget-animation-time", "retarget-animation-status", "retarget-animation-frame",
        "resource-preview-title", "resource-preview-kind", "resource-preview-surface",
        "resource-preview-icon", "resource-preview-status", "resource-preview-path",
        "resource-preview-image",
        "resource-animation-tools", "resource-animation-play", "resource-animation-pause",
        "resource-animation-clip", "resource-animation-time", "resource-animation-status",
        "viewport-motion-debug", "viewport-motion-status",
        "viewport-motion-matching", "motion-matching-title", "motion-matching-database",
        "motion-matching-query", "motion-matching-decision", "motion-matching-rows",
        "motion-timeline-play", "motion-timeline-pause", "motion-timeline-seek", "motion-timeline-status",
        "motion-replay", "motion-replay-seek", "motion-replay-status",
        "motion-query-toggle", "motion-query-details", "motion-query-channels",
        "motion-query-database", "motion-query-payload", "motion-query-component",
        "resource-motion-features", "resource-motion-status", "resource-motion-values",
        "resource-texture-image",
        "resource-texture-fit",
        "resource-frame",
        "resource-model-tools", "resource-tool-select", "resource-tool-move", "resource-tool-rotate",
        "resource-tool-scale", "resource-space-world", "resource-space-local", "resource-model-info",
        "resource-model-name", "resource-model-path", "resource-model-status", "resource-model-fields",
        "resource-model-description",
        "resource-details-panel", "resource-json-inspector", "resource-json-title",
        "resource-json-kind", "resource-json-path", "resource-json-description",
        "resource-json-status", "resource-json-fields", "resource-json-search",
        "resource-json-page", "resource-json-prev", "resource-json-next",
        "dock-parking", "dock-dynamic-tabs", "dock-shell", "dock-preview", "dock-drag-shield", "workspace-reset",
        "asset-sources", "asset-breadcrumbs", "asset-count", "asset-empty", "browser-selection",
        "browser-back", "browser-forward", "browser-up", "browser-sources-toggle",
        "browser-grid", "browser-list", "browser-recursive", "browser-sort", "browser-size",
        "browser-sources-panel", "browser-type", "browser-refresh",
        "material-instance-details", "json-inspector", "json-title", "json-kind", "json-path",
        "json-description", "json-status", "json-fields", "json-search", "json-page", "json-prev", "json-next",
        "editor-menubar", "menu-file", "menu-edit", "menu-window", "menu-help",
        "menu-file-popup", "menu-window-popup", "menu-window-panels", "menu-file-levels",
        "menu-file-save", "menu-file-revert", "menu-file-reload",
        "menu-edit-reset", "menu-edit-validate", "menu-edit-undo", "menu-edit-redo",
        "menu-window-reset", "menu-window-restore",
        "about-dialog", "about-close",
        "editor-context-menu", "editor-context-menu-title", "editor-context-menu-items",
        "outliner-search", "outliner-type", "outliner-sort", "outliner-count", "outliner-empty",
        "viewport-toolbar", "viewport-tool-select", "viewport-tool-move", "viewport-tool-rotate",
        "viewport-tool-scale", "viewport-space-world", "viewport-space-local",
        "viewport-view-lit", "viewport-view-unlit", "viewport-view-wireframe",
        "viewport-axis-indicator", "viewport-axis-bar-x", "viewport-axis-bar-y",
        "viewport-axis-bar-z", "viewport-axis-label-x", "viewport-axis-label-y",
        "viewport-axis-label-z"})
    {
        if (candidate.GetElementById(id) == nullptr)
        {
            error = std::string("Editor workspace is missing element: ") + id;
            return false;
        }
    }
    return true;
}

bool MaterialInstanceAssetEditorPanel::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    if (!ValidateDocument(candidate, error)) return false;
    Detach();
    document = &candidate;
    document->AddEventListener("click", this);
    document->AddEventListener("change", this);
    document->AddEventListener("dblclick", this);
    document->AddEventListener("mousedown", this, true);
    document->AddEventListener("mousemove", this, true);
    document->AddEventListener("mouseup", this, true);
    document->AddEventListener("mousescroll", this, true);
    document->AddEventListener("dragstart", this);
    document->AddEventListener("dragdrop", this);
    document->AddEventListener("dragend", this);
    document->AddEventListener("keydown", this, true);
    if (!docking.Attach(candidate, error))
    {
        Detach();
        return false;
    }
    // 编辑器启动必须有一个可见的关卡编辑入口；停靠布局可能记住了 Content/Details 标签，
    // 但不能让默认启动场景变成“只有资产浏览器没有视口”。
    docking.SetPanelVisible("viewport", true);
    docking.Activate("viewport");
    widgetDocument.Attach(candidate);
    retargetEditor.Attach(candidate);
    if (!contentBrowser.Attach(candidate, error))
    {
        Detach();
        return false;
    }
    if (!jsonInspector.Attach(candidate, error))
    {
        Detach();
        return false;
    }
    if (!resourceJsonInspector.Attach(candidate, error, "resource-json"))
    {
        Detach();
        return false;
    }
    if (!logPanel.Attach(candidate, error))
    {
        Detach();
        return false;
    }
    if (!performancePanel.Attach(candidate, error))
    {
        Detach();
        return false;
    }
    detailsIdentity.clear();
    actorDetailsIdentity.clear();
    blueprintAssetIdentity.clear();
    RefreshWorkspace();
    RefreshLists();
    RefreshDetails();
    UpdateRuntimeView(runtimeView);
    return true;
}

void MaterialInstanceAssetEditorPanel::Detach()
{
    CancelBlueprintViewportDrag();
    actorDragPath.clear();
    textureViewSource.clear();
    ResetTextureView();
    previewDragButton = -1;
    runtimeControlsCurrent = false;
    motionMatchingRowsMounted = false;
    motionMatchingVisible = false;
    motionMatchingQueryExpanded = false;
    motionMatchingQueryChannels.clear();
    motionMatchingRefreshTime = -1;
    motionMatchingRefreshKey.clear();
    textureOptionsDirty = true;
    textureOptionSelections.clear();
    if (document != nullptr)
    {
        document->RemoveEventListener("click", this);
        document->RemoveEventListener("change", this);
        document->RemoveEventListener("dblclick", this);
        document->RemoveEventListener("mousedown", this, true);
        document->RemoveEventListener("mousemove", this, true);
        document->RemoveEventListener("mouseup", this, true);
        document->RemoveEventListener("mousescroll", this, true);
        document->RemoveEventListener("dragstart", this);
        document->RemoveEventListener("dragdrop", this);
        document->RemoveEventListener("dragend", this);
        document->RemoveEventListener("keydown", this, true);
        contentBrowser.Detach();
        logPanel.Detach();
        performancePanel.Detach();
        jsonInspector.Detach();
        resourceJsonInspector.Detach();
        docking.Detach();
        document = nullptr;
    }
    widgetDocument.Detach();
    retargetEditor.Detach();
    axisIndicatorSignature.clear();
    // 重新 Attach 时 DOM 是新的：让大纲第一帧无条件重挂一次。
    outlinerMounted = false;
    // 右键菜单的目标属于旧文档的 DOM，重新接上后必须先回到"没有菜单"。
    contextMenuKind = ContextMenuKind::None;
    contextMenuTarget.clear();
}

void MaterialInstanceAssetEditorPanel::SetSceneSelectionSink(IMaterialInstanceSceneSelectionSink* sink)
{
    sceneSelectionSink = sink;
}

void MaterialInstanceAssetEditorPanel::SetRuntimeActionSink(IEditorRuntimeActionSink* sink)
{
    runtimeActionSink = sink;
}

void MaterialInstanceAssetEditorPanel::Submit(EditorCommandEnvelope command)
{
    if (commandSink == nullptr)
    {
        localStatus = "Editor command service is unavailable.";
        return;
    }
    // 0 由 CommandBus 分配全局唯一 ID，避免与运行时导航/预览生产者的编号冲突。
    command.commandId = 0;
    command.source = EditorCommandSource::RmlUi;
    commandSink->Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::SubmitPathCommand(EditorCommandType type)
{
    if (!snapshot.activeDocument) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = snapshot.activeDocument->revision;
    command.payload = MaterialInstanceAssetPathPayload{snapshot.activeDocument->assetPath};
    if (type == EditorCommandType::ApplyMaterialInstancePreview)
    {
        command.payload = ApplyMaterialInstancePreviewPayload{
            snapshot.activeDocument->assetPath, snapshot.activeDocument->revision};
    }
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::QueueEdit(std::string key, EditorCommandBatchItem item)
{
    if (!snapshot.activeDocument) return;
    if (pendingEdits.empty())
    {
        pendingAssetPath = snapshot.activeDocument->assetPath;
        pendingRevision = snapshot.activeDocument->revision;
    }
    // 同一字段一帧内多次 change 合并成最终值；不同字段仍按用户操作顺序组成原子 batch。
    for (auto it = pendingEdits.begin(); it != pendingEdits.end(); ++it)
    {
        if (it->key == key)
        {
            pendingEdits.erase(it);
            break;
        }
    }
    pendingEdits.push_back({std::move(key), std::move(item)});
}

void MaterialInstanceAssetEditorPanel::FlushPendingEdits(bool save)
{
    if (pendingEdits.empty())
    {
        if (save) SubmitPathCommand(EditorCommandType::SaveMaterialInstanceDocument);
        return;
    }
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ExecuteEditorCommandBatch;
    command.expectedDocumentRevision = pendingRevision;
    ExecuteEditorCommandBatchPayload payload;
    for (auto& edit : pendingEdits) payload.commands.push_back(std::move(edit.item));
    if (save)
    {
        payload.commands.push_back({EditorCommandType::SaveMaterialInstanceDocument,
            MaterialInstanceAssetPathPayload{pendingAssetPath}});
    }
    command.payload = std::move(payload);
    Submit(std::move(command));
    pendingEdits.clear();
    pendingValues.clear();
    pendingAssetPath.clear();
}

void MaterialInstanceAssetEditorPanel::RequestSave()
{
    if (const auto* tab = resourceEditor.ActiveTab(); tab && tab->kind == EditorAssetKind::Blueprint &&
        IsResourceEditorVisible())
    {
        OpenSaveDialog(tab->path);
        return;
    }
    OpenSaveDialog(snapshot.activeDocument && IsMaterialEditorVisible()
        ? snapshot.activeDocument->assetPath : std::string{});
}

void MaterialInstanceAssetEditorPanel::RequestSaveAll()
{
    OpenSaveDialog({});
}

void MaterialInstanceAssetEditorPanel::OpenSaveDialog(const std::string& path)
{
    if (HasModal()) return;
    if (snapshot.modifiedDocuments.empty() && pendingEdits.empty() && pendingBlueprintPath.empty() &&
        (!snapshot.activeDocument || snapshot.activeDocument->status == EditorDocumentStatus::Clean))
    {
        localStatus = "No modified assets.";
        return;
    }
    // 先提交未完成 batch，再从下一轮权威快照取得新 revision；弹窗打开本身不保存。
    saveAwaitingSnapshot = !pendingEdits.empty() || !pendingBlueprintPath.empty();
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    saveFilterPath = path;
    saveDialogVisible = true;
    saveChecked.clear();
    saveKnownPaths.clear();
    saveWaiting.clear();
    saveHadFailure = false;
    saveMessage.clear();
    CloseMenus();
    CloseContextMenu();
    RefreshSaveDialog();
}

void MaterialInstanceAssetEditorPanel::RefreshSaveDialog()
{
    if (!document) return;
    WidgetRefreshScope refresh(refreshingControls);
    if (saveDialogVisible && !saveWaiting.empty())
    {
        for (const auto& feedback : snapshot.saveFeedback)
        {
            if (feedback.sequence <= saveStartedSequence ||
                saveWaiting.erase(feedback.assetPath) == 0) continue;
            if (!feedback.succeeded)
            {
                saveHadFailure = true;
                saveMessage += feedback.assetPath + ": " + feedback.message + "\n";
            }
        }
        if (saveWaiting.empty() && !saveHadFailure)
        {
            saveDialogVisible = false;
            if (jsonAssetRequestSink) jsonAssetRequestSink->RequestRefreshAssetCatalog();
        }
    }
    widgetDocument.SetProperty("save-dialog", "display", saveDialogVisible ? "block" : "none");
    if (!saveDialogVisible) return;
    saveRows.clear();
    for (const auto& asset : snapshot.modifiedDocuments)
        if (saveFilterPath.empty() || saveFilterPath == asset.assetPath)
            saveRows.push_back(asset);
    // 兼容旧文档快照生产者；实际 Runtime 始终发布跨类型 modifiedDocuments。
    if (saveRows.empty() && snapshot.modifiedDocuments.empty() && snapshot.activeDocument &&
        snapshot.activeDocument->status != EditorDocumentStatus::Clean &&
        (saveFilterPath.empty() || saveFilterPath == snapshot.activeDocument->assetPath))
        saveRows.push_back({snapshot.activeDocument->assetPath, EditorAssetKind::MaterialInstance,
            snapshot.activeDocument->revision, snapshot.activeDocument->status,
            snapshot.activeDocument->validationMessage});
    std::string markup;
    for (size_t index = 0; index < saveRows.size(); ++index)
    {
        const auto& asset = saveRows[index];
        if (saveKnownPaths.insert(asset.assetPath).second) saveChecked.insert(asset.assetPath);
        markup += "<div class=\"save-row\"><input type=\"checkbox\" id=\"save-file-" +
            std::to_string(index) + "\" data-action=\"save-file\" data-index=\"" + std::to_string(index) +
            "\"/><label class=\"save-path\" for=\"save-file-" +
            std::to_string(index) + "\">" + Text(asset.assetPath) + "</label><span class=\"save-state\">" +
            Text(DocumentStatus(asset.status)) + "</span></div>";
    }
    if (markup.empty()) markup = "<p>No modified assets.</p>";
    SetMarkup("save-document-list", std::move(markup));
    // 勾选/等待态原地同步，点击 checkbox 不销毁焦点节点。
    for (size_t index = 0; index < saveRows.size(); ++index)
    {
        const auto id = "save-file-" + std::to_string(index);
        if (auto* input = document->GetElementById(id))
        {
            const bool checked = saveChecked.count(saveRows[index].assetPath) != 0;
            if (checked && !input->HasAttribute("checked")) input->SetAttribute("checked", "checked");
            else if (!checked && input->HasAttribute("checked")) input->RemoveAttribute("checked");
        }
        SetDisabled(id, !saveWaiting.empty());
    }
    SetMarkup("save-document-status", Text(saveMessage));
    bool any = false;
    for (const auto& row : saveRows) any = any || saveChecked.count(row.assetPath) != 0;
    SetDisabled("save-confirm", !any || !saveWaiting.empty() || saveAwaitingSnapshot);
    SetDisabled("save-cancel", !saveWaiting.empty());
    SetDisabled("save-select-all", saveRows.empty() || !saveWaiting.empty());
}

void MaterialInstanceAssetEditorPanel::ConfirmSave()
{
    if (!saveWaiting.empty() || saveAwaitingSnapshot) return;
    saveHadFailure = false;
    saveMessage.clear();
    saveStartedSequence = 0;
    for (const auto& feedback : snapshot.saveFeedback)
        saveStartedSequence = std::max(saveStartedSequence, feedback.sequence);
    for (const auto& row : saveRows)
    {
        if (saveChecked.count(row.assetPath) == 0) continue;
        EditorCommandEnvelope command;
        command.type = row.kind == EditorAssetKind::Scene
            ? EditorCommandType::SaveSceneDocument : row.kind == EditorAssetKind::Mesh
            ? EditorCommandType::SaveMeshDocument : row.kind == EditorAssetKind::Tressfx
            ? EditorCommandType::SaveTressfxDocument : row.kind == EditorAssetKind::Texture
            ? EditorCommandType::SaveTextureDocument : row.kind == EditorAssetKind::Blueprint
            ? EditorCommandType::SaveBlueprintDocument : EditorCommandType::SaveMaterialInstanceDocument;
        command.expectedDocumentRevision = row.revision;
        command.payload = MaterialInstanceAssetPathPayload{row.assetPath};
        saveWaiting.insert(row.assetPath);
        Submit(std::move(command));
    }
    RefreshSaveDialog();
}

void MaterialInstanceAssetEditorPanel::Build(const MaterialInstanceEditorSnapshot& value)
{
    // 不把旧 UI 修改静默重定位到新文档或新 revision；CommandBus 负责拒绝过期 batch。
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    scriptEditRequests.clear();
    textureOptionsDirty = textureOptionsDirty || !SameTextureOptions(snapshot.textureAssets, value.textureAssets);
    const std::string previousDocument = snapshot.selectedDocumentPath;
    snapshot = value;
    if (blueprintCommandAwaitingSnapshot)
    {
        const auto* authored = BlueprintDocument(blueprintCommandPath);
        if (!authored || authored->documentRevision != blueprintCommandRevision)
            blueprintCommandAwaitingSnapshot = false;
        // GT 完整 drain 后发布终态；失败不推进 revision，同样必须解除本地在途门禁。
        for (const auto& feedback : snapshot.blueprintCommandFeedback)
            if (feedback.assetPath == blueprintCommandPath &&
                feedback.sequence > blueprintCommandFeedbackSequence)
                blueprintCommandAwaitingSnapshot = false;
    }
    saveAwaitingSnapshot = false;
    // 文档集合由服务拥有，停靠标签只是投影；关卡/贴图页面不会被每帧刷新抢走。
    const bool closingMaterialActive = !closingMaterialPath.empty() &&
        docking.IsPanelActive(MaterialPanelId(closingMaterialPath));
    RefreshDocumentTabs();
    bool materialClosed = !closingMaterialPath.empty();
    for (const auto& tab : snapshot.documentTabs)
        if (tab.assetPath == closingMaterialPath) materialClosed = false;
    if (materialClosed)
    {
        // MI 关闭由 GT 确认；确认后才切邻页，不能被服务内部的 MI 选择覆盖跨类型邻居。
        const std::string next = std::move(panelAfterMaterialClose);
        closingMaterialPath.clear();
        panelAfterMaterialClose.clear();
        activateMaterialDocument = false;
        if (closingMaterialActive) ActivateWorkspacePanel(next);
    }
    else if ((activateMaterialDocument || previousDocument != snapshot.selectedDocumentPath) &&
        !snapshot.selectedDocumentPath.empty())
    {
        docking.SetPanelVisible(MaterialPanelId(snapshot.selectedDocumentPath), true);
        docking.Activate(MaterialPanelId(snapshot.selectedDocumentPath));
        activateMaterialDocument = false;
        viewingJson = false;
    }
    RefreshWorkspace();
    resourceEditorVisible = IsResourceEditorVisible();
    logPanel.Build(snapshot.logs, docking.IsPanelActive("log"));
    // 面板数据变化时顺带喂一次性能页（5 Hz 节流在性能页内部）；平时由
    // UiSubsystem 在到期窗口单独调用 UpdatePerformance，不触发整份面板重建。
    UpdatePerformance(snapshot.performance);
    // UI 修改命令已入队但新草稿尚未发布时，关闭流程必须等到权威 revision。
    const bool closingTressfxStaging = pendingCloseKind == EditorAssetKind::Tressfx &&
        hairSimulationPath == pendingClosePath && hairSimulationPendingSave;
    if (discardBlueprintThenClose &&
        (pendingCloseKind == EditorAssetKind::Blueprint || pendingCloseKind == EditorAssetKind::Tressfx))
    {
        if (!discardBlueprintCloseSubmitted && !closingTressfxStaging)
        {
            // 等上一帧的修改成为权威版本再回退，不能把旧 revision 的丢弃请求排在修改后面。
            SubmitResourceDocumentCommand(pendingCloseKind == EditorAssetKind::Tressfx
                ? EditorCommandType::RevertTressfxDocument : EditorCommandType::RevertBlueprintDocument,
                pendingClosePath);
            discardBlueprintCloseSubmitted = true;
        }
        else if (discardBlueprintCloseSubmitted)
        {
            const auto* actor = ResourceDocument(pendingClosePath);
            if (actor && !actor->dirty)
            {
                CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::RequireClean);
                pendingClosePath.clear();
            }
            // 拒绝或尚未确认时保留标签和弹窗，允许用户按新版本重试。
            discardBlueprintThenClose = discardBlueprintCloseSubmitted = false;
        }
    }
    else if (saveThenClose &&
        (pendingCloseKind == EditorAssetKind::Blueprint || pendingCloseKind == EditorAssetKind::Tressfx))
    {
        if (!saveCloseSubmitted && !closingTressfxStaging)
        {
            SubmitResourceDocumentCommand(pendingCloseKind == EditorAssetKind::Tressfx
                ? EditorCommandType::SaveTressfxDocument : EditorCommandType::SaveBlueprintDocument,
                pendingClosePath);
            saveCloseSubmitted = true;
        }
        else if (saveCloseSubmitted)
        {
            for (const auto& feedback : snapshot.saveFeedback)
                if (feedback.assetPath == pendingClosePath && feedback.sequence > saveStartedSequence)
                {
                    const auto* actor = ResourceDocument(pendingClosePath);
                    if (feedback.succeeded && actor && !actor->dirty)
                    {
                        CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::RequireClean);
                        pendingClosePath.clear();
                    }
                    saveThenClose = false;
                    saveCloseSubmitted = false;
                    break;
                }
        }
    }
    else if (saveThenClose && !saveCloseSubmitted && snapshot.activeDocument &&
        snapshot.activeDocument->assetPath == pendingClosePath)
    {
        SubmitPathCommand(EditorCommandType::SaveMaterialInstanceDocument);
        saveCloseSubmitted = true;
    }
    else if (saveThenClose && saveCloseSubmitted)
    {
        if (snapshot.activeDocument && snapshot.activeDocument->assetPath == pendingClosePath &&
            (snapshot.activeDocument->status == EditorDocumentStatus::SaveFailed ||
             snapshot.activeDocument->status == EditorDocumentStatus::SourceChanged))
        {
            saveThenClose = false;
            saveCloseSubmitted = false;
        }
        for (const auto& tab : snapshot.documentTabs)
        {
            if (saveThenClose && tab.assetPath == pendingClosePath && !tab.dirty)
            {
                CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::RequireClean);
                pendingClosePath.clear();
                saveThenClose = false;
                saveCloseSubmitted = false;
                break;
            }
        }
    }
    RefreshLists();
    RefreshDetails();
    RefreshSaveDialog();
    // 全局保存不受当前文档/只读资源页控制；单文件保存保留文档自身的可用性。
    const bool modified = !snapshot.modifiedDocuments.empty() ||
        (snapshot.activeDocument && snapshot.activeDocument->status != EditorDocumentStatus::Clean);
    SetDisabled("menu-file-save", !modified);
    SetDisabled("workspace-save", !modified);
    if (document != nullptr)
    {
        widgetDocument.SetProperty("close-dialog", "display",
            pendingClosePath.empty() ? "none" : "block");
        widgetDocument.SetProperty("material-instance-details", "display", viewingJson ? "none" : "block");
        widgetDocument.SetProperty("json-inspector", "display", viewingJson ? "block" : "none");
        if (viewingJson)
            jsonInspector.Build(snapshot.jsonDocument, requestedJsonPath, snapshot.jsonDocumentLoading);
        if (const auto* active = resourceEditor.ActiveTab(); active != nullptr &&
            snapshot.jsonDocument != nullptr && snapshot.jsonDocument->assetPath == active->path)
            resourceEditor.SetDocument(snapshot.jsonDocument, snapshot.jsonDocumentLoading);
        RefreshResourceEditor();
        RefreshMaterialPreview();
        SetMarkup("close-document-name", Text(pendingClosePath));
        SetMarkup("close-document-status", Text(snapshot.statusMessage));
        SetDisabled("close-save", saveThenClose || discardBlueprintThenClose);
        SetDisabled("close-discard", discardBlueprintThenClose ||
            (saveThenClose && (pendingCloseKind == EditorAssetKind::Blueprint ||
                pendingCloseKind == EditorAssetKind::Tressfx)));
    }
}

void MaterialInstanceAssetEditorPanel::SetMarkup(const std::string& id, std::string markup)
{
    widgetDocument.SetMarkup(id, std::move(markup));
}

void MaterialInstanceAssetEditorPanel::SetControlValue(const std::string& id, const std::string& value)
{
    widgetDocument.SetControlValue(id, value);
}

void MaterialInstanceAssetEditorPanel::SetDisabled(const std::string& id, bool disabled)
{
    widgetDocument.SetDisabled(id, disabled);
}

void MaterialInstanceAssetEditorPanel::RefreshWorkspace()
{
    if (document == nullptr) return;
    WidgetRefreshScope refresh(refreshingControls);
    docking.Sync();
    RefreshMenus();
    RefreshViewportToolbar();
}

void MaterialInstanceAssetEditorPanel::RefreshMenus()
{
    RefreshLevelMenu();
    SetMarkup("menu-window-panels", WindowMenuMarkup());
    // 菜单栏之外的状态写入同样走缓存：静止帧不重复设置 class 与 display。
    widgetDocument.SetProperty("about-dialog", "display", aboutVisible ? "block" : "none");
    // 模态弹窗打开时上下文菜单必须收起：模态期间工作区命令都不可用，
    // 留一个点不动的弹层会让人以为命令失效。
    if (HasModal() && IsContextMenuOpen()) CloseContextMenu();
}

// Open Level 子菜单来自冻结的只读资产目录；目录代次不变时不重建条目和索引表。
void MaterialInstanceAssetEditorPanel::RefreshLevelMenu()
{
    const auto* catalog = snapshot.assetCatalog.get();
    const uint64_t revision = catalog == nullptr ? 0 : catalog->revision;
    if (menuCatalogReady && menuCatalog.get() == catalog && menuCatalogRevision == revision) return;
    menuCatalog = snapshot.assetCatalog;
    menuCatalogRevision = revision;
    menuCatalogReady = true;
    menuLevelPaths.clear();
    std::string markup;
    if (catalog != nullptr)
    {
        for (const auto& asset : catalog->assets)
        {
            if (asset.kind != EditorAssetKind::Scene) continue;
            const size_t index = menuLevelPaths.size();
            menuLevelPaths.push_back(asset.assetPath);
            const std::string label = asset.name.empty()
                ? std::filesystem::path(asset.assetPath).filename().string() : asset.name;
            markup += UiWidgets::MenuItem("menu-level-" + std::to_string(index),
                {"menu-level", index}, label, false, false);
        }
    }
    if (markup.empty())
        markup = UiWidgets::MenuItem("menu-level-empty", {"menu-level-empty", 0},
            "No levels in the asset catalog", false, true);
    SetMarkup("menu-file-levels", std::move(markup));
}

std::string MaterialInstanceAssetEditorPanel::WindowMenuMarkup() const
{
    std::string markup;
    const auto& panels = UiWidgets::DockWorkspace::Panels();
    for (size_t index = 0; index < panels.size(); ++index)
    {
        markup += UiWidgets::MenuItem("menu-window-" + panels[index], {"menu-window", index},
            PanelLabel(panels[index]), docking.IsPanelVisible(panels[index]), false);
    }
    return markup;
}

void MaterialInstanceAssetEditorPanel::RefreshViewportToolbar()
{
    if (document == nullptr) return;
    SetViewportToolbarClass("viewport-tool-select", viewportTransformTool == "select");
    SetViewportToolbarClass("viewport-tool-move", viewportTransformTool == "move");
    SetViewportToolbarClass("viewport-tool-rotate", viewportTransformTool == "rotate");
    SetViewportToolbarClass("viewport-tool-scale", viewportTransformTool == "scale");
    SetViewportToolbarClass("viewport-space-world", viewportTransformSpace == "world");
    SetViewportToolbarClass("viewport-space-local", viewportTransformSpace == "local");
    // view mode 直接映射运行时 debug view：0 是完整画面，1 是基础色。
    // 工具栏不声明自己没有设置过的调试视图，因此其他调试模式在这三个按钮上都没有高亮。
    SetViewportToolbarClass("viewport-view-lit", runtimeView.debugViewMode == 0);
    SetViewportToolbarClass("viewport-view-unlit", runtimeView.debugViewMode == 1);
    // 菜单只映射已有 debug view，不复制渲染状态；勾选与标题以运行时快照为准。
    const char* modes[] = {"Lit", "Base Color", "Emissive", "World Normal",
        "Roughness", "Metallic", "Ambient Occlusion", "Shadow", "Direct Lighting",
        "Indirect Diffuse", "Indirect Specular", "Shadow Cascade Index", "Shading Model"};
    std::string markup;
    for (size_t mode = 1; mode < sizeof(modes) / sizeof(modes[0]); ++mode)
        markup += UiWidgets::MenuItem("viewport-buffer-" + std::to_string(mode),
            {"context-view-mode", mode}, modes[mode], runtimeView.debugViewMode == static_cast<int>(mode), false);
    SetMarkup("viewport-buffer-modes", markup);
    const int mode = runtimeView.debugViewMode;
    SetMarkup("viewport-view-title", std::string(mode >= 0 && mode < 13 ? modes[mode] : "Debug View") + " v");
    RefreshViewportAxisIndicator();
    RefreshActorVisualization();
}

void MaterialInstanceAssetEditorPanel::RefreshViewportAxisIndicator()
{
    if (document == nullptr) return;

    // 相机朝向来自冻结的大纲快照（EditorSceneCameraActor 带 forward/right/up），
    // 因此这里不需要新增快照字段，也不访问 renderer。
    const EditorSceneActorSnapshot* camera = nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors())
    {
        if (actor.type == EditorSceneActorType::Camera)
        {
            camera = &actor;
            break;
        }
    }
    if (camera == nullptr) return;

    const auto& view = camera->hasViewportCamera ? camera->viewportCamera : camera->camera;
    const Eigen::Vector3f forward = view.forward.normalized();
    const Eigen::Vector3f right = view.right.normalized();
    const Eigen::Vector3f up = view.up.normalized();
    const auto triad = EditorUi::ProjectAxisIndicatorTriad(right, up, forward);

    std::array<size_t, 3> order = {0, 1, 2};
    std::sort(order.begin(), order.end(), [&triad](size_t lhs, size_t rhs)
    {
        return triad[lhs].depth > triad[rhs].depth;
    });

    // 先算量化后的形态签名：相机静止时它不变，于是不写 DOM。
    // 量化到 1 度 / 1 像素，避免浮点末位噪声让"静止帧"也一直重写属性。
    std::ostringstream signature;
    for (const EditorUi::AxisIndicatorProjection& projection : triad)
    {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        EditorUi::AxisIndicatorLabelOffset(projection, offsetX, offsetY);
        signature << (projection.degenerate ? 'D' : 'A')
                  << std::lround(EditorUi::AxisIndicatorRotationDegrees(projection)) << ','
                  << std::lround(offsetX) << ',' << std::lround(offsetY) << ';';
    }
    for (size_t index : order) signature << index;
    const std::string wanted = signature.str();
    if (wanted == axisIndicatorSignature) return;
    axisIndicatorSignature = wanted;

    static const char* kBarIds[3] = {
        "viewport-axis-bar-x", "viewport-axis-bar-y", "viewport-axis-bar-z"};
    static const char* kLabelIds[3] = {
        "viewport-axis-label-x", "viewport-axis-label-y", "viewport-axis-label-z"};

    // 远的先画、近的后画，让朝向观察者的轴盖住背向的轴。depth 越小越靠近观察者，
    // 因此按 depth 降序排列。
    for (size_t rank = 0; rank < order.size(); ++rank)
    {
        const size_t index = order[rank];
        const EditorUi::AxisIndicatorProjection& projection = triad[index];
        const std::string barId = kBarIds[index];
        const std::string labelId = kLabelIds[index];

        // 先摆好层序，再写形态：index 是稳定的轴身份，rank 是绘制顺序。
        widgetDocument.SetProperty(barId, "z-index", std::to_string(rank + 1));
        widgetDocument.SetProperty(labelId, "z-index",
            std::to_string(order.size() + rank + 1));

        // 固定盒尺寸，只改绘制变换和可见性；margin/display/width 会让转动相机触发整页布局。
        const char* visibility = projection.degenerate ? "hidden" : "visible";
        widgetDocument.SetProperty(barId, "visibility", visibility);
        widgetDocument.SetProperty(labelId, "visibility", visibility);
        if (projection.degenerate)
        {
            // 平行于视线时杆和字母一起隐藏，避免三个字母在锚点重叠。
            widgetDocument.SetProperty(labelId, "transform", "translate(0px, 0px)");
            continue;
        }

        float offsetX = 0.0f;
        float offsetY = 0.0f;
        EditorUi::AxisIndicatorLabelOffset(projection, offsetX, offsetY);
        widgetDocument.SetProperty(labelId, "transform",
            "translate(" + Pixels(offsetX) + ", " + Pixels(offsetY) + ")");
        widgetDocument.SetProperty(barId, "transform",
            std::string("rotate(") +
                std::to_string(EditorUi::AxisIndicatorRotationDegrees(projection)) + "deg)");
    }
}

// 工具条激活态只在真正变化时写一次 class；静止帧不重复触碰 RmlUi 样式，也不重建 markup。
void MaterialInstanceAssetEditorPanel::SetViewportToolbarClass(const std::string& id, bool active)
{
    widgetDocument.SetClass(id, "active", active);
}

void MaterialInstanceAssetEditorPanel::ToggleMenu(const std::string& menu)
{
    openMenu = openMenu == menu ? std::string() : menu;
    ApplyMenuState();
}

void MaterialInstanceAssetEditorPanel::CloseMenus()
{
    openMenu.clear();
    ApplyMenuState();
}

void MaterialInstanceAssetEditorPanel::ApplyMenuState()
{
    for (const char* menu : {"file", "edit", "window", "help", "view", "debug-draw"})
        widgetDocument.SetClass("menu-" + std::string(menu), "open", openMenu == menu);
}

bool MaterialInstanceAssetEditorPanel::IsInsideMenuBar(const Rml::Element* element) const
{
    if (document == nullptr) return false;
    const auto* menuBar = document->GetElementById("editor-menubar");
    for (const auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current == menuBar || current->GetId() == "menu-view" || current->GetId() == "menu-debug-draw") return true;
    return false;
}

bool MaterialInstanceAssetEditorPanel::IsInsideContextMenu(const Rml::Element* element) const
{
    if (document == nullptr) return false;
    const auto* menu = document->GetElementById("editor-context-menu");
    for (const auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current == menu) return true;
    return false;
}

// RmlUi 6.2 没有 contextmenu 事件：右键只表现为带 button=1 的 mousedown/mouseup。
// 这里在右键抬起时打开菜单，一是抬起不会打断将来可能出现的右键拖动，
// 二是抬起时的指针位置就是用户指向的位置。目标在这一刻解析成稳定地址。
void MaterialInstanceAssetEditorPanel::OpenContextMenu(Rml::Event& event)
{
    if (document == nullptr) return;
    ContextMenuKind kind = ContextMenuKind::None;
    std::string target;
    std::string title;
    Rml::Element* element = event.GetTargetElement();
    if (element != nullptr && IsSceneViewportElement(element))
    {
        // 视口没有拾取后端，因此菜单里只有视口级动作，不含任何逐对象条目。
        kind = ContextMenuKind::Viewport;
        title = "Viewport";
    }
    else
    {
        for (Rml::Element* current = element; current != nullptr && current != document;
            current = current->GetParentNode())
        {
            // 树行是 flex 容器：折叠符、标题与行尾眼睛是三个并列靶面，行内空白不属于任何一个。
            // 命中 .tree-row 时统一改读行标题按钮的地址，因此整行右键都指向同一行。
            Rml::Element* addressed = current->IsClassSet("tree-row")
                ? current->QuerySelector(".list-item") : current;
            const std::string action = addressed == nullptr ? std::string()
                : addressed->GetAttribute<std::string>("data-action", "");
            // 大纲 actor 行的三个点击靶面（行标题、折叠符、行尾眼睛）携带同一个 actor 下标，
            // 因此在行的任何位置右键都指向同一个 actor；分组行没有真实动作，不提供菜单。
            if (action == "outliner-actor" || action == "outliner-expand" ||
                action == "outliner-visible" || action == "outliner-slot")
            {
                const size_t index = addressed->GetAttribute<size_t>("data-index", 0);
                if (index >= OutlinerActors().size()) break;
                const EditorSceneActorSnapshot& actor = OutlinerActors()[index];
                kind = ContextMenuKind::Actor;
                target = actor.identity;
                title = actor.displayName;
                break;
            }
            if (action == "browser-item")
            {
                // DOM 上的 data-path 是当前挂载节点的完整资产地址（不是虚拟窗口下标）。
                const std::string path = addressed->GetAttribute<std::string>("data-path", "");
                const auto className = contentBrowser.BlueprintClass(path);
                if (!className.empty())
                {
                    kind = ContextMenuKind::BlueprintClass;
                    target = className;
                    title = className;
                    break;
                }
                EditorAssetKind assetKind = EditorAssetKind::MaterialInstance;
                std::string name;
                // 目录节点在只读目录里没有条目：目录没有真实可执行动作，因此不给菜单。
                if (path.empty() || !FindCatalogAssetKind(path, assetKind, name)) break;
                kind = ContextMenuKind::BrowserAsset;
                target = path;
                title = name;
                break;
            }
        }
    }
    contextMenuKind = kind;
    contextMenuTarget = target;
    if (kind == ContextMenuKind::None)
    {
        CloseContextMenu();
        return;
    }
    // 目标有稳定地址不等于它有可执行动作（例如相机只有逐对象动作之外的空集合）：
    // 空菜单一律不打开，避免出现一个"什么都没有"的弹层。
    std::string items = ContextMenuItemsMarkup();
    if (items.empty())
    {
        CloseContextMenu();
        return;
    }
    const Rml::Vector2f point = event.GetUnprojectedMouseScreenPos();
    contextMenuX = point.x;
    contextMenuY = point.y;
    SetMarkup("editor-context-menu-title", Text(title));
    SetMarkup("editor-context-menu-items", std::move(items));
    ApplyContextMenuState();
}

void MaterialInstanceAssetEditorPanel::CloseContextMenu()
{
    contextMenuKind = ContextMenuKind::None;
    contextMenuTarget.clear();
    ApplyContextMenuState();
}

void MaterialInstanceAssetEditorPanel::ApplyContextMenuState()
{
    widgetDocument.SetClass("editor-context-menu", "open", IsContextMenuOpen());
    widgetDocument.SetProperty("editor-context-menu", "left", Pixels(contextMenuX));
    widgetDocument.SetProperty("editor-context-menu", "top", Pixels(contextMenuY));
}

// 条目在打开时生成一次：菜单存活期间快照可能换代，因此动作执行时一律按稳定地址
// 重新查目标，查不到就给出诊断，而不是退回面板的当前选择。
std::string MaterialInstanceAssetEditorPanel::ContextMenuItemsMarkup() const
{
    std::string markup;
    if (contextMenuKind == ContextMenuKind::Actor)
    {
        const EditorSceneActorSnapshot* actor = ContextMenuActor();
        if (actor == nullptr) return markup;
        // 可见性开关只对真正持有渲染侧可见性状态的对象给出命令；相机/环境/风剖面没有这一
        // 状态，因此与大纲行尾的眼睛同一规则：不显示条目，也不放一个点了没有反应的假条目。
        if (SceneActorHasVisibilityToggle(actor->type))
        {
            markup += UiWidgets::MenuItem("context-outliner-visible",
                {"context-outliner-visible", 0},
                actor->visible ? "Hide in Viewport" : "Show in Viewport", false, false);
        }
        // mesh 的材质槽走与点击槽行完全相同的选择路由（打开 MI 文档并同步视口描边）。
        const MaterialInstanceModelSnapshot model = MeshSceneActorModel(*actor);
        for (size_t index = 0; index < model.materials.size(); ++index)
        {
            const auto& material = model.materials[index];
            markup += UiWidgets::MenuItem("context-actor-material-" + std::to_string(index),
                {"context-actor-material", index},
                "Open " + (material.materialSlotName.empty()
                    ? FileLabel(material.materialInstancePath) : material.materialSlotName),
                false, false);
        }
    }
    else if (contextMenuKind == ContextMenuKind::BlueprintClass)
    {
        markup += UiWidgets::MenuItem("context-class-create", {"context-class-create", 0},
            "Create Blueprint Asset...", false, !runtimeView.scripts.available ||
                runtimeView.scripts.state != ScriptPlayState::Editing);
        markup += UiWidgets::MenuItem("context-class-source", {"context-class-source", 0},
            "Open in VS Code", false, false);
    }
    else if (contextMenuKind == ContextMenuKind::BrowserAsset)
    {
        markup += UiWidgets::MenuItem("context-browser-open", {"context-browser-open", 0},
            "Open", false, false);
        for (const auto& asset : snapshot.modifiedDocuments)
            if (asset.assetPath == contextMenuTarget)
                markup += UiWidgets::MenuItem("context-browser-save", {"context-browser-save", 0},
                    "Save...", false, false);
    }
    else if (contextMenuKind == ContextMenuKind::Viewport)
    {
        // 与视口工具条共用 SetDebugViewMode：菜单只多给出勾选状态，没有第二条 UI→renderer 路径。
        // 条目下标就是 debug view 取值（工具条的 view mode 也这样寻址）。
        markup += UiWidgets::MenuItem("context-view-lit", {"context-view-mode", 0},
            "Lit", runtimeView.debugViewMode == 0, false);
        markup += UiWidgets::MenuItem("context-view-unlit", {"context-view-mode", 1},
            "Unlit", runtimeView.debugViewMode == 1, false);
        markup += "<div class=\"menu-separator\"></div>";
        markup += UiWidgets::MenuItem("context-save-all", {"context-save-all", 0},
            "Save Modified Assets...", false, snapshot.modifiedDocuments.empty());
    }
    return markup;
}

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::ContextMenuActor() const
{
    if (contextMenuTarget.empty()) return nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors())
        if (actor.identity == contextMenuTarget) return &actor;
    return nullptr;
}

// 只读目录（缺目录时退回旧 MI 资产列表）里查路径：既是"资产还是目录"的判据，
// 也是菜单标题的名字来源。打开动作仍复用 OpenBrowserAsset 的既有命令路由。
bool MaterialInstanceAssetEditorPanel::FindCatalogAssetKind(const std::string& path,
    EditorAssetKind& kind, std::string& name) const
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

void MaterialInstanceAssetEditorPanel::ToggleContextActorVisibility()
{
    if (runtimeActionSink == nullptr) return;
    const EditorSceneActorSnapshot* actor = ContextMenuActor();
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
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

void MaterialInstanceAssetEditorPanel::OpenContextActorMaterial(size_t slotIndex)
{
    const EditorSceneActorSnapshot* actor = ContextMenuActor();
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
    SelectModelMaterial(model, slotIndex);
}

void MaterialInstanceAssetEditorPanel::OpenContextBrowserAsset()
{
    const std::string path = contextMenuTarget;
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    std::string name;
    if (path.empty() || !FindCatalogAssetKind(path, kind, name))
    {
        localStatus = "The context menu target is no longer in the asset catalog.";
        return;
    }
    OpenBrowserAsset(path);
}

void MaterialInstanceAssetEditorPanel::RefreshLists()
{
    WidgetRefreshScope refresh(refreshingControls);
    RefreshOutliner();
    std::string slots;
    if (snapshot.selectedModel)
    {
        for (size_t index = 0; index < snapshot.selectedModel->materials.size(); ++index)
        {
            const auto& slot = snapshot.selectedModel->materials[index];
            slots += UiWidgets::ListRow({"slot", index}, slot.materialSlotName.empty()
                ? std::filesystem::path(slot.materialInstancePath).filename().string()
                : slot.materialSlotName,
                snapshot.selectedMaterialSlotIndex == slot.materialSlotIndex);
        }
    }
    SetMarkup("material-slots", std::move(slots));
    widgetDocument.SetProperty("material-slots-section", "display",
        snapshot.selectedModel && !snapshot.selectedModel->materials.empty() ? "block" : "none");
    contentBrowser.Build(snapshot, runtimeView.scripts.classes);
}

// 选择态与"已挂载 actor 的可见性"合成一个重挂签名：两者都只改已挂载行的
// class/文本/眼睛图标，不改变行集合，但对 markup 是可见变化。
// 可见性按稳定身份生成，因此名字相同但换了对象（World 换代）时签名仍然变化。
std::string MaterialInstanceAssetEditorPanel::OutlinerMountedVisuals() const
{
    const std::vector<EditorSceneActorSnapshot>& actors = OutlinerActors();
    std::string visuals = snapshot.selectedActorIdentity + "\n" +
        (snapshot.selectedMaterialSlotIndex
            ? std::to_string(*snapshot.selectedMaterialSlotIndex) : std::string());
    for (size_t index = outlinerMountedWindow.begin; index < outlinerMountedWindow.end; ++index)
    {
        if (index >= outlinerRows.size()) break;
        const EditorSceneActorRow& row = outlinerRows[index];
        if (row.isSlot || row.actorIndex >= actors.size()) continue;
        const EditorSceneActorSnapshot& actor = actors[row.actorIndex];
        if (!SceneActorHasVisibilityToggle(actor.type)) continue;
        visuals += "\n" + actor.identity + (actor.visible ? "=1" : "=0");
    }
    return visuals;
}

void MaterialInstanceAssetEditorPanel::RefreshActorVisualization()
{
    if (document == nullptr) return;
    widgetDocument.SetClass("viewport-actor-visuals", "selected", showActorVisualization);
    auto* surface = document->GetElementById("editor-viewport-surface");
    if (surface == nullptr || !surface->IsVisible(true)) return;
    const auto size = surface->GetBox().GetSize(Rml::BoxArea::Content);
    const auto& actors = OutlinerActors();
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
    if (showActorVisualization && view && size.x > 0 && size.y > 0)
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
            const bool selected = actor.identity == snapshot.selectedActorIdentity;
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
    SetMarkup("viewport-actor-lines", std::move(linesMarkup));
    SetMarkup("viewport-actor-icons", std::move(iconsMarkup));
    auto* lineLayer = document->GetElementById("viewport-actor-lines");
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
    document->GetElementById("viewport-actor-icons")->QuerySelectorAll(icons, "[data-action=viewport-actor]");
    for (auto* icon : icons)
    {
        const size_t index = icon->GetAttribute<size_t>("data-index", actors.size());
        const bool selected = index < actors.size() && actors[index].identity == snapshot.selectedActorIdentity;
        const std::string id = icon->GetId();
        widgetDocument.SetClass(id, "selected", selected);
        const auto& actor = actors[index];
        Eigen::Vector2f pixel;
        const Eigen::Vector3f position = actor.type == EditorSceneActorType::Camera
            ? actor.camera.position : actor.type == EditorSceneActorType::Script ? actor.position : actor.light.position;
        const bool visible = actor.visible && view &&
            ProjectActorVisualPoint(*view, position, size.x, size.y, pixel);
        widgetDocument.SetProperty(id, "visibility", visible ? "visible" : "hidden");
        if (visible)
            widgetDocument.SetProperty(id, "transform",
                "translate(" + Number(pixel.x() - 12) + "px, " + Number(pixel.y() - 12) + "px)");
    }
}

void MaterialInstanceAssetEditorPanel::RefreshOutliner()
{
    // 面板可能在 RML 热重载里处于 Detach 状态，而 UiSubsystem 仍逐帧 Build：
    // 这里必须先确认文档存在，窗口计算会直接读元素几何。
    if (document == nullptr) return;
    const std::vector<EditorSceneActorSnapshot>& actors = OutlinerActors();
    // World 换代时展开集合、搜索与选中态都不再指向同一个对象，统一回到初始状态。
    // 排序是面板偏好，与 Content Browser 的排序键一样跨目录/换代保留。
    const uint64_t generation = actors.empty() ? 0 : actors.front().worldGeneration;
    if (treeWorldGeneration != generation)
    {
        outlinerExpansion.expandedActors.clear();
        outlinerQuery.clear();
        outlinerTypeFilter = "all";
        treeWorldGeneration = generation;
        // 换代后旧的稳定身份不再指向任何对象：已打开的右键菜单连同它的目标一起收起。
        CloseContextMenu();
        SetControlValue("outliner-search", std::string());
        SetControlValue("outliner-type", outlinerTypeFilter);
    }
    SetControlValue("outliner-sort", SceneActorSortOrderKey(outlinerSortOrder));

    // 行集合只在真正变化时重建：actor 内容签名 + 搜索/过滤/排序/展开折叠状态。
    // 静止帧因此不重新排序，也不重新分配行标签。
    if (OutlinerRowsNeedRebuild(actors)) RebuildOutlinerRows(actors);

    // 行高先按上一帧已挂载的行实测（首帧读不到布局时保持 RCSS 兜底），
    // 再由滚动偏移与可视高度算出本帧窗口，与 Content Browser 的 VisibleWindow 同序。
    MeasureOutlinerRowHeight();
    const EditorSceneActorWindow window = OutlinerWindow();
    // 选择态不重挂 DOM：RmlUi 的双击要求两次 click 的目标节点身份保持不变。
    const std::string visuals = OutlinerMountedVisuals();
    if (!outlinerMounted ||
        outlinerMountedRevision != outlinerRowsRevision ||
        !SameSceneActorWindow(outlinerMountedWindow, window))
    {
        SetMarkup("scene-models", OutlinerRowsMarkup(window));
        outlinerMounted = true;
        outlinerMountedRevision = outlinerRowsRevision;
        outlinerMountedWindow = window;
        outlinerMountedVisuals = visuals;
    }
    else if (outlinerMountedVisuals != visuals)
    {
        RefreshOutlinerVisuals();
        outlinerMountedVisuals = visuals;
    }

    // 计数只统计 actor；展开的材质槽不增加 actor 数。
    size_t visibleActors = 0;
    for (const EditorSceneActorRow& row : outlinerRows)
        if (!row.isSlot) ++visibleActors;
    const size_t total = actors.size();
    SetMarkup("outliner-count", Text(visibleActors == total
        ? ActorCountLabel(total)
        : std::to_string(visibleActors) + " / " + ActorCountLabel(total)));
    const std::string emptyMessage = outlinerRows.empty()
        ? (total == 0 ? "No actors in the active world."
                      : "No actors match the current filter.")
        : std::string();
    SetMarkup("outliner-empty", Text(emptyMessage));
    // 空状态时收起树容器：flex:1 的空容器会把提示文字顶到面板底部。
    widgetDocument.SetProperty("outliner-empty", "display",
        emptyMessage.empty() ? "none" : "block");
    widgetDocument.SetProperty("scene-models", "display",
        emptyMessage.empty() ? "block" : "none");
}

void MaterialInstanceAssetEditorPanel::RefreshOutlinerVisuals()
{
    const auto& actors = OutlinerActors();
    Rml::Element* list = document->GetElementById("scene-models");
    size_t rowIndex = outlinerMountedWindow.begin;
    for (int index = 0; index < list->GetNumChildren(); ++index)
    {
        Rml::Element* element = list->GetChild(index);
        if (!element->IsClassSet("tree-row") || rowIndex >= outlinerRows.size()) continue;
        const auto& row = outlinerRows[rowIndex++];
        const auto& actor = actors[row.actorIndex];
        const bool selected = actor.identity == snapshot.selectedActorIdentity &&
            (!row.isSlot || snapshot.selectedMaterialSlotIndex == row.materialSlotIndex);
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

bool MaterialInstanceAssetEditorPanel::OutlinerRowsNeedRebuild(
    const std::vector<EditorSceneActorSnapshot>& actors) const
{
    return SceneActorRowsSignature(actors) != outlinerRowSignature ||
        outlinerQuery != outlinerBuiltQuery ||
        outlinerTypeFilter != outlinerBuiltTypeFilter ||
        outlinerSortOrder != outlinerBuiltSortOrder ||
        !SameSceneActorExpansion(outlinerExpansion, outlinerBuiltExpansion);
}

void MaterialInstanceAssetEditorPanel::RebuildOutlinerRows(
    const std::vector<EditorSceneActorSnapshot>& actors)
{
    outlinerRows = BuildSceneActorRows(
        actors, outlinerQuery, outlinerTypeFilter, outlinerSortOrder, outlinerExpansion);
    outlinerRowSignature = SceneActorRowsSignature(actors);
    outlinerBuiltQuery = outlinerQuery;
    outlinerBuiltTypeFilter = outlinerTypeFilter;
    outlinerBuiltSortOrder = outlinerSortOrder;
    outlinerBuiltExpansion = outlinerExpansion;
    ++outlinerRowsRevision;
}

EditorSceneActorWindow MaterialInstanceAssetEditorPanel::OutlinerWindow() const
{    auto* list = document->GetElementById("scene-models");
    const auto& box = list->GetBox();
    const float paddingY = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) +
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom);
    const float clientHeight = list->GetClientHeight() - paddingY;
    const float scroll = list->GetScrollTop() -
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top);
    return ComputeSceneActorWindow(
        outlinerRows.size(), clientHeight, scroll, outlinerRowHeight);
}

// 树行高度由 RCSS 统一决定（.tree-row 的内容是 .tree-toggle 与 .list-item），
// 这里读已挂载行的 margin box 校正兜底值——正如 Content Browser 读首个可见条目。
// 首帧或刚替换的 DOM 还没有布局，读到 0 时保持上一次的值。
void MaterialInstanceAssetEditorPanel::MeasureOutlinerRowHeight(){
    if (document == nullptr) return;
    auto* list = document->GetElementById("scene-models");
    for (int index = 0; index < list->GetNumChildren(); ++index)
    {
        Rml::Element* child = list->GetChild(index);
        if (!child->IsClassSet("tree-row")) continue;
        const auto measured = child->GetBox().GetSize(Rml::BoxArea::Margin);
        if (measured.y > 0.0f) outlinerRowHeight = measured.y;
        return;
    }
}

// 只把窗口内的行挂进 DOM，上下各一个 spacer 保留未挂载行的高度：
// 滚动范围因此与整棵树一致，"窗口化"不等于"截断到前 N 行"。
std::string MaterialInstanceAssetEditorPanel::OutlinerRowsMarkup(
    const EditorSceneActorWindow& window) const
{
    const std::vector<EditorSceneActorSnapshot>& actors = OutlinerActors();
    std::string markup;
    if (window.top > 0.0f)
        markup += "<div class=\"outliner-spacer\" style=\"height: " +
            std::to_string(window.top) + "px;\"></div>";
    for (size_t index = window.begin; index < window.end && index < outlinerRows.size(); ++index)
    {
        const EditorSceneActorRow& row = outlinerRows[index];
        if (row.actorIndex >= actors.size()) continue;
        const EditorSceneActorSnapshot& actor = actors[row.actorIndex];
        const bool selected = actor.identity == snapshot.selectedActorIdentity;
        if (row.isSlot)
        {
            markup += UiWidgets::TreeRow(OutlinerSlotRow(row.actorIndex, row.slotIndex, row.label,
                selected && snapshot.selectedMaterialSlotIndex == row.materialSlotIndex));
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

const std::vector<EditorSceneActorSnapshot>& MaterialInstanceAssetEditorPanel::OutlinerActors() const
{
    return SceneActorList(snapshot.sceneActors);
}

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::SelectedActor() const
{
    if (snapshot.selectedActorIdentity.empty()) return nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors())
        if (actor.identity == snapshot.selectedActorIdentity) return &actor;
    return nullptr;
}

void MaterialInstanceAssetEditorPanel::SelectOutlinerActor(const EditorSceneActorSnapshot& actor)
{
    if (sceneSelectionSink == nullptr) return;
    FlushPendingEdits();
    ShowSceneDetails();
    if (actor.type == EditorSceneActorType::Mesh)
    {
        // mesh actor 仍走迁移前的 MI 导航路由：整模型选择 + 视口描边完全一致。
        sceneSelectionSink->RequestModelSelection(SelectionModel(MeshSceneActorModel(actor)));
        return;
    }
    sceneSelectionSink->RequestActorSelection(actor.identity);
}

void MaterialInstanceAssetEditorPanel::RefreshDetails()
{
    WidgetRefreshScope refresh(refreshingControls);
    const EditorSceneActorSnapshot* actor = SelectedActor();
    const EditorSceneActorGroupSpec* actorSpec =
        actor == nullptr ? nullptr : FindSceneActorGroupSpec(actor->type);
    std::string scriptStructure;
    if (actor && actor->type == EditorSceneActorType::Script)
    {
        if (actor->scriptPhysics.is_object())
            for (const auto& joint : actor->scriptPhysics.value("constraints", nlohmann::json::array()))
                scriptStructure += joint.at("id").get<std::string>() + ":" + joint.at("type").get<std::string>() + ";";
        for (const auto& node : actor->scriptComponents)
            scriptStructure += node.value("id", "root") + "|" + node.at("type").get<std::string>() +
                "|" + node.value("parent", "") + ";";
        for (const auto& property : actor->scriptProperties.items())
            scriptStructure += property.key() + ":" + property.value().type_name() + ";";
        scriptStructure += actor->scriptComponentInstanceEditable.dump();
        scriptStructure += SceneLogicStructure(*actor);
        for (const auto& type : runtimeView.scripts.classes)
            if (ScriptTypeNameMatches(type, actor->scriptClass)) scriptStructure += type.at("properties").dump();
    }
    const std::string actorIdentity = actor == nullptr || actorSpec == nullptr
        ? "no-actor" : std::to_string(actor->worldGeneration) + "|" +
            actor->identity + "|" + actorSpec->key + "|" + scriptStructure;
    if (actorDetailsIdentity != actorIdentity)
    {
        actorDetailsIdentity = actorIdentity;
        SetMarkup("actor-details-content", actor != nullptr ? ActorSectionMarkup(*actor) : "");
    }
    RefreshActorValues(actor);
    if (viewingJson || resourceEditorVisible)
    {
        for (const char* id : DocumentCommandIds)
            SetDisabled(id, true);
        SetMarkup("editor-notice", Text(localStatus));
        return;
    }
    // 材质正文与 actor Details 分开缓存；选择对象不会重建材质输入控件。
    const std::string identity = (snapshot.activeDocument
            ? snapshot.activeDocument->assetPath + "|" + snapshot.activeDocument->schemaDigest
            : "closed");
    if (identity != detailsIdentity)
    {
        textureOptionsDirty = true;
        detailsIdentity = identity;
        std::string markup;
        if (snapshot.activeDocument)
        {
            const auto& asset = *snapshot.activeDocument;
            // General 承载当前面板没有单独展示的文档身份：MI 路径、M_ schema、摘要与 revision。
            std::string general;
            general += UiWidgets::PropertyInfo("MI Asset", asset.assetPath);
            general += UiWidgets::PropertyInfo("M_ schema", asset.baseMaterialPath);
            general += UiWidgets::PropertyInfo("Schema digest", asset.schemaDigest);
            general += UiWidgets::PropertyInfo("Revision", std::to_string(asset.revision));
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("general"), "general",
                "General", general, {"section"}, IsSectionExpanded("general"));

            std::string renderStates;
            for (size_t index = 0; index < asset.renderStates.size(); ++index)
            {
                const auto& state = asset.renderStates[index];
                renderStates += UiWidgets::PropertyRow(state.name,
                    UiWidgets::Select("state-" + std::to_string(index), {"state", index},
                        RenderStateOptions(state.name)),
                    "reset-state-" + std::to_string(index), {"reset-state", index});
            }
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("render-states"), "render-states",
                "Render States", renderStates, {"section"}, IsSectionExpanded("render-states"));

            std::string parameters;
            for (size_t index = 0; index < asset.parameters.size(); ++index)
            {
                const auto& parameter = asset.parameters[index];
                std::vector<UiWidgets::NumericComponent> components;
                const bool color = parameter.type == EditorParameterType::Color;
                for (uint32_t component = 0; component < parameter.effectiveValue.ComponentCount(); ++component)
                {
                    static const char* names[] = {"X", "Y", "Z", "W"};
                    const auto& channel = parameter.channels[component];
                    UiWidgets::NumericComponent entry;
                    entry.id = "parameter-" + std::to_string(index) + "-" + std::to_string(component);
                    entry.label = channel.name.empty() ? names[component] : channel.name;
                    entry.value = Number(parameter.effectiveValue.values[component]);
                    entry.change = {"parameter", index, component};
                    // HEAD 对 Color 分量不使用滑条（它走 ColorPicker4），这里保持同一分工：
                    // 线性 RGBA 分量仍是文本输入，颜色只多一个 sRGB 读数与色块。
                    if (!color)
                    {
                        const SliderRange range = ResolveSliderRange(parameter, channel,
                            parameter.defaultValue.values[component]);
                        entry.hasRange = true;
                        entry.minValue = range.minValue;
                        entry.maxValue = range.maxValue;
                    }
                    components.push_back(std::move(entry));
                }
                std::string controls;
                if (color)
                {
                    controls += UiWidgets::ColorPreview("color-swatch-" + std::to_string(index),
                        "color-hex-" + std::to_string(index),
                        UiWidgets::MakeColorReadout(parameter.effectiveValue.values));
                }
                controls += UiWidgets::NumericComponents(components);
                parameters += UiWidgets::PropertyGroup(parameter.name, controls,
                    "reset-parameter-" + std::to_string(index), {"reset-parameter", index},
                    parameter.description);
            }
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("parameters"), "parameters",
                "Parameters", parameters, {"section"}, IsSectionExpanded("parameters"));

            std::string textures;
            for (size_t index = 0; index < asset.textures.size(); ++index)
            {
                const auto& texture = asset.textures[index];
                std::string options = Option(texture.effectiveAssetPath, texture.effectiveAssetPath);
                for (const auto& candidate : snapshot.textureAssets)
                {
                    if (candidate.valid && candidate.assetPath != texture.effectiveAssetPath)
                        options += Option(candidate.assetPath, candidate.assetPath);
                }
                textures += UiWidgets::PropertyGroup(texture.slotName,
                    UiWidgets::Select("texture-" + std::to_string(index), {"texture", index}, options),
                    "reset-texture-" + std::to_string(index), {"reset-texture", index}, texture.description);
            }
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("textures"), "textures",
                "Texture Bindings", textures, {"section"}, IsSectionExpanded("textures"));
        }
        SetMarkup("details-content", UiWidgets::VerticalBox(markup));
        // 新 DOM 不复用旧选择器的 markup 缓存。
        if (snapshot.activeDocument)
        {
            for (size_t index = 0; index < snapshot.activeDocument->textures.size(); ++index)
                widgetDocument.ForgetMarkup("texture-" + std::to_string(index));
        }
    }
    ApplySectionState();
    RefreshControls();
}

std::string MaterialInstanceAssetEditorPanel::ActorSectionMarkup(
    const EditorSceneActorSnapshot& actor)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(actor.type);
    if (spec == nullptr) return std::string();
    // 只读字段每个值都带 id：值随 World、相机或运行时命令变化时只刷新这一项，
    // 不重建分组 DOM（重建会丢焦点与滚动位置）。
    std::string body;
    for (const auto& field : SceneActorEditableFields(actor))
    {
        if (CameraExposureField(field.property) == nullptr)
            body += ActorPropertyRowMarkup(field);
    }
    for (const EditorSceneActorField& field : SceneActorFields(actor))
        body += UiWidgets::PropertyInfo(field.label, field.value, field.id);
    if (actor.type == EditorSceneActorType::Script)
    {
        body += UiWidgets::PropertyControlRow("Name",
            UiWidgets::Input("script-blueprint-name", {"script-blueprint-name"}, actor.displayName) +
            "<button id=\"script-blueprint-duplicate\" class=\"browser-icon\" data-action=\"script-blueprint-duplicate\" "
            "title=\"Duplicate Blueprint\"><img src=\"icons/copy.png\"/></button>"
            "<button id=\"script-blueprint-delete\" class=\"browser-icon\" data-action=\"script-blueprint-delete\" "
            "title=\"Delete Blueprint\"><img src=\"icons/trash.png\"/></button>");
        body += UiWidgets::PropertyInfo("Blueprint", actor.scriptBlueprintPath.empty() ? "(inline)" : actor.scriptBlueprintPath);
        body += UiWidgets::CollapsibleSection("script-physics-section", "script-physics", "Physics",
            ScriptPhysicsControls("script-", actor.scriptPhysics.is_object() ? actor.scriptPhysics : nlohmann::json::object()),
            {"section"}, IsSectionExpanded("script-physics"));
        if (actor.scriptSpawned) body += UiWidgets::PropertyInfo("Lifetime", "Spawned");
        body += ScriptComponentControls(actor);
        body += UiWidgets::PropertyControlRow("Add Component",
            UiWidgets::Select("script-component-kind", {"script-component-kind"},
                ScriptComponentOptions()) +
            "<button id=\"script-component-add\" class=\"browser-icon\" data-action=\"script-component-add\" "
            "title=\"Add component\"><img src=\"icons/plus.png\"/></button>");
        body += UiWidgets::PropertyControlRow("Mesh Asset",
            UiWidgets::Input("script-component-mesh", {"script-component-mesh"}, ""));
        body += SceneLogicComponentsMarkup(actor);
        size_t index = 0;
        for (const auto& property : actor.scriptProperties.items())
        {
            const std::string id = "script-property-" + std::to_string(index);
            const UiWidgets::WidgetAction change{"script-property", index};
            const auto& value = property.value();
            const auto field = ScriptPropertyField(runtimeView.scripts.classes, actor.scriptClass, property.key(), value);
            const std::string reset = "<button id=\"script-reset-" + std::to_string(index) +
                "\" class=\"browser-icon\" data-action=\"script-reset\" data-index=\"" +
                std::to_string(index) + "\" title=\"Restore inherited value\"><img src=\"icons/refresh.png\"/></button>";
            body += "<div class=\"actor-property-row\">" +
                ScriptPropertyRow(property.key(), id, change, field, value, reset) + "</div>";
            ++index;
        }
        body += UiWidgets::PropertyControlRow("New Asset",
            UiWidgets::Input("script-asset-path", {"script-asset-path"}, "Common/Blueprints/BP_object.json") +
            "<button id=\"script-asset-create\" class=\"browser-icon\" data-action=\"script-asset-create\" "
            "title=\"Create Blueprint from this instance\"><img src=\"icons/save.png\"/></button>");
    }
    if (actor.primaryDirectionalLight) body += PrimaryDirectionalLightControls(actor);
    const std::string key = ActorSectionKey(spec->key);
    std::string markup = UiWidgets::CollapsibleSection(DetailsSectionId(key), key, actor.displayName, body,
        {"section"}, IsSectionExpanded(key));
    if (actor.type == EditorSceneActorType::Camera)
    {
        std::string exposure = "<div class=\"actor-property-row\">" +
            UiWidgets::PropertyControlRow("Exposure Mode",
            "<div class=\"horizontal-box segmented\">" +
            UiWidgets::Button("actor-exposure-manual", {"actor-exposure-manual"}, "Manual") +
            UiWidgets::Button("actor-exposure-auto", {"actor-exposure-auto"}, "Auto") + "</div>") + "</div>";
        exposure += "<div id=\"actor-exposure-physical-row\">" +
            UiWidgets::PropertyInfo("Camera EV100 (physical)", "", "actor-exposure-physical-ev") + "</div>";
        for (const auto& field : SceneActorEditableFields(actor))
            if (CameraExposureField(field.property) != nullptr)
                exposure += ActorPropertyRowMarkup(field);
        markup += UiWidgets::CollapsibleSection(DetailsSectionId("camera-exposure"),
            "camera-exposure", "Exposure", exposure, {"section"},
            IsSectionExpanded("camera-exposure"));
    }
    return markup;
}

std::string MaterialInstanceAssetEditorPanel::PrimaryDirectionalLightControls(
    const EditorSceneActorSnapshot& actor) const
{
    // 仓库里已经存在的两条运行时命令：SetSunIntensity 与 SetCsmCastShadows。
    // 它们只作用于主方向光（WorldBuilder 约定：场景里第一个 directionalLight），
    // 因此这两个控件只在这一行出现，并且不在只读字段里重复同一个值。
    std::vector<UiWidgets::NumericComponent> components;
    UiWidgets::NumericComponent intensity;
    intensity.id = "actor-sun-intensity";
    intensity.label = "Intensity";
    intensity.value = SceneActorNumber(actor.light.intensity);
    intensity.change = {"actor-sun-intensity", 0};
    // 范围与运行时页的 sun 滑条一致（0..150000 lux）；滑条只影响拖动粒度，文本输入仍是精确值。
    intensity.hasRange = true;
    intensity.minValue = SunIntensityMin;
    intensity.maxValue = SunIntensityMax;
    components.push_back(std::move(intensity));

    std::string markup = UiWidgets::NumericComponents(components);
    // CSM 开关沿用运行时页的两个按钮而不是下拉：RmlUi 的 select 在程序化写值时会
    // 自己派发 change，同一帧可能提交两次；点击只产生一个事件，语义与运行时页一致。
    markup += UiWidgets::PropertyControlRow("Cast Shadows (CSM)",
        UiWidgets::Button("actor-csm-enable", {"actor-csm-enable", 0}, "Enabled",
            UiWidgets::ButtonStyle::Command, runtimeView.csmCastShadows) +
        UiWidgets::Button("actor-csm-disable", {"actor-csm-disable", 0}, "Disabled",
            UiWidgets::ButtonStyle::Command, !runtimeView.csmCastShadows));
    return markup;
}

// 作者字段与模式高亮都跟随冻结快照；焦点控件（正在拖动/输入）不会被回写。
void MaterialInstanceAssetEditorPanel::RefreshActorValues(const EditorSceneActorSnapshot* actor)
{
    if (actor == nullptr) return;
    for (const EditorSceneActorField& field : SceneActorFields(*actor))
        SetMarkup(field.id, Text(field.value));
    for (const auto& field : SceneActorEditableFields(*actor))
        for (uint32_t component = 0; component < field.components; ++component)
        {
            const std::string id = SceneActorPropertyControlId(field.property, component);
            const std::string number = SceneActorNumber(field.value[component]);
            SetControlValue(id, number);
            SetDisabled(id, runtimeView.scripts.state != ScriptPlayState::Editing);
            // 数值输入与滑条共享同一属性；只刷文本会让滑条的 value 属性停在
            // DOM 创建时的旧值，下一次点击/拖动就会用旧值覆盖刚输入的数值。
            if (field.hasRange)
            {
                SetControlValue(id + "-slider", number);
                SetDisabled(id + "-slider", runtimeView.scripts.state != ScriptPlayState::Editing);
            }
        }
    if (actor->type == EditorSceneActorType::Script)
    {
        const bool editing = runtimeView.scripts.state == ScriptPlayState::Editing &&
            !actor->scriptSpawned && !runtimeView.offlineMode;
        const bool inlineEdit = editing && actor->scriptBlueprintPath.empty();
        SetControlValue("script-blueprint-name", actor->displayName);
        for (const char* id : {"script-blueprint-name", "script-blueprint-duplicate", "script-blueprint-delete"})
            SetDisabled(id, !editing);
        for (const char* id : {"script-component-add", "script-component-kind", "script-component-mesh"})
            SetDisabled(id, !inlineEdit);
        const auto& physics = actor->scriptPhysics;
        const std::string motion = physics.is_null() ? "none" : physics.value("motion", "static");
        SetControlValue("script-physics-motion", motion);
        SetDisabled("script-physics-motion", !inlineEdit);
        widgetDocument.SetProperty("script-physics-body", "display", physics.is_null() ? "none" : "block");
        const std::string shape = physics.is_null() ? "box" : physics.value("shape", "box");
        SetControlValue("script-physics-shape", shape);
        SetDisabled("script-physics-shape", !inlineEdit || motion == "character");
        SetDisabled("script-physics-trigger", !inlineEdit || motion == "character");
        if (auto* trigger = document->GetElementById("script-physics-trigger"))
        {
            const bool checked = !physics.is_null() && physics.value("trigger", false);
            if (checked && !trigger->HasAttribute("checked")) trigger->SetAttribute("checked", "checked");
            else if (!checked && trigger->HasAttribute("checked")) trigger->RemoveAttribute("checked");
        }
        widgetDocument.SetProperty("script-physics-halfExtents-row", "display", shape == "box" ? "block" : "none");
        SetControlValue("script-physics-component", physics.is_object() ? physics.value("component", "") : "");
        SetDisabled("script-physics-component", !inlineEdit);
        widgetDocument.SetProperty("script-physics-component-row", "display", shape == "mesh" ? "block" : "none");
        RefreshScriptConstraintControls(physics.is_object() ? physics : nlohmann::json::object(),
            "script-", !inlineEdit || motion == "character");
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const auto id = "script-physics-extents-" + std::to_string(axis);
            SetControlValue(id, physics.contains("halfExtents") ? physics.at("halfExtents")[axis].dump() : "50");
            SetDisabled(id, !inlineEdit);
        }
        for (const auto& field : ScriptPhysicsFields())
        {
            const auto id = "script-physics-" + std::string(field.name);
            const float fallback = std::string(field.name) == "mass" && motion == "character" ? 70 : field.defaultValue;
            SetControlValue(id, physics.contains(field.name) ? physics.at(field.name).dump() : Number(fallback));
            SetDisabled(id, !inlineEdit);
            widgetDocument.SetProperty(id + "-row", "display",
                ScriptPhysicsFieldVisible(physics, field.name) ? "block" : "none");
        }
        for (size_t component = 0; component < actor->scriptComponents.size(); ++component)
        {
            const auto& node = actor->scriptComponents[component];
            const auto componentId = ScriptComponentId(node);
            RefreshScriptCollisionControls(component, node, "script-", !inlineEdit);
            const bool inherited = !actor->scriptBlueprintPath.empty();
            const auto parentId = ComponentControlId(component, "parent");
            SetControlValue(parentId, node.value("parent", ""));
            SetDisabled(parentId, !inlineEdit);
            SetDisabled("script-component-remove-" + std::to_string(component), !inlineEdit);
            for (const auto& field : ScriptComponentInstanceFields(node))
            {
                const bool allowed = !inherited ||
                    ScriptComponentAllowsInstance(actor->scriptComponentInstanceEditable, componentId, field);
                const auto value = ScriptComponentFieldValue(node, field);
                const bool overridden = actor->scriptComponentOverrides.contains(componentId) &&
                    actor->scriptComponentOverrides.at(componentId).contains(field);
                for (size_t axis = 0; axis < (value.is_array() ? value.size() : 1u); ++axis)
                {
                    const auto id = ComponentControlId(component, field, axis);
                    const auto& current = value.is_array() ? value[axis] : value;
                    SetControlValue(id, current.is_string() ? current.get<std::string>() : current.dump());
                    SetDisabled(id, !editing || !allowed);
                    widgetDocument.SetClass(id, "script-overridden", overridden);
                }
                SetDisabled(ComponentResetControlId(component, field), !editing || !allowed || !overridden);
            }
        }
        size_t index = 0;
        for (const auto& property : actor->scriptProperties.items())
        {
            const bool exposed = std::find(actor->scriptInstanceEditable.begin(),
                actor->scriptInstanceEditable.end(), property.key()) != actor->scriptInstanceEditable.end();
            const auto& value = property.value();
            const auto schema = ScriptPropertyField(runtimeView.scripts.classes,
                actor->scriptClass, property.key(), value);
            RefreshScriptPropertyControl(*document, widgetDocument, "script-property-" + std::to_string(index),
                {"script-property", index}, schema, value, !editing || !exposed,
                actor->scriptOverrides.contains(property.key()), nlohmann::json::array(),
                actor->scriptOverrides.value(property.key(), nlohmann::json(nullptr)));
            SetDisabled("script-reset-" + std::to_string(index), !editing || !exposed ||
                !actor->scriptOverrides.contains(property.key()));
            ++index;
        }
        SetDisabled("script-asset-create", runtimeView.scripts.state != ScriptPlayState::Editing);
        RefreshSceneLogicComponents(*actor);
    }
    if (actor->type == EditorSceneActorType::Camera)
    {
        const auto& exposure = actor->camera.exposure;
        // 模式来自作者快照；只切换行可见性，保留数值 DOM、焦点和原有 physical 配置。
        widgetDocument.SetClass("actor-exposure-manual", "selected", exposure.mode != ExposureMode::Auto);
        widgetDocument.SetClass("actor-exposure-auto", "selected", exposure.mode == ExposureMode::Auto);
        widgetDocument.SetProperty("actor-exposure-physical-row", "display",
            exposure.mode == ExposureMode::PhysicalCamera ? "block" : "none");
        SetMarkup("actor-exposure-physical-ev", Text(Number(static_cast<float>(EffectiveEv100(exposure)))));
        for (const auto& field : SceneActorEditableFields(*actor))
            if (CameraExposureField(field.property) != nullptr)
                widgetDocument.SetProperty(SceneActorPropertyControlId(field.property, 0) + "-row",
                    "display", CameraExposureFieldEditable(field.property, exposure.mode) ? "block" : "none");
    }
    if (!actor->primaryDirectionalLight) return;
    // 文本输入承载精确值；滑条只承载 0..10 的显示区间，越界值在这里按显示区间截断
    // （只影响滑条位置，不改变存储值）。拖动中的滑条是焦点元素，SetControlValue 不回写。
    const float intensity = actor->light.intensity;
    SetControlValue("actor-sun-intensity", SceneActorNumber(intensity));
    SetControlValue("actor-sun-intensity-slider",
        SceneActorNumber(std::min(std::max(intensity, SunIntensityMin), SunIntensityMax)));
    // CSM 是高亮来自冻结的运行时快照，不是面板自己的第二份状态。
    widgetDocument.SetClass("actor-csm-enable", "selected", runtimeView.csmCastShadows);
    widgetDocument.SetClass("actor-csm-disable", "selected", !runtimeView.csmCastShadows);
}

void MaterialInstanceAssetEditorPanel::ApplySectionState()
{
    // 展开状态只落在既有分组元素的 class 上：重建 DOM 会丢焦点和滚动位置，
    // 而面板每帧都从冻结快照刷新，静止帧必须写 0 次属性（SetClass 自带值缓存）。
    for (const char* key : DetailsSectionKeys)
        widgetDocument.SetClass(DetailsSectionId(key), "collapsed", !IsSectionExpanded(key));
    for (const EditorSceneActorGroupSpec& spec : SceneActorGroupSpecs())
    {
        const std::string key = ActorSectionKey(spec.key);
        widgetDocument.SetClass(DetailsSectionId(key), "collapsed", !IsSectionExpanded(key));
    }
}

void MaterialInstanceAssetEditorPanel::ToggleSection(const std::string& key)
{
    if (key.empty()) return;
    if (collapsedSections.erase(key) == 0) collapsedSections.insert(key);
    ApplySectionState();
}

void MaterialInstanceAssetEditorPanel::OpenResourceAsset(
    const std::string& path, EditorAssetKind kind)
{
    FlushBlueprintAssetEdits();
    if (!resourceEditor.Open(path, kind))
    {
        localStatus = "Too many resource editors are open.";
        return;
    }
    viewingJson = false;
    resourceEditorVisible = true;
    localStatus.clear();
    RequestActiveResourceMetadata();
    // 作者 owner 的生命周期属于资产标签，不能依赖 Simulation 段是否可见。
    if (kind == EditorAssetKind::Tressfx)
        RequestTressfxDocument(path);
    if (kind == EditorAssetKind::Blueprint)
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::GetBlueprintDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
    }
    if (kind == EditorAssetKind::Mesh || kind == EditorAssetKind::Tressfx)
    {
        // 毛发资产：目录扫描已带顶层 type 与 simulation.skinning，打开时即可决定
        // 是否请求预览。带 skinning 的资产不进入视口（由蓝图 TressFX 组件承载），
        // 这里直接跳过请求；快照到达后还会以作者文档复核一次。
        const EditorJsonAssetEntry* entry = FindCatalogEntry(snapshot.assetCatalog, path);
        hairPreviewPath = kind == EditorAssetKind::Tressfx ? path : std::string();
        hairPreviewBlocked = entry != nullptr && entry->tressfxSkinning;
        hairRotation.Reset();
        if (hairPreviewBlocked)
            localStatus = std::string(TressfxHairSkinnedAssetMessage);
        else if (runtimeActionSink != nullptr)
        {
            // 回调只投递路径；GT 复用 previewmodel 的 LoadWorld 事务，不在 UI 内加载资产。
            // 即使标签已打开也重新请求，保证切过关卡后再次双击仍能回到该模型。
            RequestModelPreview(path);
        }
    }
    RefreshResourceEditor();
    docking.SetPanelVisible(ResourcePanelId(path), true);
    docking.Activate(ResourcePanelId(path));
}

void MaterialInstanceAssetEditorPanel::RequestCloseResource(const std::string& path)
{
    for (const auto& tab : resourceEditor.Tabs())
    {
        if (tab.path != path) continue;
        const auto* authored = ResourceDocument(path);
        if ((authored && authored->dirty) || pendingBlueprintPath == path ||
            (hairSimulationPath == path && hairSimulationPendingSave))
        {
            FlushBlueprintAssetEdits();
            pendingClosePath = path;
            pendingCloseKind = tab.kind;
            saveThenClose = saveCloseSubmitted = false;
            discardBlueprintThenClose = discardBlueprintCloseSubmitted = false;
            return;
        }
        CloseResource(path);
        return;
    }
}

void MaterialInstanceAssetEditorPanel::CloseResource(const std::string& path)
{
    const std::string next = docking.PanelAfterClose(ResourcePanelId(path));
    resourceEditor.Close(path);
    requestedTressfxDocuments.erase(path);
    // 后台关闭只移除标签，不切预览 World，也不覆盖当前页面的选择。
    RefreshResourceEditor();
    if (!next.empty()) ActivateWorkspacePanel(next);
}

void MaterialInstanceAssetEditorPanel::ActivateWorkspacePanel(const std::string& panel)
{
    if (panel == "viewport")
    {
        docking.SetPanelVisible("viewport", true);
        ReturnToLevel();
        return;
    }
    const std::string path = docking.ResourcePathForPanel(panel);
    if (path.empty()) return;
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    viewingJson = false;
    docking.SetPanelVisible(panel, true);
    docking.Activate(panel);
    if (panel.rfind("resource:material:", 0) == 0)
    {
        resourceEditorVisible = false;
        EditorCommandEnvelope command;
        command.type = EditorCommandType::SelectMaterialInstanceDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
        return;
    }
    if (!resourceEditor.Activate(path)) return;
    resourceEditorVisible = true;
    RequestActiveResourceMetadata();
    const auto* active = resourceEditor.ActiveTab();
    if (active && (active->kind == EditorAssetKind::Mesh || active->kind == EditorAssetKind::Tressfx) &&
        !HasCurrentModelPreview())
        RequestModelPreview(path);
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::RequestActiveResourceMetadata()
{
    const auto* active = resourceEditor.ActiveTab();
    // 读取器只保留最新请求；切回未完成的标签须补读，已缓存的标签不重复 IO。
    if (active != nullptr && (active->snapshot == nullptr || active->loading) &&
        jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(active->path);
}

void MaterialInstanceAssetEditorPanel::RequestResourceReload()
{
    const auto* tab = resourceEditor.ActiveTab();
    if (tab && tab->kind == EditorAssetKind::Retarget && tab->snapshot)
    {
        // GT 的 loading 快照可能晚一帧；请求发出就封住旧图像，直到新元数据 revision 到达。
        reloadingRetargetPath = tab->path;
        reloadingRetargetRevision = tab->snapshot->revision;
        previewDragButton = -1;
    }
    if (tab && tab->kind == EditorAssetKind::Blueprint)
    {
        if (!pendingBlueprintPath.empty()) { localStatus = "Save or revert pending edits before reloading."; return; }
        SubmitBlueprintAssetCommand(EditorCommandType::ReloadBlueprintDocument, tab->path);
        return;
    }
    if (tab && tab->kind == EditorAssetKind::Tressfx)
    {
        const auto* authored = TressfxDocument(tab->path);
        if (!authored || authored->dirty || hairSimulationPendingSave)
        {
            localStatus = "Save or revert pending edits before reloading.";
            return;
        }
        SubmitResourceDocumentCommand(EditorCommandType::ReloadTressfxDocument, tab->path);
    }
    resourceEditor.RequestReload();
    const std::string path = resourceEditor.ConsumeReloadRequest();
    if (path.empty()) return;
    if (jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && (active->kind == EditorAssetKind::Mesh || active->kind == EditorAssetKind::Tressfx))
        RequestModelPreview(path);
}

void MaterialInstanceAssetEditorPanel::RequestModelPreview(const std::string& path)
{
    // 打开、切标签与 Reload 共用入口；蒙皮资产只能由角色组件提供预览姿态。
    const auto* active = resourceEditor.ActiveTab();
    if (active && active->path == path && active->kind == EditorAssetKind::Tressfx)
    {
        const auto* entry = FindCatalogEntry(snapshot.assetCatalog, path);
        const bool blocked = active->snapshot ? active->snapshot->tressfxSkinning :
            entry != nullptr && entry->tressfxSkinning;
        if (blocked) return;
    }
    RequestAssetPreview(path, false);
}

void MaterialInstanceAssetEditorPanel::RequestAssetPreview(const std::string& path, bool material,
    const std::string& retargetPath, uint64_t retargetRevision)
{
    previewDragButton = -1;
    if (runtimeActionSink == nullptr) return;
    if (!runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() != "SC_model_preview.json")
        levelWorldPath = runtimeView.activeWorldPath;
    // 新的预览目标从零姿态开始；旋转只属于当前预览世界，不跨资产累积。
    if (requestedPreviewPath != path) hairRotation.Reset();
    requestedPreviewPath = path;
    requestedRetargetPath = retargetPath;
    requestedRetargetRevision = retargetRevision;
    initializedRetargetGeneration = 0;
    requestedPreviewMaterial = material;
    requestedPreviewBlueprint = false;
    requestedPreviewGeneration = 0;
    for (const auto& actor : OutlinerActors())
        requestedPreviewGeneration = std::max(requestedPreviewGeneration, actor.worldGeneration);
    UiAction request;
    request.type = material ? UiActionType::PreviewMaterial : retargetPath.empty() ?
        UiActionType::PreviewModel : UiActionType::PreviewRetarget;
    request.stringValue = retargetPath.empty() ? path : retargetPath;
    if (!retargetPath.empty()) request.retargetPreviewModel = path;
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

bool MaterialInstanceAssetEditorPanel::IsResourceEditorVisible() const
{
    for (const auto& tab : resourceEditor.Tabs())
        if (docking.IsPanelActive(ResourcePanelId(tab.path))) return true;
    return false;
}

bool MaterialInstanceAssetEditorPanel::PrepareImmersiveView(std::string& levelToLoad)
{
    levelToLoad.clear();
    if (HasModal()) return false;
    const bool previewWorld = !requestedPreviewPath.empty() ||
        std::filesystem::path(runtimeView.activeWorldPath).filename() == "SC_model_preview.json";
    if (previewWorld)
    {
        if (levelWorldPath.empty())
        {
            localStatus = "Open a level before entering immersive mode.";
            return false;
        }
        levelToLoad = levelWorldPath;
        requestedPreviewPath.clear();
    }
    // 这里只选择目标页面；宿主等关卡事务完成后隐藏 UI，失败时仍保留可操作的工作区。
    resourceEditorVisible = false;
    docking.SetPanelVisible("viewport", true);
    docking.Activate("viewport");
    return true;
}

void MaterialInstanceAssetEditorPanel::ReturnToLevel()
{
    previewDragButton = -1;
    resourceEditorVisible = false;
    docking.Activate("viewport");
    // 当前只有一个活动 World；返回关卡须重新走加载事务，不能把模型 World 冒充关卡。
    if (!requestedPreviewPath.empty() && !levelWorldPath.empty() && runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = UiActionType::LoadWorld;
        request.stringValue = levelWorldPath;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
        requestedPreviewPath.clear();
    }
}

bool MaterialInstanceAssetEditorPanel::HasCurrentModelPreview() const
{
    return CurrentModelPreviewGeneration() != 0;
}

bool MaterialInstanceAssetEditorPanel::IsMotionDebugViewport() const
{
    return docking.IsPanelActive("viewport") ||
        (IsResourceEditorVisible() && HasCurrentModelPreview());
}

bool MaterialInstanceAssetEditorPanel::IsSceneViewportElement(const Rml::Element* element) const
{
    // 候选表可滚动，命中属于编辑器，不能同时触发角色输入或关卡拾取。
    for (auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current->GetId() == "viewport-motion-matching") return false;
    return docking.IsSceneViewportElement(element);
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentModelPreviewGeneration() const
{
    if (requestedPreviewMaterial) return 0;
    const auto* active = resourceEditor.ActiveTab();
    if (!active) return 0;
    if (active->kind == EditorAssetKind::Retarget)
    {
        if (!retargetEditor.AnimationVisible() || active->loading || !active->snapshot ||
            !active->snapshot->retargetPreview || active->path != requestedRetargetPath ||
            active->snapshot->revision != requestedRetargetRevision) return 0;
    }
    else if ((active->kind != EditorAssetKind::Mesh && active->kind != EditorAssetKind::Tressfx) ||
        active->path != requestedPreviewPath) return 0;
    // JSON 读取完成不能证明 GPU 模型已加载；只在新预览 World 的冻结快照发布后展示。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_model" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
        {
            if (active->kind == EditorAssetKind::Retarget)
            {
                bool paired = false;
                for (const auto& source : OutlinerActors())
                    paired |= source.worldGeneration == actor.worldGeneration &&
                        source.sceneObjectIdentity == "preview_retarget_source|" + requestedRetargetPath;
                if (!paired) continue;
            }
            return actor.worldGeneration;
        }
    return 0;
}

bool MaterialInstanceAssetEditorPanel::IsMaterialEditorVisible() const
{
    return snapshot.activeDocument &&
        docking.IsPanelActive(MaterialPanelId(snapshot.activeDocument->assetPath));
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentMaterialPreviewGeneration() const
{
    if (!requestedPreviewMaterial || !snapshot.activeDocument ||
        snapshot.activeDocument->assetPath != requestedPreviewPath) return 0;
    // 只认已发布的新 World 和同路径材质槽，切标签时不能把上一颗球的画面当成新材质。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_material" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
            for (const auto& section : actor.sections)
                if (section.materialInstancePath == requestedPreviewPath) return actor.worldGeneration;
    return 0;
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentPreviewCameraGeneration() const
{
    return IsMaterialEditorVisible() ? CurrentMaterialPreviewGeneration() :
        IsResourceEditorVisible() ? (requestedPreviewBlueprint ? CurrentBlueprintPreviewGeneration() :
            CurrentModelPreviewGeneration()) : 0;
}

void MaterialInstanceAssetEditorPanel::RefreshMaterialPreview()
{
    if (IsMaterialEditorVisible() && (!requestedPreviewMaterial ||
        requestedPreviewPath != snapshot.activeDocument->assetPath))
        RequestAssetPreview(snapshot.activeDocument->assetPath, true);
    // 最后一份材质关闭后，恢复记住的关卡；不能留下预览 World 冒充关卡。
    if (requestedPreviewMaterial && snapshot.documentTabs.empty() && docking.IsPanelActive("viewport"))
        ReturnToLevel();
    const uint64_t generation = CurrentMaterialPreviewGeneration();
    widgetDocument.SetProperty("material-preview-image", "display", generation ? "block" : "none");
    widgetDocument.SetProperty("material-preview-status", "display", generation ? "none" : "block");
    SetDisabled("material-frame", !generation);
    SetMarkup("material-preview-status", Text(snapshot.activeDocument && !snapshot.activeDocument->previewMessage.empty()
        ? snapshot.activeDocument->previewMessage : "Loading material preview..."));
    // 新球从磁盘基线加载；重连成功后补一次未保存草稿，避免切回标签丢失预览效果。
    if (generation && IsMaterialEditorVisible() && appliedMaterialPreviewGeneration != generation &&
        snapshot.activeDocument->preview == EditorPreviewStatus::Connected)
    {
        appliedMaterialPreviewGeneration = generation;
        if (snapshot.activeDocument->status == EditorDocumentStatus::Dirty)
            SubmitPathCommand(EditorCommandType::ApplyMaterialInstancePreview);
    }
}

bool MaterialInstanceAssetEditorPanel::IsActiveHairPreview() const
{
    const auto* active = resourceEditor.ActiveTab();
    return active != nullptr && active->kind == EditorAssetKind::Tressfx &&
        active->path == hairPreviewPath && !hairPreviewBlocked;
}

void MaterialInstanceAssetEditorPanel::RequestHairPreviewRotation(float deltaX, float deltaY)
{
    if (runtimeActionSink == nullptr || hairPreviewBlocked || deltaX == 0.0f) return;
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (!generation) return;
    // 累计角度由 UI 维护，GT 只写绝对 TRS；World 使用原始场景身份 preview_model。
    hairRotation.Drag(deltaX, deltaY);
    UiAction action;
    action.type = UiActionType::RotatePreviewModel;
    action.previewRotation = {generation, hairRotation.yaw, 0.0f};
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::RequestHairReset()
{
    // 复位不是重载：渲染线程在下一个正 delta 帧走一次 ResetPositions，
    // 20 MB TFX 不会被重新解析。
    if (runtimeActionSink == nullptr || !IsActiveHairPreview()) return;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active ? TressfxDocument(active->path) : nullptr;
    if (!authored || !TressfxGpuSimulationDefaults(authored->document)) return;
    if (!CurrentModelPreviewGeneration()) return;
    UiAction action;
    action.type = UiActionType::ResetHairSimulation;
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

namespace
{
// 面板当前值（可能是用户拖动后的会话值）优先于作者文档；缺字段时回退控件默认值。
std::array<double, 3> PanelSimulationValue(
    const nlohmann::json& values, const TressfxSimulationControl& control)
{
    std::array<double, 3> result = control.defaults;
    if (!values.is_object() || !values.contains(control.field)) return result;
    const auto& array = values.at(control.field);
    if (!array.is_array() || array.size() != 3) return result;
    for (std::size_t component = 0; component < 3; ++component)
        if (array[component].is_number()) result[component] = array[component].get<double>();
    return result;
}
} // namespace

const EditorAssetDocumentResultPayload* MaterialInstanceAssetEditorPanel::TressfxDocument(
    const std::string& path) const
{
    for (const auto& entry : snapshot.tressfxDocuments)
        if (entry.assetPath == path) return &entry;
    return nullptr;
}

const EditorAssetDocumentResultPayload* MaterialInstanceAssetEditorPanel::ResourceDocument(
    const std::string& path) const
{
    const auto* blueprint = BlueprintDocument(path);
    return blueprint != nullptr ? blueprint : TressfxDocument(path);
}

void MaterialInstanceAssetEditorPanel::SubmitResourceDocumentCommand(
    EditorCommandType type, const std::string& path)
{
    // 共用关闭/保存状态机，但 revision 始终来自该 kind 唯一的作者 owner。
    const auto* authored = ResourceDocument(path);
    if (!authored) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = authored->documentRevision;
    if (GetEditorAssetOperation(type) == EditorAssetOperation::Reload)
        command.payload = ReloadMaterialInstanceDocumentPayload{path, EditorDirtyDocumentPolicy::RequireClean};
    else command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::RequestTressfxDocument(const std::string& path)
{
    if (path.empty() || TressfxDocument(path) || !requestedTressfxDocuments.insert(path).second)
        return;
    // 快照晚于 UI 事件：按路径去重首次读取，不依赖某个 Details 段的可见性。
    EditorCommandEnvelope command;
    command.type = EditorCommandType::GetTressfxDocument;
    command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::RequestHairSimulationDocument(const std::string& path)
{
    if (path.empty() || (hairSimulationRequested && hairSimulationPath == path)) return;
    hairSimulationPath = path;
    hairSimulationRequested = true;
    hairSimulationBoundRevision = 0;
    hairSimulationSaveRevision = 0;
    hairSimulationPendingSave = false;
    hairSimulationSaveWaitTicks = 0;
    hairSimulationValues = nlohmann::json::object();
    hairSimulationStatus = "Reading simulation values...";
    RequestTressfxDocument(path);
}

void MaterialInstanceAssetEditorPanel::RefreshHairSimulationSection(bool isHair, bool hairBlocked)
{
    const auto* active = resourceEditor.ActiveTab();
    const bool visible = isHair && !hairBlocked && active != nullptr;
    widgetDocument.SetProperty("resource-hair-simulation", "display", visible ? "block" : "none");
    if (!visible) return;
    if (!hairSimulationRequested || hairSimulationPath != active->path)
        RequestHairSimulationDocument(active->path);
    const auto* document = TressfxDocument(active->path);
    if (document != nullptr)
    {
        // 首次绑定或外部 revision 变化（保存/重载/其他编辑）时重新取作者值。
        if (hairSimulationBoundRevision != document->documentRevision)
        {
            const uint64_t previousRevision = hairSimulationBoundRevision;
            hairSimulationBoundRevision = document->documentRevision;
            const nlohmann::json simulation = document->document.contains("simulationDefaults")
                ? document->document.at("simulationDefaults") : nlohmann::json::object();
            std::size_t count = 0;
            const TressfxSimulationControl* controls = TressfxSimulationControls(count);
            nlohmann::json values = nlohmann::json::object();
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto current = TressfxSimulationValue(simulation, controls[index]);
                values[controls[index].field] = nlohmann::json::array(
                    {current[0], current[1], current[2]});
            }
            hairSimulationValues = std::move(values);
            // 保存成功后草稿回到 clean：明确提示要重载才会应用作者值并复位模拟。
            hairSimulationStatus = previousRevision != 0 && !document->dirty
                ? "Defaults saved to the hair asset. Reload to apply inherited values and reset the simulation."
                : hairSimulationPendingSave ? hairSimulationStatus
                : "Values loaded from the asset draft.";
        }
        // modify 成功后草稿变 dirty 且 revision 前进；把保存交给既有 Save Content 弹窗，
        // 由它按快照 revision 提交 tressfx.save。候选与草稿已相同的情况不推进 revision，
        // 因此额外用值相等判定解锁，避免停在“正在暂存”。
        if (hairSimulationPendingSave)
        {
            const nlohmann::json simulation = document->document.contains("simulationDefaults")
                ? document->document.at("simulationDefaults") : nlohmann::json::object();
            const bool staged = document->documentRevision > hairSimulationSaveRevision ||
                (document->dirty && TressfxSimulationValuesMatch(simulation, hairSimulationValues));
            if (staged)
            {
                hairSimulationPendingSave = false;
                hairSimulationSaveWaitTicks = 0;
                hairSimulationStatus = "Confirm the save in the Save Content dialog (writes the hair asset).";
                if (pendingClosePath != active->path)
                    OpenSaveDialog(active->path);
            }
            else if (++hairSimulationSaveWaitTicks > 180)
            {
                hairSimulationPendingSave = false;
                hairSimulationSaveWaitTicks = 0;
                hairSimulationStatus = snapshot.statusMessage.empty()
                    ? "Save failed: the asset draft was not modified." : snapshot.statusMessage;
            }
        }
    }
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    std::string rows;
    for (std::size_t index = 0; index < count; ++index)
    {
        std::vector<UiWidgets::NumericComponent> components;
        for (uint32_t component = 0; component < controls[index].components; ++component)
        {
            UiWidgets::NumericComponent entry;
            entry.id = "hair-sim-" + std::to_string(index) + "-" + std::to_string(component);
            entry.label = controls[index].components == 1
                ? std::string() : std::string(1, "XYZ"[component]);
            // markup 只描述固定控件结构；值在挂载后单独同步，避免编辑时替换焦点/拖动节点。
            entry.value = TressfxSimulationText(controls[index].defaults[component]);
            entry.change = {"hair-sim", index, component};
            if (controls[index].hasRange)
            {
                entry.hasRange = true;
                entry.minValue = controls[index].minValue;
                entry.maxValue = controls[index].maxValue;
            }
            components.push_back(std::move(entry));
        }
        std::string label = controls[index].label;
        if (controls[index].unit[0] != '\0')
            label += std::string(" (") + controls[index].unit + ")";
        rows += UiWidgets::PropertyControlRow(label, UiWidgets::NumericComponents(components));
    }
    rows += "<div class=\"hair-sim-footer\">" +
        UiWidgets::Button("hair-sim-save", {"hair-save"},
            "Save to Hair Asset",
            UiWidgets::ButtonStyle::Command, false, "icons/save.png") +
        "<span id=\"hair-sim-status\" class=\"hair-sim-status\"></span></div>";
    // 这是资产级默认值编辑：保存写 TF_*.json，只影响未覆写这些字段的挂载；实时值只属于
    // 当前预览会话。必须写清，避免作者误以为在改关卡里的这一根头发。
    rows += "<p class=\"hair-sim-note\">Drag previews in this viewport only (not saved). "
        "Save writes the hair asset defaults (TF_*.json): inherited placement values are affected, and reloading "
        "resets the simulation. Placement overrides are edited in the Blueprint asset.</p>";
    SetMarkup("resource-hair-simulation", UiWidgets::CollapsibleSection(
        DetailsSectionId("hair-simulation"), "hair-simulation", "Simulation Defaults", rows,
        {"section"}, IsSectionExpanded("hair-simulation")));
    SetMarkup("hair-sim-status", Text(hairSimulationStatus));
    SetDisabled("hair-sim-save", document == nullptr);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto values = PanelSimulationValue(hairSimulationValues, controls[index]);
        for (uint32_t component = 0; component < controls[index].components; ++component)
        {
            const std::string id = "hair-sim-" + std::to_string(index) + "-" + std::to_string(component);
            SetControlValue(id, TressfxSimulationText(values[component]));
            if (controls[index].hasRange)
                SetControlValue(id + "-slider", TressfxSimulationText(values[component]));
        }
    }
}

void MaterialInstanceAssetEditorPanel::SubmitHairSimulationLive(std::size_t controlIndex,
    const std::array<double, 3>& values)
{
    if (runtimeActionSink == nullptr || runtimeView.offlineMode || !IsActiveHairPreview()) return;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active ? TressfxDocument(active->path) : nullptr;
    if (!authored || !TressfxGpuSimulationDefaults(authored->document)) return;
    // 预览 World 还没发布时不要投递：命令队列不绑定 World 世代，早到的值会落到
    // 之后出现的同名对象上。
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (!generation) return;
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    if (controlIndex >= count) return;
    UiAction action;
    action.type = UiActionType::SetHairSimulationParameter;
    action.previewHairParameter.worldGeneration = generation;
    action.previewHairParameter.field = controls[controlIndex].field;
    action.previewHairParameter.valueCount = controls[controlIndex].components;
    for (uint32_t component = 0; component < 3; ++component)
        action.previewHairParameter.values[component] = static_cast<float>(values[component]);
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::SubmitHairSimulationSave()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr) return;
    const auto* document = TressfxDocument(active->path);
    if (document == nullptr)
    {
        // 文档还没打开：先取一次，等快照到达后再提交修改。
        RequestHairSimulationDocument(active->path);
        hairSimulationRequested = true;
        hairSimulationPendingSave = true;
        hairSimulationSaveRevision = 0;
        hairSimulationSaveWaitTicks = 0;
        hairSimulationStatus = "Reading the asset document...";
        return;
    }
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    ModifyAssetDocumentPayload payload;
    payload.assetPath = active->path;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto values = PanelSimulationValue(hairSimulationValues, controls[index]);
        EditorAssetFieldChange change;
        change.field = std::string("simulationDefaults.") + controls[index].field;
        if (controls[index].components == 1)
            change.value = values[0];
        else
            change.value = std::array<double, 3>{values[0], values[1], values[2]};
        payload.changes.push_back(std::move(change));
    }
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyTressfxDocument;
    command.expectedDocumentRevision = document->documentRevision;
    command.payload = std::move(payload);
    Submit(std::move(command));
    hairSimulationPendingSave = true;
    hairSimulationSaveRevision = document->documentRevision;
    hairSimulationSaveWaitTicks = 0;
    hairSimulationStatus = "Staging simulation values...";
}

void MaterialInstanceAssetEditorPanel::RequestPreviewCamera(ModelPreviewNavigation mode, float x, float y)
{
    const uint64_t generation = CurrentPreviewCameraGeneration();
    if (!runtimeActionSink || !generation || HasModal()) return;
    UiAction action;
    action.type = UiActionType::NavigateModelPreview;
    action.previewCamera = {mode, generation, x, y};
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

bool MaterialInstanceAssetEditorPanel::ProcessModelPreviewInput(Rml::Event& event)
{
    const uint64_t generation = CurrentPreviewCameraGeneration();
    if (!generation || HasModal() || docking.IsDragging())
    {
        previewDragButton = -1;
        return false;
    }
    if (previewDragGeneration != generation) previewDragButton = -1;
    const bool imageHit = event.GetTargetElement() == document->GetElementById(
        IsMaterialEditorVisible() ? "material-preview-image" :
            requestedPreviewBlueprint ? "blueprint-asset-preview-image" :
                !requestedRetargetPath.empty() ? "retarget-preview-image" : "resource-preview-image");
    const auto& type = event.GetType();
    const float x = event.GetParameter<float>("mouse_x", 0);
    const float y = event.GetParameter<float>("mouse_y", 0);
    if (type == "mousedown" && imageHit)
    {
        const int button = event.GetParameter<int>("button", -1);
        if (button < 0 || button > 2) return false;
        CloseMenus();
        CloseContextMenu();
        previewDragButton = button;
        previewDragGeneration = generation;
        previewPointerX = x;
        previewPointerY = y;
        return true;
    }
    if (type == "mousemove" && previewDragButton >= 0)
    {
        // 手势归开始命中的图像所有；拖到 Details 上仍继续，到抬起/失焦才取消。
        const float dx = x - previewPointerX;
        const float dy = y - previewPointerY;
        previewPointerX = x;
        previewPointerY = y;
        if (dx != 0 || dy != 0)
        {
            // 毛发视口：左键转模型（转根点才会触发毛发惯性），中/右键仍转相机。
            // 相机绕轨道对物理没有作用，因此两条手势必须分开。
            if (previewDragButton == 0 && IsActiveHairPreview())
                RequestHairPreviewRotation(dx, dy);
            else
                RequestPreviewCamera(previewDragButton == 2 ? ModelPreviewNavigation::Pan : ModelPreviewNavigation::Orbit, dx, dy);
        }
        return true;
    }
    if (type == "mouseup" && previewDragButton == event.GetParameter<int>("button", -2))
    {
        previewDragButton = -1;
        return true;
    }
    if (type == "mousescroll" && imageHit)
    {
        RequestPreviewCamera(ModelPreviewNavigation::Zoom, 0, event.GetParameter<float>("wheel_delta_y", 0));
        return true;
    }
    if (type == "dblclick" && imageHit)
    {
        RequestPreviewCamera(ModelPreviewNavigation::Frame);
        return true;
    }
    return false;
}

void MaterialInstanceAssetEditorPanel::ResetTextureView()
{
    textureZoom = 1.0f;
    texturePan = {0, 0};
    textureDragButton = -1;
}

bool MaterialInstanceAssetEditorPanel::ProcessTexturePreviewInput(Rml::Event& event)
{
    const auto* active = resourceEditor.ActiveTab();
    if (!active || active->kind != EditorAssetKind::Texture || active->loading ||
        !active->snapshot || !active->snapshot->texturePreview || !IsResourceEditorVisible() ||
        HasModal() || docking.IsDragging() ||
        textureViewSource != EditorTexturePreviewSource(active->snapshot->revision))
    {
        textureDragButton = -1;
        return false;
    }
    auto* surface = document->GetElementById("resource-preview-surface");
    bool hit = false;
    for (auto* element = event.GetTargetElement(); element; element = element->GetParentNode())
        if (element == surface) { hit = true; break; }
    const auto& type = event.GetType();
    const Rml::Vector2f pointer(event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0));
    if (type == "mousedown" && hit)
    {
        const int button = event.GetParameter<int>("button", -1);
        if (button < 0 || button > 2) return false;
        CloseMenus();
        CloseContextMenu();
        textureDragButton = button;
        texturePointer = pointer;
        return true;
    }
    if (type == "mousemove" && textureDragButton >= 0)
    {
        texturePan += pointer - texturePointer;
        texturePointer = pointer;
    }
    else if (type == "mouseup" && textureDragButton == event.GetParameter<int>("button", -2))
    {
        textureDragButton = -1;
        return true;
    }
    else if (type == "mousescroll" && hit)
    {
        const float delta = event.GetParameter<float>("wheel_delta_y", 0);
        const float nextZoom = std::clamp(textureZoom * std::exp(-delta * 0.15f), 0.1f, 32.0f);
        const auto size = surface->GetBox().GetSize(Rml::BoxArea::Padding);
        const auto anchor = pointer - surface->GetAbsoluteOffset(Rml::BoxArea::Padding) - size * 0.5f;
        // 保持鼠标下的图片坐标不动；只改 UI 几何，不重新解码/上传像素。
        texturePan = anchor - (anchor - texturePan) * (nextZoom / textureZoom);
        textureZoom = nextZoom;
    }
    else if (type == "dblclick" && hit) ResetTextureView();
    else return false;
    RefreshResourceEditor();
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshDocumentTabs()
{
    std::vector<EditorDockResourceTab> tabs;
    for (const auto& tab : resourceEditor.Tabs())
    {
        const auto* actor = ResourceDocument(tab.path);
        tabs.push_back({ResourcePanelId(tab.path), tab.path,
            std::filesystem::path(tab.path).filename().string() + (actor && actor->dirty ? " *" : ""),
            EditorAssetKindIcon(tab.kind), -1});
    }
    for (size_t index = 0; index < snapshot.documentTabs.size(); ++index)
    {
        const auto& tab = snapshot.documentTabs[index];
        tabs.push_back({MaterialPanelId(tab.assetPath), tab.assetPath,
            std::filesystem::path(tab.assetPath).filename().string() + (tab.dirty ? " *" : ""),
            EditorAssetKindIcon(EditorAssetKind::MaterialInstance), static_cast<int>(index)});
    }
    docking.SetResourceTabs(tabs);
}

void MaterialInstanceAssetEditorPanel::RefreshAnimationControls(const std::string& prefix, bool enabled)
{
    SetDisabled(prefix + "-play", !enabled || runtimeView.animationPlaying || runtimeView.animationClips.empty());
    SetDisabled(prefix + "-pause", !enabled || !runtimeView.animationPlaying);
    SetDisabled(prefix + "-clip", !enabled || runtimeView.animationClips.empty());
    SetDisabled(prefix + "-time", !enabled || runtimeView.animationDuration <= 0);
    if (!enabled) { SetMarkup(prefix + "-status", ""); return; }
    SetMarkup(prefix + "-status", Text(std::to_string(runtimeView.animationJointCount) +
        " joints | " + SceneActorNumber(runtimeView.animationTime) + " / " +
        SceneActorNumber(runtimeView.animationDuration) + " s"));
    std::string options;
    for (size_t i = 0; i < runtimeView.animationClips.size(); ++i)
        options += "<option value=\"" + std::to_string(i) + "\">" + Text(runtimeView.animationClips[i]) + "</option>";
    widgetDocument.SetMarkup(prefix + "-clip", options);
    widgetDocument.SetControlValue(prefix + "-clip", std::to_string(runtimeView.animationClip));
    auto* timeline = document->GetElementById(prefix + "-time");
    if (timeline != document->GetContext()->GetFocusElement())
    {
        timeline->SetAttribute("max", SceneActorNumber(runtimeView.animationDuration));
        widgetDocument.SetControlValue(prefix + "-time", SceneActorNumber(runtimeView.animationTime));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshRetargetAnimation(const EditorResourceEditorTab& tab)
{
    const auto values = !tab.loading && tab.snapshot ? tab.snapshot->retargetPreview : nullptr;
    const bool visible = retargetEditor.AnimationVisible() && IsResourceEditorVisible();
    if (visible && values && runtimeActionSink &&
        (requestedRetargetPath != tab.path || requestedRetargetRevision != tab.snapshot->revision ||
            requestedPreviewPath != values->targetModelPath || requestedPreviewBlueprint))
    {
        RequestAssetPreview(values->targetModelPath, false, tab.path, tab.snapshot->revision);
    }
    const uint64_t generation = visible ? CurrentModelPreviewGeneration() : 0;
    const bool animated = generation && runtimeView.animationWorldGeneration == generation &&
        runtimeView.animationJointCount != 0;
    widgetDocument.SetProperty("retarget-preview-image", "display", generation ? "block" : "none");
    widgetDocument.SetProperty("retarget-preview-status", "display", generation ? "none" : "block");
    SetMarkup("retarget-preview-status", Text(values ? "Loading paired model preview..." :
        tab.snapshot && !tab.snapshot->previewDiagnostic.empty() ? tab.snapshot->previewDiagnostic :
        "Animation preview requires preview.targetModelPath"));
    RefreshAnimationControls("retarget-animation", animated);
    std::string assets;
    if (animated)
        for (size_t index = 0; index < runtimeView.animationClips.size(); ++index)
            assets += UiWidgets::ListRow({"animation-browser-clip", index}, runtimeView.animationClips[index],
                index == runtimeView.animationClip, "icons/play.png");
    SetMarkup("retarget-animation-assets", std::move(assets));
    SetDisabled("retarget-animation-frame", !generation);
    if (animated && initializedRetargetGeneration != generation && !runtimeView.animationClips.empty())
    {
        initializedRetargetGeneration = generation;
        UiAction action;
        action.type = UiActionType::ControlAnimationPreview;
        action.previewCamera.worldGeneration = generation;
        action.stringValue = "previewClip";
        action.floatValue = static_cast<float>(runtimeView.animationClip);
        runtimeActionSink->RequestRuntimeAction(std::move(action));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshResourceEditor()
{
    if (document == nullptr) return;
    WidgetRefreshScope refresh(refreshingControls);
    RefreshDocumentTabs();
    const auto* active = resourceEditor.ActiveTab();
    const bool blueprintAsset = active && active->kind == EditorAssetKind::Blueprint;
    const bool retargetAsset = active && active->kind == EditorAssetKind::Retarget;
    widgetDocument.SetClass("resource-editor", "retarget-active", retargetAsset);
    for (const char* id : {"blueprint-asset-editor", "blueprint-asset-save", "blueprint-asset-revert"})
        widgetDocument.SetProperty(id, "display", blueprintAsset ? "block" : "none");
    widgetDocument.SetProperty("retarget-asset-editor", "display", retargetAsset ? "flex" : "none");
    widgetDocument.SetProperty("resource-preview-panel", "display", blueprintAsset || retargetAsset ? "none" : "flex");
    widgetDocument.SetProperty("resource-details-panel", "display", blueprintAsset || retargetAsset ? "none" : "flex");
    if (retargetAsset)
    {
        for (const char* id : {"resource-model-tools", "resource-texture-tools"})
            widgetDocument.SetClass(id, "visible", false);
        SetDisabled("resource-reload", active->loading);
        auto visibleTab = *active;
        if (reloadingRetargetPath == active->path)
        {
            if (active->snapshot && active->snapshot->revision > reloadingRetargetRevision)
                reloadingRetargetPath.clear();
            else visibleTab.loading = true;
        }
        retargetEditor.Build(visibleTab);
        RefreshRetargetAnimation(visibleTab);
        return;
    }
    if (blueprintAsset)
    {
        for (const char* id : {"resource-model-tools", "resource-texture-tools"})
            widgetDocument.SetClass(id, "visible", false);
        SetDisabled("resource-reload", !BlueprintDocument(active->path));
        RefreshBlueprintAssetEditor();
        return;
    }
    const bool isTressfx = active != nullptr && active->kind == EditorAssetKind::Tressfx;
    const bool isMesh = active != nullptr && (active->kind == EditorAssetKind::Mesh || isTressfx);
    // 快照到达后以作者文档为准复核毛发判定（目录可能缺失或过期）；目录只在
    // 打开时提供同步判定，避免一帧内先请求了本不该加载的预览。
    if (isMesh && !active->loading && active->snapshot)
    {
        if (active->snapshot->type == "tressfx")
        {
            hairPreviewPath = active->path;
            hairPreviewBlocked = active->snapshot->tressfxSkinning;
        }
        else if (hairPreviewPath == active->path)
        {
            hairPreviewPath.clear();
            hairPreviewBlocked = false;
        }
    }
    else if (!isMesh || active->path != hairPreviewPath)
    {
        hairPreviewPath.clear();
        hairPreviewBlocked = false;
    }
    const bool isHair = isTressfx && hairPreviewPath == active->path;
    const bool hairBlocked = isHair && hairPreviewBlocked;
    RefreshHairSimulationSection(isHair, hairBlocked);
    const bool previewReady = HasCurrentModelPreview() && !hairBlocked;
    const bool animated = isMesh && previewReady && runtimeView.animationWorldGeneration ==
        CurrentModelPreviewGeneration() && runtimeView.animationJointCount != 0;
    widgetDocument.SetProperty("resource-animation-tools", "display", animated ? "flex" : "none");
    widgetDocument.SetProperty("resource-motion-features", "display", animated ? "block" : "none");
    if (animated)
    {
        RefreshAnimationControls("resource-animation", true);
        SetMarkup("resource-motion-status", Text(runtimeView.animationFeatureStatus));
        // 固定通道结构，只更新数值 span；播放时不重建整表或改变 Details 滚动位置。
        std::string rows;
        for (size_t i = 0; i < runtimeView.animationFeatureChannels.size(); ++i)
            rows += "<div class=\"motion-feature-row\"><span>" + Text(runtimeView.animationFeatureChannels[i]) +
                "</span><span id=\"motion-feature-" + std::to_string(i) + "\"></span></div>";
        SetMarkup("resource-motion-values", rows);
        for (size_t i = 0; i < runtimeView.animationFeatureChannels.size(); ++i)
            SetMarkup("motion-feature-" + std::to_string(i), i < runtimeView.animationFeatures.size()
                ? Text(SceneActorNumber(runtimeView.animationFeatures[i])) : "--");
    }
    const auto* texture = active && !active->loading && active->snapshot
        ? active->snapshot->texturePreview.get() : nullptr;
    const bool textureReady = active && active->kind == EditorAssetKind::Texture && texture;
    widgetDocument.SetProperty("resource-texture-image", "display", textureReady ? "block" : "none");
    auto* textureImage = document->GetElementById("resource-texture-image");
    const std::string textureSource = textureReady ? EditorTexturePreviewSource(active->snapshot->revision) : "";
    if (textureViewSource != textureSource)
    {
        textureViewSource = textureSource;
        ResetTextureView();
    }
    widgetDocument.SetClass("resource-texture-tools", "visible", active && active->kind == EditorAssetKind::Texture);
    SetDisabled("resource-texture-fit", !textureReady);
    if (textureImage->GetAttribute<std::string>("src", "") != textureSource)
        textureImage->SetAttribute("src", textureSource);
    if (textureReady)
    {
        // 按面板可用区域等比例取景，不能沿用场景图像的强制拉伸布局。
        const auto size = document->GetElementById("resource-preview-surface")->GetBox().GetSize(Rml::BoxArea::Padding);
        const float scale = std::min(std::max(1.0f, size.x - 16) / texture->width,
            std::max(1.0f, size.y - 16) / texture->height);
        const float width = texture->width * scale * textureZoom, height = texture->height * scale * textureZoom;
        widgetDocument.SetProperty("resource-texture-image", "width", std::to_string(width) + "px");
        widgetDocument.SetProperty("resource-texture-image", "height", std::to_string(height) + "px");
        widgetDocument.SetProperty("resource-texture-image", "left", std::to_string((size.x - width) * 0.5f + texturePan.x) + "px");
        widgetDocument.SetProperty("resource-texture-image", "top", std::to_string((size.y - height) * 0.5f + texturePan.y) + "px");
    }
    SetDisabled("resource-frame", !previewReady || !isMesh);
    widgetDocument.SetProperty("resource-preview-image", "display", previewReady ? "block" : "none");
    for (const char* id : {"resource-preview-icon", "resource-preview-status", "resource-preview-path"})
        widgetDocument.SetProperty(id, "display", previewReady || textureReady ? "none" : "block");
    // 毛发工具只在可预览的 tressfx 资产上出现；挡预览时隐藏复位按钮，
    // 让“没有视口”和“有视口但没东西可复位”在 UI 上不混淆。
    widgetDocument.SetProperty("resource-hair-tools", "display", isHair && !hairBlocked ? "flex" : "none");
    const auto* hairDocument = isHair ? TressfxDocument(active->path) : nullptr;
    SetDisabled("resource-hair-reset", !previewReady || !hairDocument ||
        !TressfxGpuSimulationDefaults(hairDocument->document));
    widgetDocument.SetClass("resource-model-tools", "visible", isMesh);
    widgetDocument.SetClass("resource-model-info", "visible", isMesh);
    for (const char* id : {"resource-tool-select", "resource-tool-move", "resource-tool-rotate", "resource-tool-scale",
                           "resource-space-world", "resource-space-local"})
        SetDisabled(id, !isMesh);
    if (active == nullptr)
    {
        SetDisabled("resource-reload", true);
        SetMarkup("resource-preview-title", "Asset Editor");
        SetMarkup("resource-preview-kind", "");
        SetMarkup("resource-preview-status", "Open a Mesh or Texture from Content Browser.");
        SetMarkup("resource-preview-path", "");
        auto* previewIcon = document->GetElementById("resource-preview-icon");
        if (previewIcon->GetAttribute<std::string>("src", "") != "icons/mesh.png")
            previewIcon->SetAttribute("src", "icons/mesh.png");
        resourceJsonInspector.Build(nullptr, {}, false);
        return;
    }

    SetDisabled("resource-reload", active->loading);
    SetMarkup("resource-preview-title", Text(active->snapshot != nullptr &&
        !active->snapshot->name.empty() ? active->snapshot->name :
        std::filesystem::path(active->path).filename().string()));
    SetMarkup("resource-preview-kind", Text(EditorAssetKindName(active->kind)));
    if (animated) SetMarkup("resource-preview-kind", "Skeletal Mesh");
    if (textureReady)
        SetMarkup("resource-preview-kind", Text(std::to_string(texture->sourceWidth) + " x " +
            std::to_string(texture->sourceHeight) + " | RGB | " + active->snapshot->textureColorSpace));
    SetMarkup("resource-preview-path", Text(active->path));
    SetMarkup("resource-preview-status", active->loading
        ? (active->kind == EditorAssetKind::Texture ? "Loading texture preview..." : "Reading JSON metadata...")
        : hairBlocked ? Text(std::string(TressfxHairSkinnedAssetMessage))
        : isMesh ? (isHair ? "Waiting for hair preview" : "Waiting for model preview")
        : active->snapshot && !active->snapshot->previewDiagnostic.empty()
            ? Text(active->snapshot->previewDiagnostic)
            : "Preview unavailable: referenced data is not loaded.");
    auto* previewIcon = document->GetElementById("resource-preview-icon");
    const std::string icon = EditorAssetKindIcon(active->kind);
    if (previewIcon->GetAttribute<std::string>("src", "") != icon)
        previewIcon->SetAttribute("src", icon);
    resourceJsonInspector.Build(active->snapshot, active->path, active->loading);
    if (isMesh)
    {
        for (const auto& pair : {std::pair<const char*, const char*>("resource-tool-select", "select"),
                                 {"resource-tool-move", "move"}, {"resource-tool-rotate", "rotate"},
                                 {"resource-tool-scale", "scale"}})
            widgetDocument.SetClass(pair.first, "active", resourceTransformTool == pair.second);
        widgetDocument.SetClass("resource-space-world", "active", resourceTransformSpace == "world");
        widgetDocument.SetClass("resource-space-local", "active", resourceTransformSpace == "local");
    }
    if (isMesh)
    {
        const std::string name = active->snapshot != nullptr && !active->snapshot->name.empty()
            ? active->snapshot->name : std::filesystem::path(active->path).filename().string();
        SetMarkup("resource-model-name", Text("Name: " + name));
        SetMarkup("resource-model-path", Text("Path: " + active->path));
        SetMarkup("resource-model-status", Text("Status: " +
            std::string(active->loading ? "Reading metadata" :
                active->snapshot != nullptr && active->snapshot->diagnostic.empty() ? "Metadata ready" : "Diagnostic")));
        SetMarkup("resource-model-fields", Text("Metadata fields: " + std::to_string(
            active->snapshot != nullptr ? active->snapshot->fields.size() : 0)));
        SetMarkup("resource-model-description", Text(active->snapshot != nullptr
            ? active->snapshot->description : std::string{}));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshControls()
{
    // File/Edit 菜单项与工具栏按钮共用同一份文档命令可用性。
    for (const char* id : DocumentCommandIds)
        SetDisabled(id, !snapshot.activeDocument);
    SetMarkup("editor-notice", Text(!docking.Notice().empty() ? docking.Notice() :
        localStatus.empty() ? snapshot.statusMessage : localStatus));
    if (!snapshot.activeDocument)
    {
        SetMarkup("document-title", "Material Instance");
        SetMarkup("document-status", "");
        SetDisabled("apply-preview", true);
        SetDisabled("restore-preview", true);
        return;
    }
    const auto& asset = *snapshot.activeDocument;
    SetMarkup("document-title", Text(std::filesystem::path(asset.assetPath).filename().string()));
    SetMarkup("document-status", Text(std::string(DocumentStatus(asset.status)) +
        " | Preview: " + PreviewStatus(asset.preview) + " | " + asset.validationMessage +
        (asset.previewMessage.empty() ? "" : " | " + asset.previewMessage)));
    const bool connected = asset.preview == EditorPreviewStatus::Connected;
    SetDisabled("apply-preview", !connected);
    SetDisabled("restore-preview", !connected);
    for (size_t index = 0; index < asset.renderStates.size(); ++index)
    {
        SetControlValue("state-" + std::to_string(index), asset.renderStates[index].effectiveValue);
        SetDisabled("reset-state-" + std::to_string(index), !asset.renderStates[index].overrideValue);
    }
    for (size_t index = 0; index < asset.parameters.size(); ++index)
    {
        const auto& parameter = asset.parameters[index];
        for (uint32_t component = 0; component < parameter.effectiveValue.ComponentCount(); ++component)
            SetControlValue("parameter-" + std::to_string(index) + "-" + std::to_string(component),
                Number(parameter.effectiveValue.values[component]));
        // 色块与十六进制读数逐帧跟随有效线性值；两者都走既有缓存，静止帧不重复写。
        if (parameter.type == EditorParameterType::Color)
        {
            const auto readout = UiWidgets::MakeColorReadout(parameter.effectiveValue.values);
            widgetDocument.SetProperty("color-swatch-" + std::to_string(index), "background-color", readout.rgb);
            SetMarkup("color-hex-" + std::to_string(index), Text(readout.hex));
        }
        SetDisabled("reset-parameter-" + std::to_string(index), !parameter.overrideValue);
    }
    for (size_t index = 0; index < asset.textures.size(); ++index)
    {
        const auto& texture = asset.textures[index];
        // 目录与有效绑定未变时，连选项字符串也不生成；不能只在写 DOM 时才判相等。
        if (textureOptionsDirty || index >= textureOptionSelections.size() ||
            textureOptionSelections[index] != texture.effectiveAssetPath)
        {
            std::string options = Option(texture.effectiveAssetPath, texture.effectiveAssetPath);
            for (const auto& candidate : snapshot.textureAssets)
            {
                if (candidate.valid && candidate.assetPath != texture.effectiveAssetPath)
                    options += Option(candidate.assetPath, candidate.assetPath);
            }
            SetMarkup("texture-" + std::to_string(index), std::move(options));
        }
        SetControlValue("texture-" + std::to_string(index), asset.textures[index].effectiveAssetPath);
        SetDisabled("reset-texture-" + std::to_string(index), !asset.textures[index].overrideAssetPath);
    }
    if (textureOptionsDirty || textureOptionSelections.size() != asset.textures.size())
        textureOptionSelections.resize(asset.textures.size());
    for (size_t index = 0; index < asset.textures.size(); ++index)
        textureOptionSelections[index] = asset.textures[index].effectiveAssetPath;
    textureOptionsDirty = false;
}

void MaterialInstanceAssetEditorPanel::RefreshScriptControls()
{
    if (document == nullptr) return;
    const auto& scripts = runtimeView.scripts;
    const bool level = !runtimeView.offlineMode && !IsAssetEditorVisible();
    const bool enabled = scripts.available && level;
    bool hasActors = false;
    for (const auto& actor : OutlinerActors())
        if (actor.type == EditorSceneActorType::Script) hasActors = true;
    SetDisabled("script-play", !enabled || !hasActors || scripts.state == ScriptPlayState::Playing);
    SetDisabled("script-pause", !enabled || scripts.state != ScriptPlayState::Playing);
    SetDisabled("script-stop", !enabled || scripts.state == ScriptPlayState::Editing);
    SetDisabled("script-reload", !enabled);
    SetDisabled("script-compile", !enabled || !scripts.buildAvailable || scripts.buildBusy);
    SetDisabled("script-time-scale", !enabled);
    SetControlValue("script-time-scale", Number(scripts.timeScale));
    SetDisabled("menu-script-compile", !enabled || !scripts.buildAvailable || scripts.buildBusy);
    SetDisabled("menu-script-auto", !enabled || !scripts.buildAvailable);
    widgetDocument.SetClass("menu-script-auto", "checked", scripts.autoCompile);
    const bool canAuthor = scripts.available && scripts.state == ScriptPlayState::Editing;
    SetDisabled("script-add", !canAuthor || (!scriptPlacementCreateAsset && !level) || scripts.classes.empty());
    SetDisabled("script-placement-mode", !canAuthor);
    widgetDocument.SetProperty("script-placement-mesh-row", "display", scriptPlacementCreateAsset ? "none" : "flex");
    widgetDocument.SetProperty("script-new-path-row", "display", scriptPlacementCreateAsset ? "flex" : "none");
    if (auto* button = document->GetElementById("script-add"))
    {
        const std::string title = scriptPlacementCreateAsset ?
            "Create Blueprint Asset" : "Place C# instance";
        if (button->GetAttribute<std::string>("title", "") != title) button->SetAttribute("title", title);
    }
    SetDisabled("script-placement-toggle", !canAuthor);
    SetDisabled("menu-edit-undo", !level || scripts.state != ScriptPlayState::Editing || !scripts.canUndo);
    SetDisabled("menu-edit-redo", !level || scripts.state != ScriptPlayState::Editing || !scripts.canRedo);
    RefreshBlueprintHistoryControls();
    widgetDocument.SetProperty("script-placement", "display",
        scripts.available && scriptPlacementVisible ? "block" : "none");
    std::string options;
    for (const auto& type : scripts.classes)
        options += Option(type.at("name").get<std::string>(), type.at("name").get<std::string>());
    SetMarkup("script-class", std::move(options));
    SetMarkup("script-feedback", Text(scripts.feedback));
    widgetDocument.SetProperty("script-feedback", "display", scripts.feedback.empty() ? "none" : "block");
}

void MaterialInstanceAssetEditorPanel::UpdatePerformance(const PerformanceSnapshot& performance)
{
    // 面板自己按 5 Hz 取样；这里只传冻结快照、可见性与同一份 UI 单调时钟。
    performancePanel.Build(performance, docking.IsPanelActive("performance"),
        Rml::GetSystemInterface()->GetElapsedTime());
}

void MaterialInstanceAssetEditorPanel::UpdateScriptPerformance(
    const nlohmann::json& scriptPerformance)
{
    // Log 的 Scripts 页展示脚本时间/计数器；它是时间序列，随性能分节采样，
    // 不通过事件驱动的脚本分节刷新。
    logPanel.BuildPerformance(scriptPerformance, docking.IsPanelActive("log"));
}

void MaterialInstanceAssetEditorPanel::UpdateRuntimeView(const UiViewModelSnapshot& view)
{
    if (view.scripts.worldGeneration != runtimeView.scripts.worldGeneration ||
        view.scripts.state != ScriptPlayState::Editing || view.offlineMode)
        actorDragPath.clear();
    const bool refreshOffline = !runtimeControlsCurrent || !SameOfflineControls(runtimeView, view);
    runtimeView = view;
    WidgetRefreshScope refresh(refreshingControls);
    const bool motionEnabled = view.motionDebug && view.motionDebug->enabled;
    auto* motionToggle = document ? document->GetElementById("viewport-motion-debug") : nullptr;
    if (motionToggle && motionToggle->HasAttribute("checked") != motionEnabled)
    {
        if (motionEnabled) motionToggle->SetAttribute("checked", "");
        else motionToggle->RemoveAttribute("checked");
    }
    SetDisabled("viewport-motion-debug", !view.motionWorldGeneration || view.offlineMode);
    const bool matching = motionEnabled && !view.motionDebug->databaseIdentity.empty();
    const std::string motionStatus = matching ? "MM #" + std::to_string(view.motionDebug->searchCount) +
        " | " + view.motionDebug->decisionReason +
        " | Cyan: actual | Amber: desired/no-collision | Red: query" :
        motionEnabled ? view.motionDebug->summary : "";
    SetMarkup("viewport-motion-status", Text(motionStatus));
    widgetDocument.SetClass("viewport-motion-status", "motion-matching-summary", matching);
    widgetDocument.SetProperty("viewport-motion-status", "display",
        motionEnabled && !view.offlineMode ? "block" : "none");
    RefreshMotionMatching();
    const bool debugAvailable = view.debugDraw && view.motionWorldGeneration && !view.offlineMode;
    SetDisabled("viewport-debug-draw", !debugAvailable);
    if (!debugAvailable && openMenu == "debug-draw") CloseMenus();
    if (debugAvailable)
    {
        const auto& debug = *view.debugDraw;
        auto* toggle = document->GetElementById("debug-draw-enabled");
        if (toggle->HasAttribute("checked") != debug.enabled)
        {
            if (debug.enabled) toggle->SetAttribute("checked", ""); else toggle->RemoveAttribute("checked");
        }
        std::string categories;
        for (size_t i = 0; i < debug.categories.size(); ++i)
        {
            const auto& category = debug.categories[i];
            categories += "<label class=\"debug-draw-check\"><input type=\"checkbox\" data-action=\"debug-draw-category\" data-index=\"" +
                std::to_string(i) + "\"" + (category.enabled ? " checked=\"checked\"" : "") + "/><span>" +
                Text(category.name) + "</span></label>";
        }
        SetMarkup("debug-draw-categories", std::move(categories));
        std::string owners = Option("", "All Owners");
        for (const auto& owner : debug.owners)
            if (!owner.empty()) owners += Option(owner, owner);
        if (!debug.ownerFilter.empty() &&
            std::find(debug.owners.begin(), debug.owners.end(), debug.ownerFilter) == debug.owners.end())
            owners += Option(debug.ownerFilter, debug.ownerFilter);
        SetMarkup("debug-draw-owner", std::move(owners));
        SetControlValue("debug-draw-owner", debug.ownerFilter);
        SetMarkup("debug-draw-count", std::to_string(debug.visibleCount) + " / " + std::to_string(debug.storedCount));
    }
    if (const auto* tab = resourceEditor.ActiveTab(); tab && tab->kind == EditorAssetKind::Blueprint)
        RefreshBlueprintAssetEditor();
    // 相机和统计变化不触碰离线表单；视口工具条继续独立更新。
    RefreshViewportToolbar();
    RefreshScriptControls();
    contentBrowser.Build(snapshot, runtimeView.scripts.classes);
    if (!refreshOffline || document == nullptr) return;
    SetControlValue("offline-samples", std::to_string(view.offlineSamples));
    SetControlValue("offline-bounces", std::to_string(view.offlineBounces));
    SetControlValue("offline-lightSamples", std::to_string(view.offlineLightSamples));
    SetControlValue("offline-seed", std::to_string(view.offlineSeed));
    SetControlValue("offline-aov", std::to_string(view.offlineAov));
    SetControlValue("offline-exposure", Number(view.offlineExposure));
    SetMarkup("offline-status", std::to_string(view.offlineCompletedSamples) + " / " +
        std::to_string(view.offlineSamples) + " spp");
    SetMarkup("offline-notice", Text(view.offlineUiNotice.empty()
        ? view.offlineError : view.offlineUiNotice));
    const auto state = static_cast<Offline::SessionState>(view.offlineState);
    const bool active = view.offlineMode && (state == Offline::SessionState::Preparing ||
        state == Offline::SessionState::Rendering);
    SetDisabled("offline-pause", !active);
    SetDisabled("offline-resume", !view.offlineMode || state != Offline::SessionState::Paused);
    SetDisabled("offline-stop", !active && (!view.offlineMode || state != Offline::SessionState::Paused));
    SetDisabled("offline-restart", !view.offlineMode);
    SetDisabled("offline-export", !view.offlineMode || view.offlineCompletedSamples == 0);
    widgetDocument.SetClass("mode-realtime", "selected", !view.offlineMode);
    widgetDocument.SetClass("mode-offline", "selected", view.offlineMode);
    const auto* focus = document->GetContext()->GetFocusElement();
    // 聚焦输入保留用户的半成品；失焦后的第一帧仍需用权威快照校正被拒绝的值。
    runtimeControlsCurrent = focus == nullptr || focus->GetId().find("offline-") != 0;
}

void MaterialInstanceAssetEditorPanel::RefreshMotionMatchingCandidate(
    size_t slot, const Animation::MotionCandidate* candidate)
{
    const auto prefix = "motion-candidate-" + std::to_string(slot);
    SetMarkup(prefix + "-clip", candidate ? Text(candidate->clipId) : "-");
    SetMarkup(prefix + "-time", candidate ? MotionNumber(candidate->time) + "s" : "-");
    SetMarkup(prefix + "-trajectory", candidate ? MotionNumber(candidate->trajectoryCost) : "-");
    SetMarkup(prefix + "-pose", candidate ? MotionNumber(candidate->poseCost) : "-");
    SetMarkup(prefix + "-policy", candidate ? MotionNumber(candidate->policyCost) : "-");
    SetMarkup(prefix + "-total", candidate ? MotionNumber(candidate->totalCost) : "-");
    std::string status;
    if (candidate)
    {
        status = candidate->eligible ? (candidate->continuing ? "continuing" : "eligible") :
            "rejected: " + candidate->rejection;
        if (candidate->quantizationSeconds > 0)
            status += " | quantized " + MotionNumber(candidate->quantizationSeconds) + "s";
    }
    SetMarkup(prefix + "-reason", Text(status));
    widgetDocument.SetClass(prefix, "motion-rejected", candidate && !candidate->eligible);
    auto* row = document->GetElementById(prefix);
    const std::string title = candidate ? candidate->clipId + " @ " + MotionNumber(candidate->time) +
        "s | " + status : "";
    if (row && row->GetAttribute<std::string>("title", "") != title)
        row->SetAttribute("title", title);
    SetMarkup(prefix + "-detail", Text(title));
}

void MaterialInstanceAssetEditorPanel::RefreshMotionMatchingQuery()
{
    const auto& debug = *runtimeView.motionDebug;
    auto channels = debug.channels;
    if (channels.empty())
        for (size_t channel = 0; channel < debug.features.size(); ++channel)
            channels.push_back("[" + std::to_string(channel) + "]");
    if (channels != motionMatchingQueryChannels)
    {
        std::string rows;
        for (size_t channel = 0; channel < channels.size(); ++channel)
        {
            const auto prefix = "motion-query-channel-" + std::to_string(channel);
            rows += "<div id=\"" + prefix + "\" class=\"motion-query-channel\">"
                "<div class=\"motion-query-channel-name\">" + Text(channels[channel]) + "</div>"
                "<div class=\"motion-query-channel-values\"><span id=\"" + prefix +
                "-raw\"></span><span id=\"" + prefix + "-normalized\"></span></div></div>";
        }
        SetMarkup("motion-query-channels", std::move(rows));
        motionMatchingQueryChannels = std::move(channels);
    }
    // RmlUi 不会显示 HTML title；完整身份与查询有实际可展开的固定节点，暂停只读缓存值。
    SetMarkup("motion-query-database", Text("DB " + debug.databaseIdentity));
    SetMarkup("motion-query-payload", Text("Statistics " + debug.payloadIdentity));
    SetMarkup("motion-query-component", Text("Component " + debug.componentIdentity));
    for (size_t channel = 0; channel < motionMatchingQueryChannels.size(); ++channel)
    {
        const auto prefix = "motion-query-channel-" + std::to_string(channel);
        SetMarkup(prefix + "-raw", "Raw " + (channel < debug.features.size() ?
            MotionNumber(debug.features[channel]) : "-"));
        SetMarkup(prefix + "-normalized", "N " + (channel < debug.normalizedQuery.size() ?
            MotionNumber(debug.normalizedQuery[channel]) : "-"));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshMotionMatching()
{
    if (!document) return;
    const auto& debug = runtimeView.motionDebug;
    const bool visible = debug && debug->enabled && !debug->databaseIdentity.empty() &&
        runtimeView.motionWorldGeneration && !runtimeView.offlineMode;
    widgetDocument.SetProperty("viewport-motion-matching", "display", visible ? "block" : "none");
    if (!visible)
    {
        motionMatchingVisible = false;
        return;
    }
    if (!motionMatchingRowsMounted)
    {
        std::string rows;
        for (size_t slot = 0; slot < 7; ++slot)
        {
            const auto prefix = "motion-candidate-" + std::to_string(slot);
            const auto label = slot == 0 ? "Selected" : slot == 1 ? "Continue" :
                "#" + std::to_string(slot - 1);
            rows += "<div id=\"" + prefix + "\" class=\"motion-candidate\">"
                "<div class=\"motion-candidate-heading\"><span class=\"motion-candidate-kind\">" +
                label + "</span><span id=\"" + prefix + "-clip\" class=\"motion-candidate-clip\"></span>"
                "<span id=\"" + prefix + "-time\" class=\"motion-candidate-time\"></span>"
                "<span id=\"" + prefix + "-reason\" class=\"motion-candidate-reason\"></span></div>"
                "<div class=\"motion-candidate-costs\"><span id=\"" + prefix + "-trajectory\"></span>"
                "<span id=\"" + prefix + "-pose\"></span><span id=\"" + prefix + "-policy\"></span>"
                "<span id=\"" + prefix + "-total\"></span></div>"
                "<span id=\"" + prefix + "-detail\" class=\"motion-candidate-detail\"></span></div>";
        }
        SetMarkup("motion-matching-rows", std::move(rows));
        motionMatchingRowsMounted = true;
    }
    // 候选槽位只挂载一次。运行中的数值至多 10 Hz 刷新，决策/代际变化立即显示；
    // 暂停的相同快照经 WidgetDocument 值缓存后不再写 DOM。
    const std::string refreshKey = debug->componentIdentity + "|" + debug->databaseIdentity + "|" +
        debug->payloadIdentity + "|" + debug->decisionReason + "|" + std::to_string(debug->searchCount) +
        (debug->queryReady ? "|ready" : "|warming") + "|" +
        (debug->queryUsesAchievedVelocity ? "blocked|" : "desired|") +
        debug->playbackClipId + "|" + std::to_string(debug->playbackTime) + "|" +
        std::to_string(debug->playbackDuration) + "|" + (debug->playbackPlaying ? "play|" : "pause|") +
        (debug->replayActive ? "replay|" : "live|") + std::to_string(debug->replayFrame) + "|" +
        std::to_string(debug->replayFrames) + "|" +
        std::to_string(static_cast<int>(runtimeView.scripts.state));
    const double now = Rml::GetSystemInterface()->GetElapsedTime();
    if (motionMatchingVisible && refreshKey == motionMatchingRefreshKey &&
        now - motionMatchingRefreshTime < 0.1) return;
    motionMatchingVisible = true;
    motionMatchingRefreshTime = now;
    motionMatchingRefreshKey = refreshKey;
    SetMarkup("motion-matching-title", Text("Motion Matching | #" + std::to_string(debug->searchCount) +
        " | DB " + debug->databaseIdentity.substr(0, 12)));
    SetMarkup("motion-matching-database", Text("DB " + debug->databaseIdentity.substr(0, 12) +
        " | stats " + debug->payloadIdentity.substr(0, 12)));
    SetMarkup("motion-matching-query", Text("Query " + std::to_string(debug->features.size()) +
        "D " + (debug->queryReady ? "ready" : "warming") +
        (debug->queryUsesAchievedVelocity ? " blocked" : "") + " | raw " +
        MotionQueryReadout(debug->features) + " | N " + MotionQueryReadout(debug->normalizedQuery)));
    const std::string databaseTitle = "DB " + debug->databaseIdentity + "\nStatistics " +
        debug->payloadIdentity + "\nComponent " + debug->componentIdentity;
    auto* databaseLabel = document->GetElementById("motion-matching-database");
    if (databaseLabel->GetAttribute<std::string>("title", "") != databaseTitle)
        databaseLabel->SetAttribute("title", databaseTitle);
    auto* matchingTitle = document->GetElementById("motion-matching-title");
    if (matchingTitle->GetAttribute<std::string>("title", "") != databaseTitle)
        matchingTitle->SetAttribute("title", databaseTitle);
    std::string queryTitle = "Search " + std::to_string(debug->searchCount) + " | raw / normalized";
    for (size_t channel = 0; channel < debug->features.size(); ++channel)
    {
        queryTitle += "\n" + (channel < debug->channels.size() ? debug->channels[channel] :
            "[" + std::to_string(channel) + "]") + ": " + MotionNumber(debug->features[channel]) + " / " +
            (channel < debug->normalizedQuery.size() ? MotionNumber(debug->normalizedQuery[channel]) : "-");
    }
    auto* queryLabel = document->GetElementById("motion-matching-query");
    if (queryLabel->GetAttribute<std::string>("title", "") != queryTitle)
        queryLabel->SetAttribute("title", queryTitle);
    SetMarkup("motion-matching-decision", Text(debug->decisionReason + " | allowed " +
        std::to_string(debug->allowedFrameCount) + " | rejected " +
        std::to_string(debug->policyRejectedCount) + " | blend " + MotionNumber(debug->blendAlpha)));
    // 时间轴只读原生播放器；拖动期间不覆盖焦点控件，暂停后 seek 保持暂停。
    const bool hasPlayback = debug->playbackDuration > 0;
    SetDisabled("motion-timeline-play", !hasPlayback || debug->playbackPlaying);
    SetDisabled("motion-timeline-pause", !hasPlayback || !debug->playbackPlaying);
    SetDisabled("motion-timeline-seek", !hasPlayback);
    SetMarkup("motion-timeline-status", Text(MotionNumber(debug->playbackTime) + " / " +
        MotionNumber(debug->playbackDuration) + " s" + (debug->playbackPlaying ? " >" : " ||")));
    auto* motionTimeline = document->GetElementById("motion-timeline-seek");
    if (motionTimeline && motionTimeline != document->GetContext()->GetFocusElement())
    {
        motionTimeline->SetAttribute("max", MotionNumber(debug->playbackDuration));
        widgetDocument.SetControlValue("motion-timeline-seek", MotionNumber(debug->playbackTime));
    }
    auto* timelineStatus = document->GetElementById("motion-timeline-status");
    const std::string timelineTitle = debug->playbackClipId + "\n" +
        (debug->playbackPlaying ? "playing" : "paused");
    if (timelineStatus && timelineStatus->GetAttribute<std::string>("title", "") != timelineTitle)
        timelineStatus->SetAttribute("title", timelineTitle);
    // 查询回放：时间轴只选择历史快照；回放时 World 会暂停被聚焦播放器。
    const bool hasReplay = debug->replayFrames > 0;
    SetDisabled("motion-replay", !hasReplay);
    SetDisabled("motion-replay-seek", !hasReplay);
    auto* replayToggle = document->GetElementById("motion-replay");
    if (replayToggle && replayToggle->HasAttribute("checked") != debug->replayActive)
    {
        if (debug->replayActive) replayToggle->SetAttribute("checked", "");
        else replayToggle->RemoveAttribute("checked");
    }
    auto* replaySeek = document->GetElementById("motion-replay-seek");
    if (replaySeek && replaySeek != document->GetContext()->GetFocusElement())
    {
        replaySeek->SetAttribute("max", std::to_string(std::max(1, debug->replayFrames - 1)));
        widgetDocument.SetControlValue("motion-replay-seek", std::to_string(debug->replayFrame));
    }
    SetMarkup("motion-replay-status", Text(debug->replayActive ?
        "replay " + std::to_string(debug->replayFrame) + "/" + std::to_string(debug->replayFrames) :
        hasReplay ? "live " + std::to_string(debug->replayFrame) + "/" + std::to_string(debug->replayFrames) :
        "no history"));
    RefreshMotionMatchingQuery();
    RefreshMotionMatchingCandidate(0, debug->selected ? &*debug->selected : nullptr);
    RefreshMotionMatchingCandidate(1, debug->continuation ? &*debug->continuation : nullptr);
    for (size_t rank = 0; rank < 5; ++rank)
        RefreshMotionMatchingCandidate(rank + 2, rank < debug->topCandidates.size() ?
            &debug->topCandidates[rank] : nullptr);
}

void MaterialInstanceAssetEditorPanel::CloseDocument(
    const std::string& path, EditorDirtyDocumentPolicy policy)
{
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
    if (action == "motion-timeline-play" || action == "motion-timeline-pause")
    {
        if (runtimeActionSink && runtimeView.motionDebug && runtimeView.motionWorldGeneration &&
            !runtimeView.offlineMode && runtimeView.motionDebug->playbackDuration > 0)
        {
            if ((action == "motion-timeline-play" && runtimeView.motionDebug->playbackPlaying) ||
                (action == "motion-timeline-pause" && !runtimeView.motionDebug->playbackPlaying)) return;
            UiAction request;
            request.type = UiActionType::ControlScriptAnimation;
            request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
            request.stringValue = runtimeView.motionDebug->componentIdentity;
            request.intValue = 1;
            request.floatValue = action == "motion-timeline-play" ? 1.0f : 0.0f;
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "motion-replay")
    {
        if (runtimeActionSink && runtimeView.motionDebug && runtimeView.motionWorldGeneration &&
            !runtimeView.offlineMode && runtimeView.motionDebug->replayFrames > 0)
        {
            UiAction request;
            request.type = UiActionType::ControlMotionReplay;
            request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
            request.intValue = element.HasAttribute("checked") ?
                std::max(0, runtimeView.motionDebug->replayFrames - 1) : -1;
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    if (action == "motion-query-toggle")
    {
        motionMatchingQueryExpanded = !motionMatchingQueryExpanded;
        widgetDocument.SetClass("viewport-motion-matching", "query-expanded", motionMatchingQueryExpanded);
        widgetDocument.SetClass("motion-query-toggle", "active", motionMatchingQueryExpanded);
    }
    else if (action == "debug-draw-clear")
    {
        if (runtimeActionSink && runtimeView.debugDraw && runtimeView.motionWorldGeneration && !runtimeView.offlineMode)
        {
            UiAction request; request.type = UiActionType::ControlDebugDraw;
            request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
            request.debugDrawControl.action = Debug::ControlAction::ClearAll;
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
    }
    else if (action.rfind("blueprint-asset-", 0) == 0) HandleBlueprintAssetEvent(element, action, true);
    else if (HandleSceneLogicEvent(element, action, true)) {}
    else if (HandleSceneCompositePropertyEvent(element, action, true)) {}
    else if (action == "script-physics-constraint-add" || action == "script-physics-constraint-remove")
    {
        const auto* actor = SelectedActor();
        if (!actor || !actor->scriptBlueprintPath.empty()) return;
        if (action == "script-physics-constraint-add")
        {
            auto* target = document->GetElementById("script-physics-constraint-target");
            const auto value = target ? ControlValue(*target) : "";
            if (value.empty()) { localStatus = "A target Blueprint ID is required."; return; }
            RequestScriptEdit(UiActionType::SetScriptPhysics, {{"field", "constraint-add"}, {"value", value}});
        }
        else RequestScriptEdit(UiActionType::SetScriptPhysics,
            {{"field", "constraint-remove"}, {"value", index}});
    }
    else if (action == "scene-undo" || action == "scene-redo")
    {
        if (RequestBlueprintHistory(action == "scene-redo")) return;
        if (!runtimeActionSink || runtimeView.scripts.state != ScriptPlayState::Editing || runtimeView.offlineMode ||
            IsAssetEditorVisible() || !(action == "scene-redo" ? runtimeView.scripts.canRedo : runtimeView.scripts.canUndo))
            return;
        UiAction request;
        request.type = action == "scene-redo" ? UiActionType::RedoSceneEdit : UiActionType::UndoSceneEdit;
        request.actorProperty.worldGeneration = runtimeView.scripts.worldGeneration;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-blueprint-duplicate") RequestScriptEdit(UiActionType::DuplicateScriptBlueprint);
    else if (action == "script-blueprint-delete") RequestScriptEdit(UiActionType::DeleteScriptBlueprint);
    else if (action == "script-component-add")
    {
        const auto kind = ControlValue(*document->GetElementById("script-component-kind"));
        nlohmann::json node = NewScriptComponent(kind);
        if (kind == "mesh") node["modelPath"] = ControlValue(*document->GetElementById("script-component-mesh"));
        RequestScriptEdit(UiActionType::AddScriptComponent, {{"node", node}});
    }
    else if (action == "script-component-remove")
    {
        const auto* actor = SelectedActor();
        if (actor && index < actor->scriptComponents.size())
            RequestScriptEdit(UiActionType::RemoveScriptComponent,
                {{"component", actor->scriptComponents[index].value("id", "root")}});
    }
    else if (action == "script-component-reset")
    {
        const auto* actor = SelectedActor();
        if (!actor || index >= actor->scriptComponents.size() || actor->scriptBlueprintPath.empty()) return;
        const auto component = ScriptComponentId(actor->scriptComponents[index]);
        const auto field = element.GetAttribute<std::string>("data-field", "");
        if (!ScriptComponentAllowsInstance(actor->scriptComponentInstanceEditable, component, field) ||
            !actor->scriptComponentOverrides.contains(component) ||
            !actor->scriptComponentOverrides.at(component).contains(field)) return;
        RequestScriptEdit(UiActionType::SetScriptComponent,
            {{"component", component}, {"field", field}, {"reset", true}});
    }
    else if (action == "script-placement-toggle")
    {
        scriptPlacementVisible = !scriptPlacementVisible;
        RefreshScriptControls();
    }
    else if ((action == "script-compile" || action == "script-auto") && runtimeActionSink)
    {
        if (!runtimeView.scripts.available || !runtimeView.scripts.buildAvailable || runtimeView.offlineMode ||
            IsAssetEditorVisible() || HasModal()) return;
        if (action == "script-compile" && runtimeView.scripts.buildBusy) return;
        UiAction request;
        request.type = action == "script-compile" ? UiActionType::CompileScripts : UiActionType::SetAutoCompileScripts;
        request.intValue = runtimeView.scripts.autoCompile ? 0 : 1;
        request.actorProperty.worldGeneration = runtimeView.scripts.worldGeneration;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if ((action == "script-control" || action == "script-reload") && runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = action == "script-reload" ? UiActionType::ReloadScripts : UiActionType::ControlScripts;
        request.intValue = element.GetAttribute<int>("data-value", 0);
        request.actorProperty.worldGeneration = runtimeView.scripts.worldGeneration;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-add" && runtimeActionSink != nullptr)
    {
        if (!runtimeView.scripts.available || runtimeView.scripts.state != ScriptPlayState::Editing ||
            (!scriptPlacementCreateAsset && (runtimeView.offlineMode || IsAssetEditorVisible())) ||
            runtimeView.scripts.classes.empty() || HasModal()) return;
        UiAction request;
        request.actorProperty.worldGeneration = runtimeView.scripts.worldGeneration;
        if (scriptPlacementCreateAsset)
        {
            request.type = UiActionType::CreateScriptBlueprintAsset;
            request.stringValue = ControlValue(*document->GetElementById("script-new-path"));
            request.script = {{"class", ControlValue(*document->GetElementById("script-class"))},
                {"name", ControlValue(*document->GetElementById("script-name"))}};
        }
        else
        {
            if (runtimeView.scripts.classes.empty()) return;
            request.type = UiActionType::AddScriptBlueprint;
            request.script = {{"class", ControlValue(*document->GetElementById("script-class"))},
                {"name", ControlValue(*document->GetElementById("script-name"))},
                {"modelPath", ControlValue(*document->GetElementById("script-mesh"))}};
        }
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if ((action == "script-reset" || action == "script-asset-create") && runtimeActionSink != nullptr)
    {
        const auto* actor = SelectedActor();
        if (!actor || actor->type != EditorSceneActorType::Script ||
            runtimeView.scripts.state != ScriptPlayState::Editing) return;
        UiAction request;
        request.actorProperty.worldGeneration = actor->worldGeneration;
        request.actorProperty.identity = actor->identity;
        if (action == "script-reset")
        {
            if (index >= actor->scriptProperties.size()) return;
            auto property = actor->scriptProperties.begin();
            std::advance(property, index);
            if (std::find(actor->scriptInstanceEditable.begin(), actor->scriptInstanceEditable.end(),
                property.key()) == actor->scriptInstanceEditable.end()) return;
            if (!actor->scriptOverrides.contains(property.key())) return;
            request.type = UiActionType::SetScriptProperty;
            request.script = {{"property", property.key()}, {"reset", true}};
        }
        else
        {
            request.type = UiActionType::CreateScriptBlueprintAsset;
            request.stringValue = ControlValue(*document->GetElementById("script-asset-path"));
        }
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "material-frame") RequestPreviewCamera(ModelPreviewNavigation::Frame);
    else if (action == "json-close") ShowMaterialDetails();
    else if (action == "resource-frame") RequestPreviewCamera(ModelPreviewNavigation::Frame);
    else if (action == "hair-reset") RequestHairReset();
    else if (action == "hair-save") SubmitHairSimulationSave();
    else if (action == "animation-browser-clip")
    {
        const auto* active = resourceEditor.ActiveTab();
        const size_t index = element.GetAttribute<size_t>("data-index", runtimeView.animationClips.size());
        if (active && active->kind == EditorAssetKind::Retarget && index < runtimeView.animationClips.size() &&
            CurrentModelPreviewGeneration() && runtimeView.animationWorldGeneration == CurrentModelPreviewGeneration() &&
            runtimeActionSink)
        {
            UiAction request;
            request.type = UiActionType::ControlAnimationPreview;
            request.previewCamera.worldGeneration = CurrentModelPreviewGeneration();
            request.stringValue = "previewClip";
            request.floatValue = static_cast<float>(index);
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
    }
    else if (action == "animation-play" || action == "animation-pause" ||
        action == "animation-clip" || action == "animation-time")
    {
        if (runtimeActionSink && HasCurrentModelPreview() &&
            runtimeView.animationWorldGeneration == CurrentModelPreviewGeneration())
        {
            UiAction request;
            request.type = UiActionType::ControlAnimationPreview;
            request.previewCamera.worldGeneration = CurrentModelPreviewGeneration();
            request.stringValue = action == "animation-clip" ?
                (!requestedRetargetPath.empty() ? "previewClip" : "clip") :
                action == "animation-time" ? "seek" : "play";
            request.floatValue = action == "animation-play" ? 1.0f : 0.0f;
            if (action == "animation-clip" || action == "animation-time")
            {
                const auto* active = resourceEditor.ActiveTab();
                const std::string prefix = active && active->kind == EditorAssetKind::Retarget ?
                    "retarget-animation" : "resource-animation";
                auto* control = document->GetElementById(prefix +
                    (action == "animation-clip" ? "-clip" : "-time"));
                try { request.floatValue = std::stof(ControlValue(*control)); }
                catch (...) { return; }
            }
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
    }
    else if (action == "resource-texture-fit") { ResetTextureView(); RefreshResourceEditor(); }
    else if (action == "resource-reload") RequestResourceReload();
    else if (action == "resource-tool")
    {
        resourceTransformTool = element.GetAttribute<std::string>("data-tool", "select");
        RefreshResourceEditor();
    }
    else if (action == "resource-space")
    {
        resourceTransformSpace = element.GetAttribute<std::string>("data-space", "world");
        RefreshResourceEditor();
    }
    else if (action == "browser-refresh")
    {
        if (jsonAssetRequestSink != nullptr) jsonAssetRequestSink->RequestRefreshAssetCatalog();
    }
    else if (action == "workspace-content" || action == "workspace-renderer")
    {
        docking.Activate(action == "workspace-renderer" ? "renderer" : "content");
    }
    else if (action == "save") RequestSave();
    else if (action == "save-all") RequestSaveAll();
    else if (action == "save-confirm") ConfirmSave();
    else if (action == "save-cancel")
    {
        if (saveWaiting.empty()) saveDialogVisible = false;
        RefreshSaveDialog();
    }
    else if (action == "save-select-all")
    {
        for (const auto& row : saveRows) saveChecked.insert(row.assetPath);
        RefreshSaveDialog();
    }
    else if (action == "context-browser-save") OpenSaveDialog(contextMenuTarget);
    else if (action == "context-class-source") OpenBlueprintClassSource(contextMenuTarget);
    else if (action == "context-class-create") BeginBlueprintClassAsset(contextMenuTarget);
    else if (action == "menu-open") ToggleMenu(element.GetAttribute<std::string>("data-menu", ""));
    else if (action == "menu-window")
    {
        // 面板可见性由停靠视图裁决；被拒绝时 docking 会给出人类可读的 notice。
        const auto& panels = UiWidgets::DockWorkspace::Panels();
        if (index < panels.size())
            docking.SetPanelVisible(panels[index], !docking.IsPanelVisible(panels[index]));
    }
    else if (action == "menu-level" && index < menuLevelPaths.size())
    {
        // 与 Content Browser 双击关卡走同一条 LoadWorld 事务；UI 不直接触碰 World。
        OpenBrowserAsset(menuLevelPaths[index]);
    }
    else if (action == "menu-reset-layout") docking.Reset();
    else if (action == "menu-restore-layout")
    {
        docking.RestoreSavedLayout();
    }
    else if (action == "about-open") aboutVisible = true;
    else if (action == "about-close") aboutVisible = false;
    else if (action == "viewport-tool")
    {
        // 工具条只切换本地高亮；数值 Transform 编辑走 Details，尚无视口 gizmo。
        // 这里刻意不产生任何 UiAction/资产命令，避免用假开关冒充已实现的编辑能力。
        viewportTransformTool = element.GetAttribute<std::string>("data-tool", "select");
        RefreshViewportToolbar();
    }
    else if (action == "viewport-space")
    {
        // 同上：World/Local 只是呈现状态，渲染器和相机不读取它。
        viewportTransformSpace = element.GetAttribute<std::string>("data-space", "world");
        RefreshViewportToolbar();
    }
    else if (action == "actor-exposure-manual" || action == "actor-exposure-auto")
    {
        const auto* actor = SelectedActor();
        if (runtimeActionSink == nullptr || actor == nullptr || actor->type != EditorSceneActorType::Camera) return;
        const ExposureMode mode = action == "actor-exposure-auto" ? ExposureMode::Auto : ExposureMode::Ev100;
        if (actor->camera.exposure.mode == mode) return;
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {actor->worldGeneration, actor->identity,
            EditorActorProperty::CameraExposureMode, 0, mode == ExposureMode::Auto ? 2.0f : 0.0f};
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "actor-csm-enable" || action == "actor-csm-disable")
    {
        if (runtimeActionSink == nullptr) return;
        const bool enabled = action == "actor-csm-enable";
        if (enabled == runtimeView.csmCastShadows) return;
        const auto* actor = SelectedActor();
        if (!actor || !actor->primaryDirectionalLight) return;
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {actor->worldGeneration, actor->identity,
            EditorActorProperty::CastShadows, 0, enabled ? 1.0f : 0.0f};
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "revert")
    {
        pendingEdits.clear();
        pendingValues.clear();
        SubmitPathCommand(EditorCommandType::RevertMaterialInstanceDocument);
    }
    else if (action == "validate")
    {
        if (!pendingEdits.empty())
        {
            QueueEdit("validate", {EditorCommandType::ValidateMaterialInstanceDocument,
                MaterialInstanceAssetPathPayload{pendingAssetPath}});
            FlushPendingEdits();
        }
        else SubmitPathCommand(EditorCommandType::ValidateMaterialInstanceDocument);
    }
    else if (action == "apply-preview")
    {
        // 修改 batch 已触发自动预览；不能再用旧 revision 紧跟着请求一次 Apply。
        if (!pendingEdits.empty()) FlushPendingEdits();
        else SubmitPathCommand(EditorCommandType::ApplyMaterialInstancePreview);
    }
    else if (action == "restore-preview")
    {
        // batch 的自动 Apply 在命令队列尾部；同帧 Restore 会被它覆盖，必须等权威快照。
        if (!pendingEdits.empty())
        {
            FlushPendingEdits();
            localStatus = "Edits submitted. Restore baseline after preview completes.";
        }
        else SubmitPathCommand(EditorCommandType::RestoreMaterialInstancePreviewBaseline);
    }
    else if (action == "reload" && snapshot.activeDocument)
    {
        if (!pendingEdits.empty())
        {
            localStatus = "Save or revert pending edits before reloading.";
            return;
        }
        EditorCommandEnvelope command;
        command.type = EditorCommandType::ReloadMaterialInstanceDocument;
        command.expectedDocumentRevision = snapshot.activeDocument->revision;
        command.payload = ReloadMaterialInstanceDocumentPayload{
            snapshot.activeDocument->assetPath, EditorDirtyDocumentPolicy::RequireClean};
        Submit(std::move(command));
    }
    else if (action == "reset-all" && snapshot.activeDocument)
    {
        pendingEdits.clear();
        pendingValues.clear();
        for (const auto& parameter : snapshot.activeDocument->parameters)
            pendingValues[parameter.name] = parameter.defaultValue;
        QueueEdit("all", {EditorCommandType::ResetMaterialInstanceOverrides,
            ResetMaterialInstanceOverridesPayload{snapshot.activeDocument->assetPath, EditorResetScope::All}});
    }
    else if (action == "reset-parameter" && snapshot.activeDocument && index < snapshot.activeDocument->parameters.size())
    {
        const auto& parameter = snapshot.activeDocument->parameters[index];
        pendingValues[parameter.name] = parameter.defaultValue;
        QueueEdit("parameter:" + parameter.name, {EditorCommandType::ClearMaterialParameterOverride,
            ClearMaterialParameterOverridePayload{snapshot.activeDocument->assetPath, parameter.name}});
    }
    else if (action == "reset-state" && snapshot.activeDocument && index < snapshot.activeDocument->renderStates.size())
    {
        const auto& state = snapshot.activeDocument->renderStates[index];
        QueueEdit("state:" + state.name, {EditorCommandType::ClearMaterialRenderStateOverride,
            ClearMaterialRenderStateOverridePayload{snapshot.activeDocument->assetPath,
                ParseEditorMaterialRenderStateField(state.name)}});
    }
    else if (action == "reset-texture" && snapshot.activeDocument && index < snapshot.activeDocument->textures.size())
    {
        const auto& texture = snapshot.activeDocument->textures[index];
        QueueEdit("texture:" + texture.slotName, {EditorCommandType::ClearMaterialTextureOverride,
            ClearMaterialTextureOverridePayload{snapshot.activeDocument->assetPath, texture.slotName}});
    }
    else if (action == "section")
    {
        // 展开状态只在面板里，切换不提交业务命令，也不写资产或停靠布局。
        ToggleSection(element.GetAttribute<std::string>("data-section", ""));
    }
    else if (action == "tab" && index < snapshot.documentTabs.size())
    {
        ShowMaterialDetails();
        FlushPendingEdits();
        EditorCommandEnvelope command;
        command.type = EditorCommandType::SelectMaterialInstanceDocument;
        command.payload = MaterialInstanceAssetPathPayload{snapshot.documentTabs[index].assetPath};
        Submit(std::move(command));
    }
    else if (action == "close")
    {
        const std::string targetPanel = element.GetAttribute<std::string>("data-key", "");
        const std::string targetPath = docking.ResourcePathForPanel(targetPanel);
        if (!targetPath.empty() && targetPanel.rfind("resource:material:", 0) != 0)
        {
            RequestCloseResource(targetPath);
            return;
        }
        size_t closeIndex = index;
        if (!targetPanel.empty())
        {
            closeIndex = snapshot.documentTabs.size();
            for (size_t candidate = 0; candidate < snapshot.documentTabs.size(); ++candidate)
                if (snapshot.documentTabs[candidate].assetPath == targetPath) closeIndex = candidate;
        }
        if (closeIndex >= snapshot.documentTabs.size()) return;
        const auto& tab = snapshot.documentTabs[closeIndex];
        if (tab.dirty || (!pendingEdits.empty() && pendingAssetPath == tab.assetPath))
        {
            FlushPendingEdits();
            pendingClosePath = tab.assetPath;
            pendingCloseKind = EditorAssetKind::MaterialInstance;
            saveThenClose = false;
            saveCloseSubmitted = false;
        }
        else CloseDocument(tab.assetPath, EditorDirtyDocumentPolicy::RequireClean);
    }
    else if (action == "close-cancel")
    {
        pendingClosePath.clear();
        saveThenClose = false;
        saveCloseSubmitted = false;
        discardBlueprintThenClose = discardBlueprintCloseSubmitted = false;
    }
    else if (action == "close-discard")
    {
        if (pendingCloseKind == EditorAssetKind::Blueprint || pendingCloseKind == EditorAssetKind::Tressfx)
        {
            discardBlueprintThenClose = true;
            discardBlueprintCloseSubmitted = false;
            return;
        }
        CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::DiscardChanges);
        pendingClosePath.clear();
        saveThenClose = false;
        saveCloseSubmitted = false;
    }
    else if (action == "close-save")
    {
        if (pendingCloseKind == EditorAssetKind::Blueprint || pendingCloseKind == EditorAssetKind::Tressfx)
        {
            saveStartedSequence = 0;
            for (const auto& feedback : snapshot.saveFeedback)
                saveStartedSequence = std::max(saveStartedSequence, feedback.sequence);
            saveThenClose = true;
            saveCloseSubmitted = false;
            return;
        }
        // 非活动页先选中以获得权威 revision；保存失败时不能关闭文档。
        if (!snapshot.activeDocument || snapshot.activeDocument->assetPath != pendingClosePath)
        {
            EditorCommandEnvelope command;
            command.type = EditorCommandType::SelectMaterialInstanceDocument;
            command.payload = MaterialInstanceAssetPathPayload{pendingClosePath};
            Submit(std::move(command));
        }
        saveThenClose = true;
        saveCloseSubmitted = false;
    }
    else if (action == "outliner-expand" && index < OutlinerActors().size())
    {
        const std::string& identity = OutlinerActors()[index].identity;
        if (outlinerExpansion.expandedActors.erase(identity) == 0)
            outlinerExpansion.expandedActors.insert(identity);
    }
    else if ((action == "outliner-actor" || action == "viewport-actor") && index < OutlinerActors().size())
    {
        SelectOutlinerActor(OutlinerActors()[index]);
    }
    else if (action == "viewport-actor-visuals")
    {
        showActorVisualization = !showActorVisualization;
        RefreshActorVisualization();
    }
    else if (action == "outliner-visible" && index < OutlinerActors().size())
    {
        // 眼睛不改选择、不改展开、不改滚动：它只发一条运行时命令，且身份用冻结快照里的
        // 稳定字符串，不用 DOM 上的数据或 sceneActors 下标。
        const EditorSceneActorSnapshot& actor = OutlinerActors()[index];
        if (runtimeActionSink == nullptr || !SceneActorHasVisibilityToggle(actor.type)) return;
        UiAction request;
        request.type = UiActionType::SetActorVisible;
        request.stringValue = actor.identity;
        request.intValue = actor.visible ? 0 : 1;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "outliner-slot" && index < OutlinerActors().size())
    {
        const EditorSceneActorSnapshot& actor = OutlinerActors()[index];
        SelectModelMaterial(MeshSceneActorModel(actor),
            element.GetAttribute<uint32_t>("data-component", 0));
    }
    else if (action == "slot" && sceneSelectionSink != nullptr && snapshot.selectedModel &&
        index < snapshot.selectedModel->materials.size())
    {
        SelectModelMaterial(*snapshot.selectedModel, index);
    }
    // 上下文菜单条目：目标一律取打开菜单时记下的稳定地址，不读面板的当前选择，
    // 否则"右键 B、菜单却改 A"就会静默发生。
    else if (action == "context-outliner-visible") ToggleContextActorVisibility();
    else if (action == "context-actor-material") OpenContextActorMaterial(index);
    else if (action == "context-browser-open") OpenContextBrowserAsset();
    else if (action == "context-view-mode" && runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = UiActionType::SetDebugViewMode;
        request.intValue = static_cast<int>(index);
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "context-save-all")
    {
        RequestSaveAll();
    }
    else if (runtimeActionSink != nullptr)
    {
        UiAction request;
        if (action == "immersive-view") request.type = UiActionType::ToggleDeveloperUi;
        else if (action == "exit-editor") request.type = UiActionType::Quit;
        else if (action == "viewport-view-mode")
        {
            // 视口工具条里只有 view mode 有真实后端：Lit=0 完整画面，Unlit=1 基础色。
            request.type = UiActionType::SetDebugViewMode;
            request.intValue = element.GetAttribute<int>("data-mode", 0);
        }
        else if (action == "offline-mode")
        {
            request.type = UiActionType::SetOfflineMode;
            request.intValue = element.GetAttribute<int>("data-value", 0);
        }
        else if (action == "offline-control")
        {
            request.type = UiActionType::ControlOfflineSession;
            request.intValue = element.GetAttribute<int>("data-value", 0);
        }
        else if (action == "offline-export") request.type = UiActionType::ExportOffline;
        else return;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
}

void MaterialInstanceAssetEditorPanel::HandleChange(Rml::Element& element, const std::string& action,
    const Rml::Event* event)
{
    if (action == "hair-sim")
    {
        // 拖动/输入只更新会话值并投递实时覆盖；保存是独立动作，不会自动写资产。
        std::size_t count = 0;
        const TressfxSimulationControl* controls = TressfxSimulationControls(count);
        const std::size_t index = element.GetAttribute<size_t>("data-index", 0);
        const std::size_t component = element.GetAttribute<size_t>("data-component", 0);
        if (index >= count || component >= controls[index].components) return;
        // WidgetSlider 先发 change(value)，返回后才写 value 属性；读属性会落后一拍，
        // 下一帧同步又把滑块拉回旧值。这里只消费该事件的新值，文本仍保留半成品输入。
        const bool sliderChange = event != nullptr &&
            element.GetAttribute<Rml::String>("type", "") == "range" &&
            event->GetParameters().count("value") != 0;
        const std::string text = sliderChange
            ? Number(event->GetParameter<float>("value", 0.0f)) : ControlValue(element);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        // 文本控件的半成品输入不能当成 0；只接受整段都是数字的值。
        if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) return;
        auto values = PanelSimulationValue(hairSimulationValues, controls[index]);
        values[component] = value;
        hairSimulationValues[controls[index].field] =
            nlohmann::json::array({values[0], values[1], values[2]});
        const auto* active = resourceEditor.ActiveTab();
        const auto* authored = active ? TressfxDocument(active->path) : nullptr;
        hairSimulationStatus = authored && TressfxGpuSimulationDefaults(authored->document)
            ? "Live preview in this viewport (not saved)." : "Default value edited (not saved).";
        SubmitHairSimulationLive(index, values);
        return;
    }
    if (action.rfind("debug-draw-", 0) == 0)
    {
        if (!runtimeActionSink || !runtimeView.debugDraw || !runtimeView.motionWorldGeneration || runtimeView.offlineMode) return;
        UiAction request; request.type = UiActionType::ControlDebugDraw;
        request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
        auto& control = request.debugDrawControl;
        control.enabled = element.HasAttribute("checked");
        if (action == "debug-draw-enabled")
        {
            if (control.enabled == runtimeView.debugDraw->enabled) return;
            control.action = Debug::ControlAction::Enabled;
        }
        else if (action == "debug-draw-owner")
        {
            control.action = Debug::ControlAction::Owner; control.name = ControlValue(element);
            // RmlUi 可在后续 Update 才发布 options 初始化的 change；相同快照值不是用户修改。
            if (control.name == runtimeView.debugDraw->ownerFilter) return;
        }
        else if (action == "debug-draw-category")
        {
            const size_t index = element.GetAttribute<size_t>("data-index", 0);
            if (index >= runtimeView.debugDraw->categories.size()) return;
            if (control.enabled == runtimeView.debugDraw->categories[index].enabled) return;
            control.action = Debug::ControlAction::Category; control.name = runtimeView.debugDraw->categories[index].name;
        }
        else return;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
        return;
    }
    if (action == "motion-debug")
    {
        if (runtimeActionSink && runtimeView.motionWorldGeneration && !runtimeView.offlineMode)
        {
            UiAction request;
            request.type = UiActionType::ControlMotionDebug;
            request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
            request.intValue = element.HasAttribute("checked") ? 1 : 0;
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "motion-replay-seek")
    {
        if (runtimeActionSink && runtimeView.motionDebug && runtimeView.motionWorldGeneration &&
            !runtimeView.offlineMode && runtimeView.motionDebug->replayFrames > 0)
        {
            int frame = 0;
            try { frame = std::stoi(ControlValue(element)); }
            catch (...) { return; }
            UiAction request;
            request.type = UiActionType::ControlMotionReplay;
            request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
            request.intValue = std::clamp(frame, 0, runtimeView.motionDebug->replayFrames - 1);
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "motion-timeline-play" || action == "motion-timeline-pause" ||
        action == "motion-timeline-seek")
    {
        if (runtimeActionSink && runtimeView.motionDebug && runtimeView.motionWorldGeneration &&
            !runtimeView.offlineMode && runtimeView.motionDebug->playbackDuration > 0)
        {
            UiAction request;
            request.type = UiActionType::ControlScriptAnimation;
            request.previewCamera.worldGeneration = runtimeView.motionWorldGeneration;
            request.stringValue = runtimeView.motionDebug->componentIdentity;
            if (action == "motion-timeline-seek")
            {
                request.intValue = 0;
                try { request.floatValue = std::stof(ControlValue(element)); }
                catch (...) { return; }
            }
            else
            {
                request.intValue = 1;
                request.floatValue = action == "motion-timeline-play" ? 1.0f : 0.0f;
            }
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "animation-clip" || action == "animation-time")
    {
        HandleClick(element, action);
        return;
    }
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    const std::string value = ControlValue(element);
    localStatus.clear();
    if (action.rfind("blueprint-asset-", 0) == 0) HandleBlueprintAssetEvent(element, action, false);
    else if (HandleSceneLogicEvent(element, action, false)) {}
    else if (HandleSceneCompositePropertyEvent(element, action, false)) {}
    else if (action == "script-placement-mode")
    {
        if (value != "place" && value != "create") return;
        scriptPlacementCreateAsset = value == "create";
        RefreshScriptControls();
    }
    else if (action == "script-time-scale")
    {
        if (!runtimeActionSink || !runtimeView.scripts.available || runtimeView.offlineMode ||
            IsAssetEditorVisible() || HasModal()) return;
        float scale;
        if (!ParseNumber(value, scale) || scale < 0 || scale > 100)
        { localStatus = "Time Scale must be a finite number from 0 to 100."; return; }
        if (scale == runtimeView.scripts.timeScale) return;
        UiAction request;
        request.type = UiActionType::SetScriptTimeScale;
        request.floatValue = scale;
        request.actorProperty.worldGeneration = runtimeView.scripts.worldGeneration;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-blueprint-name")
    {
        const auto* actor = SelectedActor();
        if (actor && value != actor->displayName) RequestScriptEdit(UiActionType::RenameScriptBlueprint, {{"name", value}});
    }
    else if (action.rfind("script-physics-", 0) == 0)
    {
        const auto field = action.substr(15);
        const auto* actor = SelectedActor();
        if (!actor) return;
        nlohmann::json payload{{"field", field}};
        if (field == "motion" || field == "shape" || field == "component" ||
            field == "constraint-type" || field == "constraint-target") payload["value"] = value;
        else if (field == "constraint-axis") payload["value"] = nlohmann::json::parse(value);
        else if (field == "trigger") payload["value"] = element.HasAttribute("checked");
        else
        {
            float number;
            if (!ParseNumber(value, number)) { localStatus = "A complete finite number is required."; return; }
            payload["value"] = number;
            if (field == "halfExtents" || field == "constraint-anchor")
                payload["axis"] = element.GetAttribute<size_t>("data-component", 0);
        }
        if (field.rfind("constraint-", 0) == 0) payload["index"] = index;
        RequestScriptEdit(UiActionType::SetScriptPhysics, std::move(payload));
    }
    else if (action.rfind("script-component-", 0) == 0 && runtimeActionSink != nullptr)
    {
        const auto* actor = SelectedActor();
        if (!actor || actor->type != EditorSceneActorType::Script ||
            index >= actor->scriptComponents.size() ||
            actor->scriptSpawned || runtimeView.scripts.state != ScriptPlayState::Editing) return;
        const auto field = action.substr(17);
        const auto& node = actor->scriptComponents[index];
        // 资产拥有的毛发字段不允许从关卡写：即使旧控件或脚本仍发出事件也直接丢弃。
        // 绑定/权重只在蓝图资产页作者，关卡只能改放置与资产白名单授权的实例覆写。
        if (node.value("type", "") == "tressfx" &&
            (field == "modelPath" || field == "rootBone" || field == "weights")) return;
        if (field.rfind("collision", 0) == 0 && field != "collisionMask")
        {
            if (!actor->scriptBlueprintPath.empty()) return;
            nlohmann::json payload{{"component", ScriptComponentId(node)}, {"field", field}};
            if (field == "collisionShape") payload["value"] = value;
            else
            {
                float number;
                if (!ParseNumber(value, number)) return;
                payload["value"] = number;
                if (field == "collisionHalfExtents")
                    payload["axis"] = element.GetAttribute<size_t>("data-component", 0);
            }
            RequestScriptEdit(UiActionType::SetScriptComponent, std::move(payload));
            return;
        }
        if (!actor->scriptBlueprintPath.empty() && !ScriptComponentAllowsInstance(
            actor->scriptComponentInstanceEditable, ScriptComponentId(node), field)) return;
        nlohmann::json candidate;
        if (field == "parent" || field == "modelPath" || field == "materialOverride")
        {
            if (node.value(field, "") == value) return;
            candidate = value;
        }
        else if (field == "autoActivate" || field == "visible" || field == "doCollisionTest" ||
            field == "enableCameraLag" || field == "enableCameraRotationLag" || field == "usePawnControlRotation" ||
            field == "inheritPitch" || field == "inheritYaw" || field == "inheritRoll" ||
            field == "absoluteRotation" || field == "drawDebug" || field == "useCameraLagSubstepping") candidate = value == "true";
        else if (field == "fov" || field == "near_clip" || field == "far_clip" || field == "priority" ||
            field == "targetArmLength" || field == "probeRadius" || field == "collisionMask" ||
            field == "cameraLagSpeed" || field == "cameraLagMaxDistance" || field == "cameraRotationLagSpeed" ||
            field == "cameraLagMaxTimeStep" || IsScriptLightNumber(field))
        {
            float number;
            if (!ParseNumber(value, number)) return;
            candidate = number;
        }
        else
        {
            float number;
            if ((field != "position" && field != "rotation" && field != "scale" && field != "color" &&
                 field != "socketOffset" && field != "rotationOffset" && field != "targetOffset") || !ParseNumber(value, number)) return;
            const auto axis = element.GetAttribute<size_t>("data-component", 0);
            if (axis >= 3) return;
            const int fallback = field == "scale" || field == "color" ? 1 : 0;
            candidate = node.value(field, nlohmann::json::array({fallback, fallback, fallback}));
            if (candidate[axis] == number) return;
            candidate[axis] = number;
        }
        if (node.contains(field) && node.at(field) == candidate) return;
        UiAction request;
        request.type = UiActionType::SetScriptComponent;
        request.actorProperty.worldGeneration = actor->worldGeneration;
        request.actorProperty.identity = actor->identity;
        request.script = {{"component", node.value("id", "root")}, {"field", field}, {"value", candidate}};
        if (candidate.is_array())
        {
            const auto axis = element.GetAttribute<size_t>("data-component", 0);
            request.script["axis"] = axis;
            request.script["value"] = candidate[axis];
        }
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-property" && runtimeActionSink != nullptr)
    {
        const auto* actor = SelectedActor();
        if (!actor || actor->type != EditorSceneActorType::Script ||
            index >= actor->scriptProperties.size() || runtimeView.scripts.state != ScriptPlayState::Editing) return;
        auto property = actor->scriptProperties.begin();
        std::advance(property, index);
        if (std::find(actor->scriptInstanceEditable.begin(), actor->scriptInstanceEditable.end(),
            property.key()) == actor->scriptInstanceEditable.end()) return;
        try
        {
            const auto schema = ScriptPropertyField(runtimeView.scripts.classes,
                actor->scriptClass, property.key(), property.value());
            const bool vector = ScriptPropertyUsesComponents(schema, property.value());
            const auto axis = element.GetAttribute<size_t>("data-component", 0);
            if (vector && axis >= property.value().size()) return;
            nlohmann::json candidate;
            if (schema.at("type") == "quaternion")
            {
                if (!ParseScriptQuaternion(value, candidate)) return;
            }
            else candidate = property.value().is_string() ? nlohmann::json(value)
                : nlohmann::json::parse(value);
            if (candidate == (vector ? property.value()[axis] : property.value())) return;
            UiAction request;
            request.type = UiActionType::SetScriptProperty;
            request.actorProperty.worldGeneration = actor->worldGeneration;
            request.actorProperty.identity = actor->identity;
            request.script = {{"property", property.key()}, {"value", candidate}};
            if (vector) request.script["axis"] = axis;
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        catch (const nlohmann::json::exception&) { localStatus = "A complete property value is required."; }
    }
    else if (action == "save-file" && index < saveRows.size() && saveWaiting.empty())
    {
        const auto& path = saveRows[index].assetPath;
        if (element.HasAttribute("checked")) saveChecked.insert(path);
        else saveChecked.erase(path);
        RefreshSaveDialog();
    }
    else if (action == "outliner-search")
    {
        // 搜索只改变可见性，不提交业务命令；折叠规则与 Content Browser 共用。
        outlinerQuery = value;
    }
    else if (action == "outliner-type")
    {
        outlinerTypeFilter = value.empty() ? "all" : value;
    }
    else if (action == "outliner-sort")
    {
        // 排序只改变组内顺序；分组顺序、选中态与展开集合都不参与，因此也不被重排。
        SceneActorSortOrder order = outlinerSortOrder;
        if (ParseSceneActorSortOrder(value, order)) outlinerSortOrder = order;
    }
    else if (action == "actor-property" && runtimeActionSink != nullptr)
    {
        const auto* actor = SelectedActor();
        float number = 0.0f;
        if (actor == nullptr || !ParseNumber(value, number))
        {
            localStatus = "A complete finite number is required.";
            return;
        }
        const auto property = static_cast<EditorActorProperty>(index);
        const uint32_t component = element.GetAttribute<uint32_t>("data-component", 0);
        for (const auto& field : SceneActorEditableFields(*actor))
        {
            if (field.property != property || component >= field.components) continue;
            if (const char* exposureField = CameraExposureField(property))
            {
                if (!CameraExposureFieldEditable(property, actor->camera.exposure.mode)) return;
                try
                {
                    // UI 给出即时错误；GT 仍按当时的完整作者状态校验，避免连续事件互相覆盖。
                    EditCameraExposure(actor->camera.exposure, exposureField, number);
                }
                catch (const std::invalid_argument& error)
                {
                    localStatus = error.what();
                    return;
                }
            }
            if (number == field.value[component]) return;
            UiAction request;
            request.type = UiActionType::SetActorProperty;
            request.actorProperty = {actor->worldGeneration, actor->identity, property, component, number};
            runtimeActionSink->RequestRuntimeAction(std::move(request));
            return;
        }
    }
    else if (action == "actor-sun-intensity" && runtimeActionSink != nullptr)
    {

        // 作者属性带 actor 身份和 World 代数；console 的 sun 调试动作仍独立保留。
        float number = 0.0f;
        if (!ParseNumber(value, number))
        {
            localStatus = "A complete finite number is required.";
            return;
        }
        // 沿用主方向光控件的 lux 范围，精确输入与拖动使用同一编辑边界。
        if (number < SunIntensityMin || number > SunIntensityMax)
        {
            localStatus = "Sun intensity must be between " + SceneActorNumber(SunIntensityMin) +
                " and " + SceneActorNumber(SunIntensityMax) + ".";
            return;
        }
        const EditorSceneActorSnapshot* actor = SelectedActor();
        if (actor == nullptr || !ChangedFloat(number, actor->light.intensity)) return;
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {actor->worldGeneration, actor->identity,
            EditorActorProperty::Intensity, 0, number};
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "parameter" && snapshot.activeDocument &&
        index < snapshot.activeDocument->parameters.size())
    {
        const auto& parameter = snapshot.activeDocument->parameters[index];
        const uint32_t component = element.GetAttribute<uint32_t>("data-component", 0);
        float number = 0.0f;
        if (component >= parameter.effectiveValue.ComponentCount() || !ParseNumber(value, number))
        {
            localStatus = "A complete finite number is required.";
            return;
        }
        const auto previous = pendingValues.find(parameter.name);
        EditorParameterValue candidate = previous == pendingValues.end()
            ? parameter.effectiveValue : previous->second;
        candidate.values[component] = number;
        pendingValues[parameter.name] = candidate;
        QueueEdit("parameter:" + parameter.name, {EditorCommandType::SetMaterialParameterOverride,
            SetMaterialParameterOverridePayload{snapshot.activeDocument->assetPath,
                parameter.name, CommandType(candidate.type), CommandValue(candidate)}});
    }
    else if (action == "state" && snapshot.activeDocument && index < snapshot.activeDocument->renderStates.size())
    {
        const auto& state = snapshot.activeDocument->renderStates[index];
        const auto field = ParseEditorMaterialRenderStateField(state.name);
        EditorMaterialRenderStateValue candidate;
        switch (field)
        {
        case EditorMaterialRenderStateField::RenderMode: candidate = ParseEditorMaterialRenderMode(value); break;
        case EditorMaterialRenderStateField::CullMode: candidate = ParseEditorMaterialCullMode(value); break;
        case EditorMaterialRenderStateField::ShadingModel: candidate = ParseEditorMaterialShadingModel(value); break;
        }
        QueueEdit("state:" + state.name, {EditorCommandType::SetMaterialRenderStateOverride,
            SetMaterialRenderStateOverridePayload{snapshot.activeDocument->assetPath, field, candidate}});
    }
    else if (action == "texture" && snapshot.activeDocument && index < snapshot.activeDocument->textures.size())
    {
        const auto& texture = snapshot.activeDocument->textures[index];
        QueueEdit("texture:" + texture.slotName, {EditorCommandType::SetMaterialTextureOverride,
            SetMaterialTextureOverridePayload{snapshot.activeDocument->assetPath, texture.slotName, value}});
    }
    else if (runtimeActionSink != nullptr)
    {
        UiAction request;
        if (action == "offline-setting")
        {
            int number = 0;
            const std::string field = element.GetAttribute<std::string>("data-field", "");
            if (!ParseNumber(value, number))
            {
                localStatus = "A complete integer is required.";
                return;
            }
            if (const char* error = OfflineSettingValidationError(field, number))
            {
                localStatus = error;
                return;
            }
            request.type = UiActionType::SetOfflineSetting;
            request.stringValue = field;
            request.intValue = number;
        }
        else if (action == "offline-aov")
        {
            int mode = 0;
            if (!ParseNumber(value, mode) || mode < 0 || mode > 2) return;
            request.type = UiActionType::SetOfflineAov;
            request.intValue = mode;
        }
        else if (action == "offline-exposure")
        {
            float exposure = 0.0f;
            if (!ParseNumber(value, exposure) || exposure <= 0.0f)
            {
                localStatus = "Exposure must be a positive finite number.";
                return;
            }
            request.type = UiActionType::SetOfflineExposure;
            request.floatValue = exposure;
        }
        else return;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
}

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

void MaterialInstanceAssetEditorPanel::ProcessEvent(Rml::Event& event)
{
    if (refreshingControls || document == nullptr) return;
    if (ProcessBlueprintAssetDrag(event)) return;
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
        action != "save-file" && action != "save-confirm" && action != "save-cancel" &&
        action != "save-select-all")
        return;
    // checkbox 在目标节点的默认 click 中切换 checked；捕获阶段不能截断它。
    // 随后的 change 才把选择同步到弹窗状态，程序刷新仍由 WidgetRefreshScope 抑制。
    if (event.GetType() == "click" && (action == "save-file" || action == "script-physics-trigger" ||
        action == "blueprint-asset-instance" || action == "blueprint-asset-physics-trigger" ||
        action == "blueprint-asset-component-instance" || action == "blueprint-asset-snap-enabled" ||
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
