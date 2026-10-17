#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include "editor/scene/scriptBlueprintConfiguration.h"
#include "engine/blueprintPreviewRequest.h"
#include "editor/ui/scriptComponentControls.h"
#include "editor/ui/scriptPhysicsControls.h"
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptPropertySchema.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
using Json = nlohmann::json;
using namespace UiWidgets;
const std::string Prefix = "blueprint-asset-";

std::string ControlText(Rml::Element& element)
{
    const auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    return control ? control->GetValue() : "";
}

double BlueprintNumber(const std::string& text)
{
    size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value)) throw std::invalid_argument("A complete finite number is required");
    return value;
}

Json BlueprintFields(const Json& classes, const Json& draft)
{
    for (const auto& type : classes)
        if (ScriptTypeNameMatches(type, draft.at("class").get<std::string>())) return type.at("properties");
    return Json::array();
}

std::string ScalarText(const Json& value)
{
    return value.is_string() ? value.get<std::string>() : value.dump();
}

bool AllowsInstance(const Json& draft, const Json& name)
{
    const auto allowed = draft.value("instanceEditable", Json::array());
    return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
}

std::string BlueprintStructure(const Json& draft, const Json& fields)
{
    Json structure{{"class", draft.at("class")}, {"fields", fields}, {"nodes", Json::array()}};
    for (const auto& node : draft.value("components", Json::array()))
        structure["nodes"].push_back({{"id", node.value("id", "root")},
            {"type", node.at("type")}, {"parent", node.value("parent", "")}});
    for (const auto& node : draft.value("scriptComponents", Json::array()))
        structure["logic"].push_back({{"id", node.at("id")}, {"class", node.at("class")}});
    const auto physics = draft.value("physics", Json::object());
    structure["motion"] = physics.value("motion", "none");
    structure["shape"] = physics.value("shape", "box");
    structure["constraints"] = Json::array();
    for (const auto& joint : physics.value("constraints", Json::array()))
        structure["constraints"].push_back({{"id", joint.at("id")}, {"type", joint.at("type")}});
    return structure.dump();
}

// 组件选择只属于编辑页会话；行索引从当前冻结文档解析，不进入资产 JSON。
std::string BlueprintComponentTree(const Json& nodes, const std::string& parent = {}, uint32_t depth = 1)
{
    std::string body;
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        const auto& node = nodes[index];
        if (node.value("parent", "") != parent) continue;
        const auto id = node.value("id", "root");
        TreeRowData row;
        row.label = id;
        row.depth = depth;
        row.rowClass = "blueprint-component-row";
        row.select = {"blueprint-asset-select-component", index};
        row.icon = node.at("type") == "mesh" ? "icons/mesh.png" :
            node.at("type") == "camera" ? "icons/camera.png" : "icons/blueprint.png";
        body += "<div id=\"blueprint-asset-tree-" + std::to_string(index) + "\">" + TreeRow(row) + "</div>";
        body += BlueprintComponentTree(nodes, id, depth + 1);
    }
    return body;
}
} // namespace

const EditorAssetDocumentResultPayload* MaterialInstanceAssetEditorPanel::BlueprintDocument(const std::string& path) const
{
    for (const auto& entry : snapshot.blueprintDocuments)
        if (entry.assetPath == path) return &entry;
    return nullptr;
}

void MaterialInstanceAssetEditorPanel::QueueBlueprintAssetDraft(Json candidate)
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    if (!authored || runtimeView.scripts.state != ScriptPlayState::Editing) return;
    if (!pendingBlueprintPath.empty() && pendingBlueprintPath != tab->path) FlushBlueprintAssetEdits();
    // 同帧修改在 producer 的值副本上合成，下一次 Build 只提交一个 revision-gated 候选。
    pendingBlueprintPath = tab->path;
    pendingBlueprintRevision = authored->documentRevision;
    pendingBlueprintDraft = std::move(candidate);
}

void MaterialInstanceAssetEditorPanel::FlushBlueprintAssetEdits()
{
    if (pendingBlueprintPath.empty()) return;
    const auto* authored = BlueprintDocument(pendingBlueprintPath);
    ModifyAssetDocumentPayload payload;
    payload.assetPath = pendingBlueprintPath;
    if (authored)
        for (const char* field : {"name", "description", "class", "properties", "instanceEditable", "componentInstanceEditable",
            "components", "scriptComponents", "physics", "rotation", "scale"})
        {
            if (authored->document.contains(field) == pendingBlueprintDraft.contains(field) &&
                (!pendingBlueprintDraft.contains(field) || authored->document.at(field) == pendingBlueprintDraft.at(field))) continue;
            EditorAssetFieldChange change;
            change.field = field;
            change.reset = !pendingBlueprintDraft.contains(field);
            if (!change.reset)
            {
                const auto& value = pendingBlueprintDraft.at(field);
                if (value.is_string()) change.value = value.get<std::string>();
                else if (std::string(field) == "rotation" || std::string(field) == "scale")
                    change.value = value.get<std::array<double, 3>>();
                else change.value = value;
            }
            payload.changes.push_back(std::move(change));
        }
    if (!payload.changes.empty())
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::ModifyBlueprintDocument;
        command.expectedDocumentRevision = pendingBlueprintRevision;
        command.payload = std::move(payload);
        Submit(std::move(command));
        blueprintCommandAwaitingSnapshot = true;
        blueprintCommandRevision = pendingBlueprintRevision;
        blueprintCommandPath = pendingBlueprintPath;
        for (const auto& feedback : snapshot.blueprintCommandFeedback)
            blueprintCommandFeedbackSequence = std::max(blueprintCommandFeedbackSequence, feedback.sequence);
    }
    pendingBlueprintPath.clear();
    pendingBlueprintDraft = nullptr;
}

void MaterialInstanceAssetEditorPanel::SubmitBlueprintAssetCommand(EditorCommandType type, const std::string& path)
{
    const auto* authored = BlueprintDocument(path);
    if (!authored) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = authored->documentRevision;
    if (type == EditorCommandType::ReloadBlueprintDocument)
        command.payload = ReloadMaterialInstanceDocumentPayload{path, EditorDirtyDocumentPolicy::RequireClean};
    else command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::RequestBlueprintPreview()
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    if (!authored || !runtimeActionSink || !runtimeView.scripts.available ||
        runtimeView.scripts.state != ScriptPlayState::Editing || runtimeView.offlineMode) return;
    previewDragButton = -1;
    CancelBlueprintViewportDrag();
    if (!runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() != "SC_model_preview.json")
        levelWorldPath = runtimeView.activeWorldPath;
    requestedPreviewPath = tab->path;
    requestedPreviewBlueprint = true;
    requestedPreviewMaterial = false;
    requestedBlueprintPreviewRevision = authored->documentRevision;
    requestedPreviewGeneration = runtimeView.scripts.worldGeneration;
    for (const auto& actor : OutlinerActors())
        requestedPreviewGeneration = std::max(requestedPreviewGeneration, actor.worldGeneration);
    blueprintPreviewVisualSignature = BlueprintPreviewVisualSignature(authored->document);
    blueprintPreviewPending = false;
    UiAction action;
    action.type = UiActionType::PreviewBlueprint;
    action.stringValue = tab->path;
    action.blueprintPreviewRevision = authored->documentRevision;
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentBlueprintPreviewGeneration() const
{
    const auto* tab = resourceEditor.ActiveTab();
    if (!requestedPreviewBlueprint || !tab || tab->kind != EditorAssetKind::Blueprint ||
        tab->path != requestedPreviewPath) return 0;
    const auto identity = "blueprintInstance|" + BlueprintPreviewIdentity(tab->path, requestedBlueprintPreviewRevision);
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration && actor.identity == identity &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
            return actor.worldGeneration;
    return 0;
}

void MaterialInstanceAssetEditorPanel::RefreshBlueprintPreview()
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    if (!authored) return;
    const bool visible = IsResourceEditorVisible();
    const bool allowed = runtimeView.scripts.available && !runtimeView.offlineMode &&
        runtimeView.scripts.state == ScriptPlayState::Editing;
    if (visible && allowed && (!requestedPreviewBlueprint || requestedPreviewPath != tab->path))
        RequestBlueprintPreview();
    else if (visible && allowed && pendingBlueprintPath.empty())
    {
        const auto signature = BlueprintPreviewVisualSignature(authored->document);
        const double now = Rml::GetSystemInterface()->GetElapsedTime();
        if (blueprintPreviewVisualSignature != signature)
        {
            blueprintPreviewVisualSignature = signature;
            blueprintPreviewChangeTime = now;
            blueprintPreviewPending = true;
        }
        // 只预览权威草稿，连续组件输入稳定后再准备 World，避免逐键同步导入资源。
        if (blueprintPreviewPending && now - blueprintPreviewChangeTime >= 0.35) RequestBlueprintPreview();
    }
    const auto generation = CurrentBlueprintPreviewGeneration();
    widgetDocument.SetProperty("blueprint-asset-preview-image", "display", generation ? "block" : "none");
    widgetDocument.SetProperty("blueprint-asset-preview-status", "display", generation ? "none" : "block");
    SetDisabled("blueprint-asset-preview-frame", !generation);
    SetDisabled("blueprint-asset-preview-refresh", !allowed || !pendingBlueprintPath.empty());
    SetMarkup("blueprint-asset-preview-status", Text(!allowed ? "Blueprint preview unavailable" :
        runtimeView.scripts.feedback.empty() ? "Loading Blueprint preview..." : runtimeView.scripts.feedback));
}

void MaterialInstanceAssetEditorPanel::RefreshBlueprintAssetEditor()
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    if (!authored)
    {
        SetMarkup("blueprint-asset-editor", PropertyInfo("Blueprint", snapshot.statusMessage.empty()
            ? "Reading Blueprint document..." : snapshot.statusMessage));
        blueprintAssetIdentity.clear();
        return;
    }
    const auto& draft = authored->document;
    const auto fields = BlueprintFields(runtimeView.scripts.classes, draft);
    const auto identity = tab->path + BlueprintStructure(draft, fields) + runtimeView.scripts.componentClasses.dump();
    EditorSceneActorSnapshot actor;
    actor.scriptComponents = draft.value("components", Json::array());
    actor.scriptComponentInstanceEditable = draft.value("componentInstanceEditable", Json::object());
    if (blueprintAssetSelectionPath != tab->path)
    {
        CancelBlueprintViewportDrag();
        blueprintAssetSelectionPath = tab->path;
        blueprintAssetSelectedComponent.clear();
        blueprintAssetSelectedLogicComponent.clear();
    }
    bool selectedExists = false;
    for (const auto& node : actor.scriptComponents)
        if (node.value("id", "root") == blueprintAssetSelectedComponent) selectedExists = true;
    if (!selectedExists) blueprintAssetSelectedComponent.clear();
    selectedExists = false;
    for (const auto& node : draft.value("scriptComponents", Json::array()))
        if (node.at("id") == blueprintAssetSelectedLogicComponent) selectedExists = true;
    if (!selectedExists) blueprintAssetSelectedLogicComponent.clear();
    if (identity != blueprintAssetIdentity)
    {
        blueprintAssetIdentity = identity;
        TreeRowData root;
        root.label = "Blueprint (Self)";
        root.icon = "icons/blueprint.png";
        root.select = {"blueprint-asset-select-defaults"};
        std::string body = "<div class=\"blueprint-asset-header\"><h2 id=\"blueprint-asset-title\"></h2>"
            "<p class=\"blueprint-asset-path\">" + Text(tab->path) + "</p>" +
            "<p id=\"blueprint-asset-status\" class=\"notice\"></p></div>" +
            "<div class=\"blueprint-asset-workspace\"><div id=\"blueprint-asset-components\">" +
            "<div class=\"blueprint-asset-panel-title\">Components</div>" +
            "<div class=\"blueprint-asset-add-tools\">" +
            Select(Prefix + "component-kind", {Prefix + "component-kind"},
                ScriptComponentOptions()) +
            "<button id=\"blueprint-asset-component-add\" class=\"browser-icon\" data-action=\"blueprint-asset-component-add\" "
            "title=\"Add component\"><img src=\"icons/plus.png\"/></button></div>" +
            "<div class=\"blueprint-asset-mesh-path\"><label>Mesh Asset</label>" +
            Input(Prefix + "component-mesh", {Prefix + "component-mesh"}, "") + "</div>" +
            "<div id=\"blueprint-asset-component-tree\"><div id=\"blueprint-asset-tree-root\">" + TreeRow(root) +
            "</div>" + BlueprintComponentTree(actor.scriptComponents) + "</div>" + BlueprintLogicTree(draft) + "</div>" +
            "<div id=\"blueprint-asset-preview\"><div class=\"blueprint-asset-preview-toolbar\">"
            "<button id=\"blueprint-asset-tool-select\" class=\"browser-icon\" data-action=\"blueprint-asset-tool\" data-tool=\"select\" title=\"Select component\"><img src=\"icons/search.png\"/><span class=\"browser-tooltip\">Select Component</span></button>"
            "<button id=\"blueprint-asset-tool-move\" class=\"browser-icon\" data-action=\"blueprint-asset-tool\" data-tool=\"move\" title=\"Translate in parent space\"><img src=\"icons/arrow-up.png\"/><span class=\"browser-tooltip\">Move in Parent Axes</span></button>"
            "<button id=\"blueprint-asset-tool-rotate\" class=\"browser-icon\" data-action=\"blueprint-asset-tool\" data-tool=\"rotate\" title=\"Rotate local Euler axes\"><img src=\"icons/refresh.png\"/><span class=\"browser-tooltip\">Rotate Local Euler Axes</span></button>"
            "<button id=\"blueprint-asset-tool-scale\" class=\"browser-icon\" data-action=\"blueprint-asset-tool\" data-tool=\"scale\" title=\"Scale local axes\"><img src=\"icons/layout-grid.png\"/><span class=\"browser-tooltip\">Scale Local Axes</span></button>"
            "<button id=\"blueprint-asset-preview-frame\" class=\"browser-icon\" data-action=\"blueprint-asset-preview-frame\" "
            "title=\"Frame Blueprint\"><img src=\"icons/camera.png\"/><span class=\"browser-tooltip\">Frame Blueprint</span></button>"
            "<button id=\"blueprint-asset-preview-refresh\" class=\"browser-icon\" data-action=\"blueprint-asset-preview-refresh\" "
            "title=\"Refresh preview\"><img src=\"icons/refresh.png\"/><span class=\"browser-tooltip\">Refresh Preview</span></button></div>"
            "<div class=\"blueprint-snap-toolbar\"><label><input id=\"blueprint-snap-enabled\" type=\"checkbox\" data-action=\"blueprint-asset-snap-enabled\"/><span>Snap</span></label>"
            "<select id=\"blueprint-snap-move\" data-action=\"blueprint-asset-snap-move\" title=\"Translation snap in centimeters\"><option value=\"1\">1 cm</option><option value=\"10\" selected=\"selected\">10 cm</option><option value=\"50\">50 cm</option><option value=\"100\">100 cm</option></select>"
            "<select id=\"blueprint-snap-rotate\" data-action=\"blueprint-asset-snap-rotate\" title=\"Rotation snap in degrees\"><option value=\"5\">5 deg</option><option value=\"15\" selected=\"selected\">15 deg</option><option value=\"45\">45 deg</option><option value=\"90\">90 deg</option></select>"
            "<select id=\"blueprint-snap-scale\" data-action=\"blueprint-asset-snap-scale\" title=\"Scale snap relative to starting scale\"><option value=\"0.05\">5%</option><option value=\"0.1\" selected=\"selected\">10%</option><option value=\"0.25\">25%</option><option value=\"0.5\">50%</option></select></div>"
            "<div id=\"blueprint-asset-preview-surface\"><img id=\"blueprint-asset-preview-image\" src=\"vl://scene-viewport\"/>"
            "<div id=\"blueprint-asset-gizmo-overlay\"></div>"
            "<p id=\"blueprint-asset-preview-status\" class=\"notice\"></p></div></div>" +
            "<div id=\"blueprint-asset-details\"><div class=\"blueprint-asset-panel-title\" id=\"blueprint-asset-details-title\"></div>" +
            "<div id=\"blueprint-asset-details-scroll\" class=\"inspector-body\"><div id=\"blueprint-asset-defaults\"><h3>Class Defaults</h3>";
        body += PropertyControlRow("Name", Input(Prefix + "name", {Prefix + "name"}, draft.at("name")));
        body += PropertyControlRow("Description", Input(Prefix + "description", {Prefix + "description"},
            draft.value("description", "")));
        std::string options = Option(draft.at("class"), draft.at("class"));
        for (const auto& type : runtimeView.scripts.classes)
            if (!ScriptTypeNameMatches(type, draft.at("class").get<std::string>()))
                options += Option(type.at("name"), type.at("name"));
        body += PropertyControlRow("Class", Select(Prefix + "class", {Prefix + "class"}, options));
        for (const char* field : {"rotation", "scale"})
        {
            std::vector<NumericComponent> controls;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                NumericComponent control;
                control.id = Prefix + field + "-" + std::to_string(axis);
                control.label = std::string(1, "XYZ"[axis]);
                control.change = {Prefix + field, 0, static_cast<uint32_t>(axis)};
                control.value = std::string(field) == "scale" ? "1" : "0";
                controls.push_back(std::move(control));
            }
            body += PropertyControlRow(field, NumericComponents(controls));
        }
        for (size_t index = 0; index < fields.size(); ++index)
        {
            const auto& field = fields[index];
            const auto id = Prefix + "property-" + std::to_string(index);
            const auto value = CompleteScriptPropertyValue(draft, field);
            body += "<div class=\"actor-property-row\">" + ScriptPropertyRow(field.at("name"), id,
                {Prefix + "property", index}, field, value,
                "<button id=\"" + Prefix + "reset-" + std::to_string(index) +
                "\" class=\"browser-icon\" data-action=\"blueprint-asset-reset\" data-index=\"" +
                std::to_string(index) + "\" title=\"Restore class default\"><img src=\"icons/refresh.png\"/></button>") + "</div>";
            body += PropertyControlRow("Instance Editable", "<input id=\"" + Prefix + "instance-" +
                std::to_string(index) + "\" type=\"checkbox\" data-action=\"blueprint-asset-instance\" data-index=\"" +
                std::to_string(index) + "\"/>");
        }
        body += "<h3>Physics</h3>" + ScriptPhysicsControls(Prefix, draft.value("physics", Json::object()));
        body += "</div>" + ScriptComponentControls(actor, {}, 0, Prefix, true) +
            BlueprintLogicDetails(draft) + "</div></div></div>";
        SetMarkup("blueprint-asset-editor", std::move(body));
    }
    SetMarkup(Prefix + "title", Text(draft.at("name")));
    SetMarkup(Prefix + "details-title", Text(blueprintAssetSelectedComponent.empty()
        ? (blueprintAssetSelectedLogicComponent.empty() ? "Details - Blueprint" : "Details - " + blueprintAssetSelectedLogicComponent)
        : "Details - " + blueprintAssetSelectedComponent));
    const bool selfSelected = blueprintAssetSelectedComponent.empty() && blueprintAssetSelectedLogicComponent.empty();
    widgetDocument.SetProperty(Prefix + "defaults", "display", selfSelected ? "block" : "none");
    if (auto* root = document->QuerySelector("#blueprint-asset-tree-root .list-item"))
        if (root->IsClassSet("selected") != selfSelected) root->SetClass("selected", selfSelected);
    for (size_t index = 0; index < actor.scriptComponents.size(); ++index)
    {
        const bool selected = actor.scriptComponents[index].value("id", "root") == blueprintAssetSelectedComponent;
        const auto suffix = std::to_string(index);
        widgetDocument.SetProperty(Prefix + "component-details-" + suffix, "display", selected ? "block" : "none");
        if (auto* row = document->QuerySelector("#blueprint-asset-tree-" + suffix + " .list-item"))
            if (row->IsClassSet("selected") != selected) row->SetClass("selected", selected);
    }
    const bool disabled = !runtimeView.scripts.available || runtimeView.scripts.state != ScriptPlayState::Editing;
    RefreshBlueprintLogicComponents(draft, disabled);
    SetMarkup(Prefix + "status", Text(snapshot.statusMessage));
    for (const char* field : {"name", "description", "class"})
    {
        SetControlValue(Prefix + field, draft.value(field, ""));
        SetDisabled(Prefix + field, disabled);
    }
    for (const char* field : {"rotation", "scale"})
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const auto id = Prefix + field + "-" + std::to_string(axis);
            SetControlValue(id, draft.contains(field) ? draft.at(field)[axis].dump() : std::string(field) == "scale" ? "1" : "0");
            SetDisabled(id, disabled);
        }
    for (size_t index = 0; index < fields.size(); ++index)
    {
        const auto& field = fields[index];
        const auto name = field.at("name").get<std::string>();
        const auto value = CompleteScriptPropertyValue(draft, field);
        RefreshScriptPropertyControl(*document, widgetDocument, Prefix + "property-" + std::to_string(index),
            {Prefix + "property", index}, field, value, disabled, false, Json::array(),
            draft.at("properties").value(name, Json(nullptr)));
        SetDisabled(Prefix + "reset-" + std::to_string(index), disabled || !draft.at("properties").contains(name));
        const auto id = Prefix + "instance-" + std::to_string(index);
        if (auto* input = document->GetElementById(id))
        {
            if (AllowsInstance(draft, name)) { if (!input->HasAttribute("checked")) input->SetAttribute("checked", "checked"); }
            else if (input->HasAttribute("checked")) input->RemoveAttribute("checked");
        }
        SetDisabled(id, disabled);
    }
    for (const char* suffix : {"component-kind", "component-mesh", "component-add"}) SetDisabled(Prefix + suffix, disabled);
    for (size_t index = 0; index < actor.scriptComponents.size(); ++index)
    {
        const auto& node = actor.scriptComponents[index];
        for (const auto& field : ScriptComponentInstanceFields(node))
        {
            const auto id = ComponentInstanceControlId(index, field, Prefix);
            if (auto* input = document->GetElementById(id))
            {
                const bool checked = ScriptComponentAllowsInstance(actor.scriptComponentInstanceEditable,
                    ScriptComponentId(node), field);
                if (checked && !input->HasAttribute("checked")) input->SetAttribute("checked", "checked");
                else if (!checked && input->HasAttribute("checked")) input->RemoveAttribute("checked");
            }
            SetDisabled(id, disabled);
        }
        SetDisabled(Prefix + "component-remove-" + std::to_string(index), disabled);
        RefreshScriptCollisionControls(index, node, Prefix, disabled);
        for (const char* field : {"parent", "modelPath", "materialOverride", "fov", "near_clip", "far_clip", "autoActivate",
            "position", "rotation", "scale", "color", "intensity", "visible", "coneAngleInner", "coneAngleOuter"})
        {
            const std::string key = field;
            const bool vector = key == "position" || key == "rotation" || key == "scale" || key == "color";
            for (size_t axis = 0; axis < (vector ? 3u : 1u); ++axis)
            {
                const auto id = ComponentControlId(index, field, axis, Prefix);
                if (!document->GetElementById(id)) continue;
                Json value = vector ? (node.contains(field) ? node.at(field)[axis] : Json(key == "scale" || key == "color" ? 1 : 0))
                    : node.value(field, Json(key == "visible" ? Json(true) : IsScriptLightNumber(key) ? Json(ScriptLightDefault(key)) :
                        std::string(field) == "autoActivate" ? Json(false) :
                        std::string(field) == "fov" ? Json(60) : std::string(field) == "near_clip" ? Json(1) :
                        std::string(field) == "far_clip" ? Json(10000) : Json("")));
                SetControlValue(id, ScalarText(value));
                SetDisabled(id, disabled);
            }
        }
    }
    const auto physics = draft.value("physics", Json::object());
    const auto motion = physics.value("motion", "none"), shape = physics.value("shape", "box");
    SetControlValue(Prefix + "physics-motion", motion);
    SetControlValue(Prefix + "physics-shape", shape);
    SetDisabled(Prefix + "physics-motion", disabled);
    SetDisabled(Prefix + "physics-shape", disabled || motion == "character");
    SetDisabled(Prefix + "physics-trigger", disabled || motion == "character");
    if (auto* trigger = document->GetElementById(Prefix + "physics-trigger"))
    {
        if (physics.value("trigger", false)) { if (!trigger->HasAttribute("checked")) trigger->SetAttribute("checked", "checked"); }
        else if (trigger->HasAttribute("checked")) trigger->RemoveAttribute("checked");
    }
    widgetDocument.SetProperty(Prefix + "physics-body", "display", motion == "none" ? "none" : "block");
    SetControlValue(Prefix + "physics-component", physics.value("component", ""));
    SetDisabled(Prefix + "physics-component", disabled);
    widgetDocument.SetProperty(Prefix + "physics-component-row", "display", shape == "mesh" ? "block" : "none");
    RefreshScriptConstraintControls(physics, Prefix, disabled || motion == "character");
    widgetDocument.SetProperty(Prefix + "physics-halfExtents-row", "display", shape == "box" ? "block" : "none");
    for (size_t axis = 0; axis < 3; ++axis)
    {
        const auto id = Prefix + "physics-extents-" + std::to_string(axis);
        SetControlValue(id, physics.contains("halfExtents") ? physics.at("halfExtents")[axis].dump() : "50");
        SetDisabled(id, disabled);
    }
    for (const auto& field : ScriptPhysicsFields())
    {
        const auto id = Prefix + "physics-" + field.name;
        SetControlValue(id, physics.value(field.name, Json(std::string(field.name) == "mass" && motion == "character"
            ? 70 : field.defaultValue)).dump());
        SetDisabled(id, disabled);
        widgetDocument.SetProperty(id + "-row", "display",
            ScriptPhysicsFieldVisible(physics, field.name) ? "block" : "none");
    }
    SetDisabled("blueprint-asset-save", disabled || (!authored->dirty && pendingBlueprintPath.empty()));
    SetDisabled("blueprint-asset-revert", disabled || (!authored->dirty && pendingBlueprintPath.empty()));
    RefreshBlueprintPreview();
    RefreshBlueprintViewport();
    RefreshBlueprintHistoryControls();
}

void MaterialInstanceAssetEditorPanel::HandleBlueprintAssetEvent(Rml::Element& element,
    const std::string& action, bool click)
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? BlueprintDocument(tab->path) : nullptr;
    if (!authored || tab->kind != EditorAssetKind::Blueprint) return;
    if (action.rfind(Prefix + "logic-", 0) == 0)
    {
        auto draft = pendingBlueprintPath == tab->path ? pendingBlueprintDraft : authored->document;
        HandleBlueprintLogicEvent(element, action, click, draft);
        return;
    }
    if (action.rfind(Prefix + "snap-", 0) == 0)
    {
        if (!click)
        {
            CancelBlueprintViewportDrag();
            if (action == Prefix + "snap-enabled") blueprintSnapEnabled = element.HasAttribute("checked");
            else
            {
                const auto text = ControlText(element);
                if (action == Prefix + "snap-move" &&
                    (text == "1" || text == "10" || text == "50" || text == "100"))
                    blueprintMoveSnap = static_cast<float>(BlueprintNumber(text));
                if (action == Prefix + "snap-rotate" &&
                    (text == "5" || text == "15" || text == "45" || text == "90"))
                    blueprintRotateSnap = static_cast<float>(BlueprintNumber(text));
                if (action == Prefix + "snap-scale" &&
                    (text == "0.05" || text == "0.1" || text == "0.25" || text == "0.5"))
                    blueprintScaleSnap = static_cast<float>(BlueprintNumber(text));
            }
        }
        return;
    }
    if (click && action == Prefix + "tool")
    {
        CancelBlueprintViewportDrag();
        const auto tool = element.GetAttribute<std::string>("data-tool", "");
        if (tool == "select" || tool == "move" || tool == "rotate" || tool == "scale")
            blueprintViewportTool = tool;
        RefreshBlueprintViewport();
        return;
    }
    if (click && action == Prefix + "preview-frame")
    {
        RequestPreviewCamera(ModelPreviewNavigation::Frame);
        return;
    }
    if (click && action == Prefix + "preview-refresh")
    {
        RequestBlueprintPreview();
        return;
    }
    if (click && (action == Prefix + "select-defaults" || action == Prefix + "select-component"))
    {
        CancelBlueprintViewportDrag();
        blueprintAssetSelectedComponent.clear();
        blueprintAssetSelectedLogicComponent.clear();
        if (action == Prefix + "select-component")
        {
            const auto nodes = authored->document.value("components", Json::array());
            const auto index = element.GetAttribute<size_t>("data-index", nodes.size());
            if (index >= nodes.size()) return;
            blueprintAssetSelectedComponent = nodes[index].value("id", "root");
        }
        // 切换只改变既有表单可见性，保留未完成输入与文档 revision。
        if (auto* scroll = document->GetElementById(Prefix + "details-scroll")) scroll->SetScrollTop(0);
        return;
    }
    if (click && action == Prefix + "property")
    {
        const auto operation = element.GetAttribute<std::string>("data-script-operation", "");
        if (operation == "toggle" || operation == "previous" || operation == "next")
        {
            const auto& source = pendingBlueprintPath == tab->path ? pendingBlueprintDraft : authored->document;
            const auto fields = BlueprintFields(runtimeView.scripts.classes, source);
            const auto index = element.GetAttribute<size_t>("data-index", fields.size());
            if (index < fields.size())
            {
                Json request;
                ReadScriptCompositeEvent(element, fields[index],
                    CompleteScriptPropertyValue(source, fields[index]), true, request);
            }
            return;
        }
    }
    if (!runtimeView.scripts.available ||
        runtimeView.scripts.state != ScriptPlayState::Editing) return;
    if (action == "blueprint-asset-save" && click) { OpenSaveDialog(tab->path); return; }
    if (action == "blueprint-asset-revert" && click)
    {
        pendingBlueprintPath.clear(); pendingBlueprintDraft = nullptr;
        SubmitBlueprintAssetCommand(EditorCommandType::RevertBlueprintDocument, tab->path);
        return;
    }
    Json draft = pendingBlueprintPath == tab->path ? pendingBlueprintDraft : authored->document;
    const auto fields = BlueprintFields(runtimeView.scripts.classes, draft);
    const auto field = action.substr(Prefix.size());
    const auto index = element.GetAttribute<size_t>("data-index", 0);
    const auto axis = element.GetAttribute<size_t>("data-component", 0);
    const auto text = ControlText(element);
    if (!click && (field == "name" || field == "description"))
    {
        if (field == "description" && text.empty()) draft.erase(field); else draft[field] = text;
    }
    else if (!click && field == "class")
    {
        if (draft.at("class") == text) return;
        draft["class"] = text;
        draft["properties"] = Json::object();
        draft["instanceEditable"] = Json::array();
    }
    else if (!click && (field == "rotation" || field == "scale") && axis < 3)
    {
        const auto value = BlueprintNumber(text);
        if ((draft.contains(field) ? draft.at(field)[axis].get<double>() : field == "scale" ? 1.0 : 0.0) == value)
            return;
        if (!draft.contains(field)) draft[field] = field == "scale" ? Json::array({1,1,1}) : Json::array({0,0,0});
        draft[field][axis] = value;
    }
    else if ((field == "property" || field == "instance" || field == "reset") && index < fields.size())
    {
        const auto& schema = fields[index];
        const auto name = schema.at("name").get<std::string>();
        if (field == "reset" && click) draft["properties"].erase(name);
        else if (field == "instance" && !click)
        {
            if (AllowsInstance(draft, name) == element.HasAttribute("checked")) return;
            auto allowed = draft.value("instanceEditable", Json::array());
            for (auto entry = allowed.begin(); entry != allowed.end();)
                if (*entry == name) entry = allowed.erase(entry); else ++entry;
            if (element.HasAttribute("checked")) allowed.push_back(name);
            draft["instanceEditable"] = std::move(allowed);
        }
        else if (field == "property" && ScriptPropertyIsComposite(schema))
        {
            const auto authored = draft.at("properties").value(name, Json(nullptr));
            const auto effective = CompleteScriptPropertyValue(draft, schema);
            Json request;
            const auto event = ReadScriptCompositeEvent(element, schema, effective, click, request);
            if (event != ScriptPropertyEvent::Edit) return;
            const auto updated = ApplyScriptPropertyEdit(schema, authored, effective, request);
            if (updated.is_null()) draft["properties"].erase(name);
            else draft["properties"][name] = updated;
        }
        else if (field == "property" && !click)
        {
            Json value = draft.at("properties").value(name, schema.at("value"));
            if (schema.at("type") == "quaternion")
            {
                if (!ParseScriptQuaternion(text, value)) return;
            }
            else if (value.is_array())
            {
                if (axis >= value.size()) return;
                value[axis] = BlueprintNumber(text);
            }
            else value = value.is_string() ? Json(text) :
                schema.at("type") == "bool" ? Json(text == "true") : Json::parse(text);
            if (value == draft.at("properties").value(name, schema.at("value"))) return;
            if (value == schema.at("value")) draft["properties"].erase(name);
            else draft["properties"][name] = value;
        }
        else return;
    }
    else if (field == "component-add" && click)
    {
        if (!draft.contains("components")) draft["components"] = Json::array();
        auto& nodes = draft["components"];
        for (auto& node : nodes) if (!node.contains("id")) node["id"] = "root";
        size_t suffix = nodes.size();
        std::string id;
        bool found;
        do
        {
            id = "component-" + std::to_string(++suffix);
            found = false;
            for (const auto& node : nodes) if (node.at("id") == id) found = true;
        } while (found);
        const auto kind = ControlText(*document->GetElementById(Prefix + "component-kind"));
        Json node = NewScriptComponent(kind);
        node["id"] = id;
        if (kind == "mesh") node["modelPath"] = ControlText(*document->GetElementById(Prefix + "component-mesh"));
        nodes.push_back(std::move(node));
    }
    else if (field == "component-remove" && click && index < draft.value("components", Json::array()).size())
    {
        auto& nodes = draft["components"];
        std::set<std::string> removed{nodes[index].value("id", "root")};
        size_t count;
        do
        {
            count = removed.size();
            for (const auto& node : nodes)
                if (removed.count(node.value("parent", ""))) removed.insert(node.value("id", "root"));
        } while (count != removed.size());
        for (auto node = nodes.begin(); node != nodes.end();)
            if (removed.count(node->value("id", "root"))) node = nodes.erase(node); else ++node;
        if (draft.contains("componentInstanceEditable"))
        {
            for (const auto& id : removed) draft["componentInstanceEditable"].erase(id);
            if (draft["componentInstanceEditable"].empty()) draft.erase("componentInstanceEditable");
        }
    }
    else if (field == "component-instance" && !click && index < draft.value("components", Json::array()).size())
    {
        const auto id = ScriptComponentId(draft["components"][index]);
        const auto name = element.GetAttribute<std::string>("data-field", "");
        auto permissions = draft.value("componentInstanceEditable", Json::object());
        if (ScriptComponentAllowsInstance(permissions, id, name) == element.HasAttribute("checked")) return;
        auto allowed = permissions.value(id, Json::array());
        for (auto item = allowed.begin(); item != allowed.end();)
            if (*item == name) item = allowed.erase(item); else ++item;
        if (element.HasAttribute("checked")) allowed.push_back(name);
        if (allowed.empty()) permissions.erase(id); else permissions[id] = std::move(allowed);
        if (permissions.empty()) draft.erase("componentInstanceEditable");
        else draft["componentInstanceEditable"] = std::move(permissions);
    }
    else if (field.rfind("component-", 0) == 0 && !click && index < draft.value("components", Json::array()).size())
    {
        const auto key = field.substr(10);
        auto& node = draft["components"][index];
        if (key.rfind("collision", 0) == 0)
        {
            Json request{{"field", key}, {"value", key == "collisionShape" ? Json(text) : Json(BlueprintNumber(text))}};
            if (key == "collisionHalfExtents") request["axis"] = axis;
            Editor::EditScriptComponentCollision(node, request);
        }
        else if (key == "parent" || key == "modelPath" || key == "materialOverride")
        {
            // select 的延迟 change 也会经过这里；等于有效值时不把缺省字段写成覆写。
            if (node.value(key, "") == text) return;
            if (key == "materialOverride" && text.empty()) node.erase(key); else node[key] = text;
        }
        else if (key == "autoActivate" || key == "visible")
        {
            if (node.value(key, key == "visible") == (text == "true")) return;
            node[key] = text == "true";
        }
        else if (key == "fov" || key == "near_clip" || key == "far_clip" || IsScriptLightNumber(key))
        {
            const auto value = BlueprintNumber(text);
            if (node.value(key, IsScriptLightNumber(key) ? double(ScriptLightDefault(key)) :
                key == "fov" ? 60.0 : key == "near_clip" ? 1.0 : 10000.0) == value) return;
            node[key] = value;
        }
        else if ((key == "position" || key == "rotation" || key == "scale" || key == "color") && axis < 3)
        {
            const bool ones = key == "scale" || key == "color";
            const auto value = BlueprintNumber(text);
            if ((node.contains(key) ? node.at(key)[axis].get<double>() : ones ? 1.0 : 0.0) == value)
                return;
            if (!node.contains(key)) node[key] = ones ? Json::array({1,1,1}) : Json::array({0,0,0});
            node[key][axis] = value;
        }
        else return;
    }
    else if (field.rfind("physics-constraint-", 0) == 0)
    {
        const auto key = field.substr(8);
        if (click && key == "constraint-add")
        {
            const auto* input = document->GetElementById(Prefix + "physics-constraint-target");
            auto* control = dynamic_cast<const Rml::ElementFormControl*>(input);
            const auto target = control ? control->GetValue() : "";
            if (target.empty()) { localStatus = "A target Blueprint ID is required."; return; }
            Editor::EditScriptPhysics(draft, {{"field", key}, {"value", target}});
        }
        else if (click && key == "constraint-remove")
            Editor::EditScriptPhysics(draft, {{"field", key}, {"value", index}});
        else if (!click && (key == "constraint-type" || key == "constraint-target" ||
            key == "constraint-anchor" || key == "constraint-axis" || key == "constraint-minAngle" || key == "constraint-maxAngle"))
        {
            Json request{{"field", key}, {"index", index},
                {"value", key == "constraint-type" || key == "constraint-target" ? Json(text) :
                    key == "constraint-axis" ? Json::parse(text) : Json(BlueprintNumber(text))}};
            if (key == "constraint-anchor") request["axis"] = axis;
            Editor::EditScriptPhysics(draft, request);
        }
        else return;
    }
    else if (field.rfind("physics-", 0) == 0 && !click)
    {
        const auto key = field.substr(8);
        Json value = key == "motion" || key == "shape" || key == "component" ? Json(text) :
            key == "trigger" ? Json(element.HasAttribute("checked")) : Json(BlueprintNumber(text));
        const auto physics = draft.value("physics", Json::object());
        if (key == "motion" && value == physics.value(key, "none")) return;
        if (key == "shape" && value == physics.value(key, "box")) return;
        if (key == "trigger" && value == physics.value(key, false)) return;
        if (key == "halfExtents" && axis < 3 &&
            value == (physics.contains(key) ? physics.at(key)[axis] : Json(50))) return;
        for (const auto& coefficient : ScriptPhysicsFields())
            if (key == coefficient.name && value == physics.value(key, Json(key == "mass" &&
                physics.value("motion", "none") == "character" ? 70 : coefficient.defaultValue))) return;
        Json request{{"field", key}, {"value", value}};
        if (key == "halfExtents") request["axis"] = axis;
        Editor::EditScriptPhysics(draft, request);
    }
    else return;
    QueueBlueprintAssetDraft(std::move(draft));
}

void MaterialInstanceAssetEditorPanel::RefreshScriptCollisionControls(size_t index, const Json& node,
    const std::string& prefix, bool disabled)
{
    const auto collision = node.value("collision", Json::object());
    const auto shape = collision.value("shape", "none");
    SetControlValue(ComponentControlId(index, "collisionShape", 0, prefix), shape);
    SetDisabled(ComponentControlId(index, "collisionShape", 0, prefix), disabled);
    for (const auto& field : std::vector<std::pair<std::string, std::string>>{
        {"collisionHalfExtents", "halfExtents"}, {"collisionRadius", "radius"}, {"collisionHalfHeight", "halfHeight"}})
    {
        const bool extents = field.second == "halfExtents";
        widgetDocument.SetProperty(ComponentControlId(index, field.first + "-row", 0, prefix), "display",
            (extents ? shape == "box" : field.second == "radius" ? shape == "sphere" || shape == "capsule" :
                shape == "capsule") ? "block" : "none");
        for (size_t axis = 0; axis < (extents ? 3u : 1u); ++axis)
        {
            const auto id = ComponentControlId(index, field.first, axis, prefix);
            SetControlValue(id, extents ? collision.value("halfExtents", Json::array({50, 50, 50}))[axis].dump() :
                collision.value(field.second, Json(field.second == "radius" ? 30 : 60)).dump());
            SetDisabled(id, disabled || shape == "none");
        }
    }
}

void MaterialInstanceAssetEditorPanel::RefreshScriptConstraintControls(const Json& physics,
    const std::string& prefix, bool disabled)
{
    SetDisabled(prefix + "physics-constraint-target", disabled);
    SetDisabled(prefix + "physics-constraint-add", disabled);
    const auto joints = physics.value("constraints", Json::array());
    for (size_t index = 0; index < joints.size(); ++index)
    {
        const auto& joint = joints[index];
        SetDisabled(prefix + "physics-constraint-remove-" + std::to_string(index), disabled);
        for (const char* field : {"type", "target", "anchor", "axis", "minAngle", "maxAngle"})
        {
            if (!joint.contains(field) && std::string(field) != "minAngle" && std::string(field) != "maxAngle") continue;
            const auto value = joint.value(field, Json(std::string(field) == "minAngle" ? -180 : 180));
            if (std::string(field) == "axis")
            {
                const auto id = ScriptConstraintControlId(prefix, index, field);
                SetControlValue(id, value.dump());
                SetDisabled(id, disabled);
                continue;
            }
            for (size_t axis = 0; axis < (value.is_array() ? 3u : 1u); ++axis)
            {
                const auto id = ScriptConstraintControlId(prefix, index, field, axis);
                const auto current = value.is_array() ? value[axis] : value;
                SetControlValue(id, ScalarText(current));
                SetDisabled(id, disabled);
            }
        }
    }
}
} // namespace VL::EditorUi
