#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>

#include "core/renderSession.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/editorTextMatch.h"
#include "editor/ui/editorViewportAxisIndicator.h"
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

// 文档命令的可用性统一作用在所有发出该命令的控件上，避免出现两套互相矛盾的启用状态。
// 工具条不再重复排列这些命令（见 editor-workspace.rml 的注释），所以只剩菜单项；
// 这里保留数组形式，将来再加其它发布点时只需追加 id。
constexpr const char* DocumentCommandIds[] = {
    "menu-file-save", "menu-file-revert", "menu-file-reload",
    "menu-edit-reset", "menu-edit-validate"};

std::string Number(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
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
constexpr const char* DetailsSectionKeys[] = {"general", "render-states", "parameters", "textures"};

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

// 运行时页 sun 滑条的范围（ui/runtime-control.rml 的 #sun-intensity-slider）。
// Details 的强度控件与它共用同一条 SetSunIntensity 命令，必须同区间：那一页即使被隐藏，
// 它的双向 data binding 仍会把超范围值截断后回写一条新命令（历史上实测到 13→10 的抖动）。
//
// 这里跟随物理光照改为**勒克斯**：master 把运行时滑条与太阳强度语义一起迁到 lux
// （`documents/contracts/physical-lighting-and-exposure.md` 记录旧单位 × 1000 = lux），
// 而本仓库的编辑器面板不在 master 上，所以那次迁移改不到这里 —— 合并时由测试
// `ActorIntensitySliderRangeMatchesTheRuntimeSunSlider` 抓出来，正是它存在的目的。
constexpr float SunIntensityMin = 0.0f;
constexpr float SunIntensityMax = 150000.0f;

std::string ActorCountLabel(size_t count)
{
    return std::to_string(count) + (count == 1 ? " actor" : " actors");
}

// 树行只携带展示数据与动作地址；动作索引是 sceneActors 的下标，过滤只改变可见性。
UiWidgets::TreeRowData OutlinerGroupRow(
    const EditorSceneActorGroupSpec& spec, size_t groupIndex, size_t actorCount, bool expanded)
{
    UiWidgets::TreeRowData row;
    row.label = std::string(spec.title) + " (" + std::to_string(actorCount) + ")";
    row.select = {"outliner-group", groupIndex};
    row.toggle = {"outliner-group", groupIndex};
    row.hasChildren = true;
    row.expanded = expanded;
    row.rowClass = "group-row";
    return row;
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
        "scene-models", "material-slots", "asset-list", "document-tabs",
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
        "content-browser-page", "renderer-page", "editor-viewport-surface", "editor-viewport-image",
        "workspace-viewport-tab", "workspace-outliner-tab", "workspace-details-tab",
        "workspace-resource-tab", "workspace-resource-icon", "workspace-resource-label",
        "resource-editor", "resource-editor-toolbar", "resource-reload", "resource-close", "resource-preview-panel",
        "resource-preview-title", "resource-preview-kind", "resource-preview-surface",
        "resource-preview-icon", "resource-preview-status", "resource-preview-path",
        "resource-preview-image",
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
    detailsIdentity.clear();
    RefreshWorkspace();
    RefreshLists();
    RefreshDetails();
    UpdateRuntimeView(runtimeView);
    return true;
}

void MaterialInstanceAssetEditorPanel::Detach()
{
    if (document != nullptr)
    {
        document->RemoveEventListener("click", this);
        document->RemoveEventListener("change", this);
        document->RemoveEventListener("dblclick", this);
        document->RemoveEventListener("mousedown", this, true);
        document->RemoveEventListener("mousemove", this, true);
        document->RemoveEventListener("mouseup", this, true);
        contentBrowser.Detach();
        jsonInspector.Detach();
        resourceJsonInspector.Detach();
        docking.Detach();
        document = nullptr;
    }
    widgetDocument.Detach();
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
    if (viewingJson || resourceEditorVisible) return;
    FlushPendingEdits(true);
}

void MaterialInstanceAssetEditorPanel::Build(const MaterialInstanceEditorSnapshot& value)
{
    // 不把旧 UI 修改静默重定位到新文档或新 revision；CommandBus 负责拒绝过期 batch。
    FlushPendingEdits();
    snapshot = value;
    RefreshWorkspace();
    resourceEditorVisible = IsResourceEditorVisible();
    if (saveThenClose && !saveCloseSubmitted && snapshot.activeDocument &&
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
        SetMarkup("close-document-name", Text(pendingClosePath));
        SetMarkup("close-document-status", Text(snapshot.statusMessage));
        SetDisabled("close-save", saveThenClose);
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
    RefreshViewportAxisIndicator();
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

    const Eigen::Vector3f forward = camera->camera.forward.normalized();
    const Eigen::Vector3f right = camera->camera.right.normalized();
    const Eigen::Vector3f up = camera->camera.up.normalized();
    const auto triad = EditorUi::ProjectAxisIndicatorTriad(right, up, forward);

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
    const std::string wanted = signature.str();
    if (wanted == axisIndicatorSignature) return;
    axisIndicatorSignature = wanted;

    static const char* kBarIds[3] = {
        "viewport-axis-bar-x", "viewport-axis-bar-y", "viewport-axis-bar-z"};
    static const char* kLabelIds[3] = {
        "viewport-axis-label-x", "viewport-axis-label-y", "viewport-axis-label-z"};

    // 远的先画、近的后画，让朝向观察者的轴盖住背向的轴。depth 越小越靠近观察者，
    // 因此按 depth 降序排列。
    std::array<size_t, 3> order = {0, 1, 2};
    std::sort(order.begin(), order.end(), [&triad](size_t lhs, size_t rhs)
    {
        return triad[lhs].depth > triad[rhs].depth;
    });

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

        if (projection.degenerate)
        {
            // 与视线近乎平行的轴在屏幕上没有方向可言：杆收成零宽，**标签一并隐藏**。
            // 只收杆不藏标签会让它落在锚点上，与另外两个轴本就在锚点附近的字母叠成一团
            // （实测三个字母挤在 6x8 px 内，无法辨认）。位置也归零，避免隐藏期间留下旧偏移。
            widgetDocument.SetProperty(barId, "width", Pixels(0.0f));
            widgetDocument.SetProperty(labelId, "display", "none");
            widgetDocument.SetProperty(labelId, "margin-left", Pixels(-5.0f));
            widgetDocument.SetProperty(labelId, "margin-top", Pixels(-6.0f));
            continue;
        }

        float offsetX = 0.0f;
        float offsetY = 0.0f;
        EditorUi::AxisIndicatorLabelOffset(projection, offsetX, offsetY);
        widgetDocument.SetProperty(labelId, "display", "block");
        widgetDocument.SetProperty(labelId, "margin-left", Pixels(offsetX - 5.0f));
        widgetDocument.SetProperty(labelId, "margin-top", Pixels(offsetY - 6.0f));

        const float length = EditorUi::kAxisIndicatorRadiusPx;
        widgetDocument.SetProperty(barId, "width", Pixels(length));
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
    for (const char* menu : {"file", "edit", "window", "help"})
        widgetDocument.SetClass("menu-" + std::string(menu), "open", openMenu == menu);
}

bool MaterialInstanceAssetEditorPanel::IsInsideMenuBar(const Rml::Element* element) const
{
    if (document == nullptr) return false;
    const auto* menuBar = document->GetElementById("editor-menubar");
    for (const auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current == menuBar) return true;
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
    if (element != nullptr && docking.IsSceneViewportElement(element))
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
    else if (contextMenuKind == ContextMenuKind::BrowserAsset)
    {
        markup += UiWidgets::MenuItem("context-browser-open", {"context-browser-open", 0},
            "Open", false, false);
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
        // CSM 设置要写进活动场景文件：没有活动 World 就没有写入目标，此时明确禁用，
        // 而不是提交一条注定失败的运行时命令（禁用表现与 Edit 的 Undo/Redo 同一规则）。
        const bool hasWorld = !runtimeView.activeWorldPath.empty();
        markup += UiWidgets::MenuItem("context-save-csm", {"context-save-csm", 0},
            "Save CSM Settings to Scene", false, !hasWorld);
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
    contentBrowser.Build(snapshot);
    std::string tabs;
    for (size_t index = 0; index < snapshot.documentTabs.size(); ++index)
    {
        const auto& tab = snapshot.documentTabs[index];
        tabs += UiWidgets::DocumentTab(
            std::filesystem::path(tab.assetPath).filename().string() + (tab.dirty ? " *" : ""),
            {"tab", index}, {"close", index}, tab.assetPath == snapshot.selectedDocumentPath);
    }
    SetMarkup("document-tabs", std::move(tabs));
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
        if (row.isGroup || row.isSlot || row.actorIndex >= actors.size()) continue;
        const EditorSceneActorSnapshot& actor = actors[row.actorIndex];
        if (!SceneActorHasVisibilityToggle(actor.type)) continue;
        visuals += "\n" + actor.identity + (actor.visible ? "=1" : "=0");
    }
    return visuals;
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
        outlinerExpansion.collapsedGroups.clear();
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
    // 选择态与可见性只改已挂载行的 class/文本/眼睛图标，不改变行集合，但对 markup
    // 是可见变化，因此与窗口一起参与"是否需要重挂 DOM"的判断。
    const std::string visuals = OutlinerMountedVisuals();
    if (!outlinerMounted ||
        outlinerMountedRevision != outlinerRowsRevision ||
        !SameSceneActorWindow(outlinerMountedWindow, window) ||
        outlinerMountedVisuals != visuals)
    {
        SetMarkup("scene-models", OutlinerRowsMarkup(window));
        outlinerMounted = true;
        outlinerMountedRevision = outlinerRowsRevision;
        outlinerMountedWindow = window;
        outlinerMountedVisuals = visuals;
    }

    // 计数始终是筛选结果（分组行的 actorCount 之和），与折叠状态和挂载窗口都无关。
    size_t visibleActors = 0;
    for (const EditorSceneActorRow& row : outlinerRows)
        if (row.isGroup) visibleActors += row.actorCount;
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
    const auto& specs = SceneActorGroupSpecs();
    std::string markup;
    if (window.top > 0.0f)
        markup += "<div class=\"outliner-spacer\" style=\"height: " +
            std::to_string(window.top) + "px;\"></div>";
    for (size_t index = window.begin; index < window.end && index < outlinerRows.size(); ++index)
    {
        const EditorSceneActorRow& row = outlinerRows[index];
        if (row.isGroup)
        {
            // 分组事件按规范表下标寻址；分组行的类型 -> 下标为此处的唯一下标来源。
            const size_t groupIndex = SceneActorGroupIndex(row.type);
            const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(row.type);
            if (spec == nullptr || groupIndex >= specs.size()) continue;
            markup += UiWidgets::TreeRow(
                OutlinerGroupRow(*spec, groupIndex, row.actorCount, row.expanded));
            continue;
        }
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
    if (actor.type == EditorSceneActorType::Mesh)
    {
        // mesh actor 仍走迁移前的 MI 导航路由：整模型选择 + 视口描边完全一致。
        sceneSelectionSink->RequestModelSelection(SelectionModel(MeshSceneActorModel(actor)));
        return;
    }
    sceneSelectionSink->RequestActorSelection(actor.identity);
}

void MaterialInstanceAssetEditorPanel::ToggleOutlinerGroup(size_t groupIndex)
{
    const auto& specs = SceneActorGroupSpecs();
    if (groupIndex >= specs.size()) return;
    const std::string key = specs[groupIndex].key;
    if (outlinerExpansion.collapsedGroups.erase(key) == 0)
        outlinerExpansion.collapsedGroups.insert(key);
}

void MaterialInstanceAssetEditorPanel::RefreshDetails()
{
    WidgetRefreshScope refresh(refreshingControls);
    if (viewingJson || resourceEditorVisible)
    {
        for (const char* id : DocumentCommandIds)
            SetDisabled(id, true);
        SetMarkup("editor-notice", Text(localStatus));
        return;
    }
    const EditorSceneActorSnapshot* actor = SelectedActor();
    const EditorSceneActorGroupSpec* actorSpec =
        actor == nullptr ? nullptr : FindSceneActorGroupSpec(actor->type);
    // 身份同时包含选中 actor 与打开文档：任一变化都要重建分组结构。
    const std::string identity = std::string(actor == nullptr || actorSpec == nullptr
        ? "no-actor" : actor->identity + "|" + actorSpec->key) + "\n" +
        (snapshot.activeDocument
            ? snapshot.activeDocument->assetPath + "|" + snapshot.activeDocument->schemaDigest
            : "closed");
    if (identity != detailsIdentity)
    {
        detailsIdentity = identity;
        std::string markup;
        // actor 分组在前：选中对象是本轮的详情来源，MI 文档分组沿用迁移前的结构。
        if (actor != nullptr) markup += ActorSectionMarkup(*actor);
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
    RefreshActorValues(actor);
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
    for (const EditorSceneActorField& field : SceneActorFields(actor))
        body += UiWidgets::PropertyInfo(field.label, field.value, field.id);
    if (actor.primaryDirectionalLight) body += PrimaryDirectionalLightControls(actor);
    const std::string key = ActorSectionKey(spec->key);
    return UiWidgets::CollapsibleSection(DetailsSectionId(key), key, spec->title, body,
        {"section"}, IsSectionExpanded(key));
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

// 只读字段与两个可编辑控件都跟随冻结快照；焦点控件（正在拖动/输入）不会被回写。
void MaterialInstanceAssetEditorPanel::RefreshActorValues(const EditorSceneActorSnapshot* actor)
{
    if (actor == nullptr) return;
    for (const EditorSceneActorField& field : SceneActorFields(*actor))
        SetMarkup(field.id, Text(field.value));
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
    if (!resourceEditor.Open(path, kind))
    {
        localStatus = "Too many resource editors are open.";
        return;
    }
    viewingJson = false;
    resourceEditorVisible = true;
    localStatus.clear();
    RequestActiveResourceMetadata();
    if (kind == EditorAssetKind::Mesh && runtimeActionSink != nullptr)
    {
        // 回调只投递路径；GT 复用 previewmodel 的 LoadWorld 事务，不在 UI 内加载资产。
        // 即使标签已打开也重新请求，保证切过关卡后再次双击仍能回到该模型。
        RequestModelPreview(path);
    }
    RefreshResourceEditor();
    docking.SetPanelVisible(ResourcePanelId(path), true);
    docking.Activate(ResourcePanelId(path));
}

void MaterialInstanceAssetEditorPanel::CloseActiveResource()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr) return;
    resourceEditor.Close(active->path);
    resourceEditorVisible = resourceEditor.ActiveTab() != nullptr;
    if (!resourceEditorVisible) ReturnToLevel();
    if (!resourceEditorVisible) localStatus.clear();
    RefreshResourceEditor();
    const auto* next = resourceEditor.ActiveTab();
    if (next != nullptr)
    {
        RequestActiveResourceMetadata();
        docking.Activate(ResourcePanelId(next->path));
        if (next->kind == EditorAssetKind::Mesh && !HasCurrentModelPreview())
            RequestModelPreview(next->path);
    }
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
    resourceEditor.RequestReload();
    const std::string path = resourceEditor.ConsumeReloadRequest();
    if (path.empty()) return;
    if (jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && active->kind == EditorAssetKind::Mesh) RequestModelPreview(path);
}

void MaterialInstanceAssetEditorPanel::RequestModelPreview(const std::string& path)
{
    if (runtimeActionSink == nullptr) return;
    if (requestedPreviewPath.empty() && !runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() != "SC_model_preview.json")
        levelWorldPath = runtimeView.activeWorldPath;
    requestedPreviewPath = path;
    requestedPreviewGeneration = 0;
    for (const auto& actor : OutlinerActors())
        requestedPreviewGeneration = std::max(requestedPreviewGeneration, actor.worldGeneration);
    UiAction request;
    request.type = UiActionType::PreviewModel;
    request.stringValue = path;
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
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr || active->kind != EditorAssetKind::Mesh ||
        active->path != requestedPreviewPath) return false;
    // JSON 读取完成不能证明 GPU 模型已加载；只在新预览 World 的冻结快照发布后展示。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_model" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
            return true;
    return false;
}

void MaterialInstanceAssetEditorPanel::RefreshResourceEditor()
{
    if (document == nullptr) return;
    const auto* active = resourceEditor.ActiveTab();
    const bool isMesh = active != nullptr && active->kind == EditorAssetKind::Mesh;
    const bool previewReady = HasCurrentModelPreview();
    widgetDocument.SetProperty("resource-preview-image", "display", previewReady ? "block" : "none");
    for (const char* id : {"resource-preview-icon", "resource-preview-status", "resource-preview-path"})
        widgetDocument.SetProperty(id, "display", previewReady ? "none" : "block");
    widgetDocument.SetClass("resource-model-tools", "visible", isMesh);
    widgetDocument.SetClass("resource-model-info", "visible", isMesh);
    for (const char* id : {"resource-tool-select", "resource-tool-move", "resource-tool-rotate", "resource-tool-scale",
                           "resource-space-world", "resource-space-local"})
        SetDisabled(id, !isMesh);
    if (active == nullptr)
    {
        docking.SetResourceTabs({});
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

    std::vector<EditorDockResourceTab> tabs;
    tabs.reserve(resourceEditor.Tabs().size());
    for (const auto& tab : resourceEditor.Tabs())
    {
        tabs.push_back({
            ResourcePanelId(tab.path),
            tab.path,
            std::filesystem::path(tab.path).filename().string(),
            EditorAssetKindIcon(tab.kind)});
    }
    docking.SetResourceTabs(tabs);
    SetDisabled("resource-reload", active->loading);
    SetMarkup("resource-preview-title", Text(active->snapshot != nullptr &&
        !active->snapshot->name.empty() ? active->snapshot->name :
        std::filesystem::path(active->path).filename().string()));
    SetMarkup("resource-preview-kind", Text(EditorAssetKindName(active->kind)));
    SetMarkup("resource-preview-path", Text(active->path));
    SetMarkup("resource-preview-status", active->loading
        ? "Reading JSON metadata..."
        : isMesh ? "Waiting for model preview" : "Preview unavailable: referenced data is not loaded.");
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
        std::string options = Option(texture.effectiveAssetPath, texture.effectiveAssetPath);
        for (const auto& candidate : snapshot.textureAssets)
        {
            if (candidate.valid && candidate.assetPath != texture.effectiveAssetPath)
                options += Option(candidate.assetPath, candidate.assetPath);
        }
        SetMarkup("texture-" + std::to_string(index), std::move(options));
        SetControlValue("texture-" + std::to_string(index), asset.textures[index].effectiveAssetPath);
        SetDisabled("reset-texture-" + std::to_string(index), !asset.textures[index].overrideAssetPath);
    }
}

void MaterialInstanceAssetEditorPanel::UpdateRuntimeView(const UiViewModelSnapshot& view)
{
    runtimeView = view;
    WidgetRefreshScope refresh(refreshingControls);
    SetMarkup("editor-frame-metrics", std::to_string(std::lround(view.framesPerSecond)) + " FPS");
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
    if (document != nullptr)
    {
        document->GetElementById("mode-realtime")->SetClass("selected", !view.offlineMode);
        document->GetElementById("mode-offline")->SetClass("selected", view.offlineMode);
    }
    RefreshViewportToolbar();
}

void MaterialInstanceAssetEditorPanel::CloseDocument(
    const std::string& path, EditorDirtyDocumentPolicy policy)
{
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
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    if (action == "json-close") ShowMaterialDetails();
    else if (action == "resource-close") CloseActiveResource();
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
        // 只切换本地高亮：这个仓库没有 gizmo，也没有 transform 编辑后端。
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
    else if (action == "actor-csm-enable" || action == "actor-csm-disable")
    {
        if (runtimeActionSink == nullptr) return;
        const bool enabled = action == "actor-csm-enable";
        if (enabled == runtimeView.csmCastShadows) return;
        UiAction request;
        request.type = UiActionType::SetCsmCastShadows;
        request.intValue = enabled ? 1 : 0;
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
    else if (action == "close" && index < snapshot.documentTabs.size())
    {
        const auto& tab = snapshot.documentTabs[index];
        if (tab.dirty || (!pendingEdits.empty() && pendingAssetPath == tab.assetPath))
        {
            FlushPendingEdits();
            pendingClosePath = tab.assetPath;
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
    }
    else if (action == "close-discard")
    {
        CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::DiscardChanges);
        pendingClosePath.clear();
        saveThenClose = false;
        saveCloseSubmitted = false;
    }
    else if (action == "close-save")
    {
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
    else if (action == "outliner-group") ToggleOutlinerGroup(index);
    else if (action == "outliner-expand" && index < OutlinerActors().size())
    {
        const std::string& identity = OutlinerActors()[index].identity;
        if (outlinerExpansion.expandedActors.erase(identity) == 0)
            outlinerExpansion.expandedActors.insert(identity);
    }
    else if (action == "outliner-actor" && index < OutlinerActors().size())
    {
        SelectOutlinerActor(OutlinerActors()[index]);
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
    else if (action == "context-save-csm" && runtimeActionSink != nullptr &&
        !runtimeView.activeWorldPath.empty())
    {
        // 与运行时页的 save_csm_to_scene 是同一条 UiAction，不新增场景写入路径。
        UiAction request;
        request.type = UiActionType::SaveCsmSettingsToScene;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (runtimeActionSink != nullptr)
    {
        UiAction request;
        if (action == "runtime-page") request.type = UiActionType::ToggleRuntimePage;
        else if (action == "immersive-view") request.type = UiActionType::ToggleDeveloperUi;
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

void MaterialInstanceAssetEditorPanel::HandleChange(Rml::Element& element, const std::string& action)
{
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    const std::string value = ControlValue(element);
    localStatus.clear();
    if (action == "outliner-search")
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
    else if (action == "actor-sun-intensity" && runtimeActionSink != nullptr)
    {

        // 与运行时页共用同一条 UiAction 路由，不新增 UI→renderer 的第二条路径。
        float number = 0.0f;
        if (!ParseNumber(value, number))
        {
            localStatus = "A complete finite number is required.";
            return;
        }
        // 运行时页的 sun 滑条是 0..10 的双向 data binding；即使编辑器可见时那一页被隐藏，
        // 超范围值仍会被它截断并回写 SetSunIntensity。这里按同一区间校验并明确提示，
        // 而不是无声地提交一个随后被第三个控件改写的值。
        if (number < SunIntensityMin || number > SunIntensityMax)
        {
            localStatus = "Sun intensity must be between " + SceneActorNumber(SunIntensityMin) +
                " and " + SceneActorNumber(SunIntensityMax) + ".";
            return;
        }
        const EditorSceneActorSnapshot* actor = SelectedActor();
        if (actor != nullptr && !ChangedFloat(number, actor->light.intensity)) return;
        UiAction request;
        request.type = UiActionType::SetSunIntensity;
        request.floatValue = number;
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

void MaterialInstanceAssetEditorPanel::OpenBrowserAsset(const std::string& path)
{
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
        docking.Activate("details");
        EditorCommandEnvelope command;
        command.type = EditorCommandType::OpenMaterialInstanceAsset;
        command.payload = OpenMaterialInstanceAssetPayload{path, std::nullopt};
        Submit(std::move(command));
    }
    else if (kind == EditorAssetKind::Mesh || kind == EditorAssetKind::Texture ||
        kind == EditorAssetKind::Terrain)
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

void MaterialInstanceAssetEditorPanel::ProcessEvent(Rml::Event& event)
{
    if (refreshingControls || document == nullptr) return;
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
        if (activatedPanel == "viewport") ReturnToLevel();
        const std::string path = docking.ResourcePathForPanel(activatedPanel);
        if (!path.empty() && resourceEditor.Activate(path))
        {
            RequestActiveResourceMetadata();
            resourceEditorVisible = true;
            const auto* active = resourceEditor.ActiveTab();
            if (active != nullptr && active->kind == EditorAssetKind::Mesh && !HasCurrentModelPreview())
                RequestModelPreview(path);
            RefreshResourceEditor();
        }
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
    while (element != nullptr && element != document && !element->HasAttribute("data-action"))
        element = element->GetParentNode();
    if (element == nullptr || element == document || element->HasAttribute("disabled")) return;
    const std::string action = element->GetAttribute<std::string>("data-action", "");
    if (HasModal() && action != "close-save" && action != "close-discard" &&
        action != "close-cancel" && action != "about-close")
        return;
    // 选中菜单条目后收起菜单；标题自己决定展开还是收起，不能在这里先清空。
    if (action != "menu-open" && IsInsideMenuBar(element)) CloseMenus();
    try
    {
        std::optional<std::string> openAssetPath;
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
        else if (event.GetType() == "change") HandleChange(*element, action);
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
