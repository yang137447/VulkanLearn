#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
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

// 文档命令的可用性同时作用于工具栏按钮和 File/Edit 菜单项，避免出现两套互相矛盾的启用状态。
constexpr const char* DocumentCommandIds[] = {
    "save-asset", "revert-asset", "reset-asset", "validate-asset", "reload-asset",
    "menu-file-save", "menu-file-revert", "menu-file-reload",
    "menu-edit-reset", "menu-edit-validate"};

std::string Number(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
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
// 它的 0..10 双向 data binding 仍会把超范围值截断后回写一条新命令。
constexpr float SunIntensityMin = 0.0f;
constexpr float SunIntensityMax = 10.0f;

bool MeshActorHasOpenableSlot(const EditorSceneActorSnapshot& actor)
{
    for (const EditorSceneMeshSectionSnapshot& section : actor.sections)
        if (!section.materialInstancePath.empty()) return true;
    return false;
}

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
        "mode-realtime", "mode-offline", "save-asset", "revert-asset", "reset-asset",
        "validate-asset", "reload-asset", "apply-preview", "restore-preview",
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
        "outliner-search", "outliner-type", "outliner-count", "outliner-empty",
        "viewport-toolbar", "viewport-tool-select", "viewport-tool-move", "viewport-tool-rotate",
        "viewport-tool-scale", "viewport-space-world", "viewport-space-local",
        "viewport-view-lit", "viewport-view-unlit", "viewport-view-wireframe"})
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

void MaterialInstanceAssetEditorPanel::RefreshOutliner()
{
    const std::vector<EditorSceneActorSnapshot>& actors = OutlinerActors();
    // World 换代时展开集合、搜索与选中态都不再指向同一个对象，统一回到初始状态。
    const uint64_t generation = actors.empty() ? 0 : actors.front().worldGeneration;
    if (treeWorldGeneration != generation)
    {
        expandedActors.clear();
        collapsedOutlinerGroups.clear();
        outlinerQuery.clear();
        outlinerTypeFilter = "all";
        treeWorldGeneration = generation;
        SetControlValue("outliner-search", std::string());
        SetControlValue("outliner-type", outlinerTypeFilter);
    }

    const std::vector<EditorSceneActorRow> rows = BuildSceneActorRows(
        actors, outlinerQuery, outlinerTypeFilter);
    const auto& specs = SceneActorGroupSpecs();
    std::string markup;
    size_t visibleActors = 0;
    // 折叠只影响行是否挂进 DOM；计数始终是筛选结果（与 Content Browser 的计数同一语义）。
    bool groupExpanded = true;
    for (const EditorSceneActorRow& row : rows)
    {
        if (row.isGroup)
        {
            // 分组事件按规范表下标寻址；分组行的类型 -> 下标为此处的唯一下标来源。
            const size_t groupIndex = SceneActorGroupIndex(row.type);
            const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(row.type);
            if (spec == nullptr || groupIndex >= specs.size()) continue;
            groupExpanded = collapsedOutlinerGroups.count(spec->key) == 0;
            markup += UiWidgets::TreeRow(
                OutlinerGroupRow(*spec, groupIndex, row.actorCount, groupExpanded));
            continue;
        }
        ++visibleActors;
        if (!groupExpanded) continue;
        const EditorSceneActorSnapshot& actor = actors[row.actorIndex];
        const bool selected = actor.identity == snapshot.selectedActorIdentity;
        const bool hasSlots = actor.type == EditorSceneActorType::Mesh &&
            MeshActorHasOpenableSlot(actor);
        const bool expanded = expandedActors.count(actor.identity) != 0;
        markup += UiWidgets::TreeRow(OutlinerActorRow(row.actorIndex, actor, selected,
            expanded && hasSlots, hasSlots));
        if (!hasSlots || !expanded) continue;
        // 槽行与 MeshSceneActorModel 的 materials 同序：动作分量索引可以直接交给
        // 既有的 SelectModelMaterial，不需要第二份槽位表。
        const MaterialInstanceModelSnapshot model = MeshSceneActorModel(actor);
        for (size_t slotIndex = 0; slotIndex < model.materials.size(); ++slotIndex)
        {
            const MaterialInstanceModelMaterialSnapshot& slot = model.materials[slotIndex];
            const std::string label = slot.materialSlotName.empty()
                ? std::filesystem::path(slot.materialInstancePath).filename().string()
                : slot.materialSlotName;
            markup += UiWidgets::TreeRow(OutlinerSlotRow(row.actorIndex, slotIndex, label,
                selected && snapshot.selectedMaterialSlotIndex == slot.materialSlotIndex));
        }
    }
    SetMarkup("scene-models", std::move(markup));

    const size_t total = actors.size();
    SetMarkup("outliner-count", Text(visibleActors == total
        ? ActorCountLabel(total)
        : std::to_string(visibleActors) + " / " + ActorCountLabel(total)));
    const std::string emptyMessage = rows.empty()
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
    if (collapsedOutlinerGroups.erase(key) == 0) collapsedOutlinerGroups.insert(key);
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
    // 范围与运行时页的 sun 滑条一致（0..10）；滑条只影响拖动粒度，文本输入仍是精确值。
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
    bool alreadyOpen = false;
    for (const auto& tab : resourceEditor.Tabs())
        if (tab.path == path) { alreadyOpen = true; break; }
    if (!resourceEditor.Open(path, kind))
    {
        localStatus = "Too many resource editors are open.";
        return;
    }
    viewingJson = false;
    resourceEditorVisible = true;
    localStatus.clear();
    if (!alreadyOpen && jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::CloseActiveResource()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr) return;
    resourceEditor.Close(active->path);
    resourceEditorVisible = resourceEditor.ActiveTab() != nullptr;
    if (!resourceEditorVisible) docking.Activate("viewport");
    if (!resourceEditorVisible) localStatus.clear();
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::RequestResourceReload()
{
    resourceEditor.RequestReload();
    const std::string path = resourceEditor.ConsumeReloadRequest();
    if (path.empty()) return;
    if (jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
}

void MaterialInstanceAssetEditorPanel::RefreshResourceEditor()
{
    if (document == nullptr) return;
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr)
    {
        docking.SetResourceTabs({}, {});
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
    docking.SetResourceTabs(tabs, active->path);
    SetDisabled("resource-reload", active->loading);
    SetMarkup("resource-preview-title", Text(active->snapshot != nullptr &&
        !active->snapshot->name.empty() ? active->snapshot->name :
        std::filesystem::path(active->path).filename().string()));
    SetMarkup("resource-preview-kind", Text(EditorAssetKindName(active->kind)));
    SetMarkup("resource-preview-path", Text(active->path));
    SetMarkup("resource-preview-status", active->loading
        ? "Reading JSON metadata..."
        : "Preview unavailable: referenced data is not loaded.");
    auto* previewIcon = document->GetElementById("resource-preview-icon");
    const std::string icon = EditorAssetKindIcon(active->kind);
    if (previewIcon->GetAttribute<std::string>("src", "") != icon)
        previewIcon->SetAttribute("src", icon);
    resourceJsonInspector.Build(active->snapshot, active->path, active->loading);
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
        if (expandedActors.erase(identity) == 0) expandedActors.insert(identity);
    }
    else if (action == "outliner-actor" && index < OutlinerActors().size())
    {
        SelectOutlinerActor(OutlinerActors()[index]);
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
    else if (runtimeActionSink != nullptr)
    {
        UiAction request;
        if (action == "runtime-page") request.type = UiActionType::ToggleRuntimePage;
        else if (action == "hide-editor") request.type = UiActionType::ToggleDeveloperUi;
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
        docking.Activate("details");
        if (kind == EditorAssetKind::Scene && runtimeActionSink != nullptr)
        {
            // 关卡双击复用运行时场景选择器的 LoadWorld 事务；UI 不直接触碰 World，
            // 同时继续请求 JSON inspector，让用户能查看本次切换目标的元数据。
            UiAction request;
            request.type = UiActionType::LoadWorld;
            request.stringValue = path;
            runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        // 非关卡 JSON 只读查看，不导入模型或贴图；MI 文档事务保持独立。
        if (jsonAssetRequestSink == nullptr) return;
        viewingJson = true;
        requestedJsonPath = path;
        localStatus.clear();
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
        docking.Activate("details");
    }
}

void MaterialInstanceAssetEditorPanel::ProcessEvent(Rml::Event& event)
{
    if (refreshingControls || document == nullptr) return;
    // 点击菜单栏以外的任何位置都收起已展开的菜单；菜单项自身的收起在下面按 action 处理。
    if (event.GetType() == "click" && !openMenu.empty() && !IsInsideMenuBar(event.GetTargetElement()))
        CloseMenus();
    if (docking.ProcessEvent(event, HasModal()))
    {
        const std::string activatedPanel = docking.ConsumeActivatedPanel();
        const std::string path = docking.ResourcePathForPanel(activatedPanel);
        if (!path.empty() && resourceEditor.Activate(path))
        {
            resourceEditorVisible = true;
            RefreshResourceEditor();
        }
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
    }
    catch (const std::exception& error)
    {
        localStatus = error.what();
    }
    event.StopPropagation();
}

} // namespace VL::EditorUi
