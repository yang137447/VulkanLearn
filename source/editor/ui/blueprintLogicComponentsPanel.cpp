#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <cmath>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptPropertySchema.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"

namespace VL::EditorUi
{
namespace
{
using Json = nlohmann::json;
using namespace UiWidgets;
const std::string Prefix = "blueprint-asset-logic-";

std::string ControlText(Rml::Element& element)
{
    const auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    return control ? control->GetValue() : "";
}

Json LogicFields(const Json& classes, const Json& node)
{
    for (const auto& type : classes)
        if (ScriptTypeNameMatches(type, node.at("class").get<std::string>())) return type.at("properties");
    return Json::array();
}

std::string ControlId(size_t component, const std::string& field)
{
    return Prefix + field + "-" + std::to_string(component);
}

bool InstanceAllowed(const Json& node, const Json& name)
{
    const auto allowed = node.value("instanceEditable", Json::array());
    return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
}
}

std::string MaterialInstanceAssetEditorPanel::BlueprintLogicTree(const Json& draft) const
{
    std::string options;
    for (const auto& type : runtimeView.scripts.componentClasses)
        options += Option(type.at("name"), type.at("name"));
    std::string body = "<div class=\"blueprint-asset-panel-title\">C# Components</div>"
        "<div class=\"blueprint-asset-add-tools\">" +
        Select(Prefix + "add-class", {Prefix + "add-class"}, options) +
        "<button id=\"" + Prefix + "add\" class=\"browser-icon\" data-action=\"" +
        Prefix + "add\" title=\"Add C# component\"><img src=\"icons/plus.png\"/></button></div>";
    const auto nodes = draft.value("scriptComponents", Json::array());
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        TreeRowData row;
        row.label = nodes[index].at("id");
        row.icon = "icons/blueprint.png";
        row.depth = 1;
        row.select = {Prefix + "select", index};
        body += "<div id=\"" + ControlId(index, "tree") + "\">" + TreeRow(row) + "</div>";
    }
    return body;
}

std::string MaterialInstanceAssetEditorPanel::BlueprintLogicDetails(const Json& draft) const
{
    std::string body;
    const auto nodes = draft.value("scriptComponents", Json::array());
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        const auto& node = nodes[index];
        const auto fields = LogicFields(runtimeView.scripts.componentClasses, node);
        body += "<div id=\"" + ControlId(index, "details") + "\"><h3>C# Component</h3>";
        body += PropertyInfo("ID", node.at("id"));
        std::string options = Option(node.at("class"), node.at("class"));
        for (const auto& type : runtimeView.scripts.componentClasses)
            if (!ScriptTypeNameMatches(type, node.at("class").get<std::string>()))
                options += Option(type.at("name"), type.at("name"));
        body += PropertyControlRow("Class", Select(ControlId(index, "class"), {Prefix + "class", index}, options));
        body += PropertyControlRow("Enabled", Select(ControlId(index, "enabled"), {Prefix + "enabled", index},
            Option("true", "True") + Option("false", "False")));
        for (size_t property = 0; property < fields.size(); ++property)
        {
            const auto& field = fields[property];
            const auto value = CompleteScriptPropertyValue(node, field);
            const auto suffix = std::to_string(property);
            body += ScriptPropertyRow(field.at("name"), ControlId(index, "property-" + suffix),
                    {Prefix + "property-" + suffix, index}, field, value,
                "<button id=\"" + ControlId(index, "reset-" + suffix) +
                "\" class=\"browser-icon\" data-action=\"" + Prefix + "reset-" + suffix +
                "\" data-index=\"" + std::to_string(index) +
                "\" title=\"Restore class default\"><img src=\"icons/refresh.png\"/></button>");
            body += PropertyControlRow("Instance Editable", "<input id=\"" +
                ControlId(index, "instance-" + suffix) + "\" type=\"checkbox\" data-action=\"" +
                Prefix + "instance-" + suffix + "\" data-index=\"" + std::to_string(index) + "\"/>");
        }
        body += "<button id=\"" + ControlId(index, "remove") + "\" class=\"browser-icon\" data-action=\"" +
            Prefix + "remove\" data-index=\"" + std::to_string(index) +
            "\" title=\"Remove C# component\"><img src=\"icons/trash.png\"/></button></div>";
    }
    return body;
}

void MaterialInstanceAssetEditorPanel::RefreshBlueprintLogicComponents(const Json& draft, bool disabled)
{
    const auto nodes = draft.value("scriptComponents", Json::array());
    bool selectedExists = false;
    for (const auto& node : nodes)
        if (node.at("id") == blueprintAssetSelectedLogicComponent) selectedExists = true;
    if (!selectedExists) blueprintAssetSelectedLogicComponent.clear();
    SetDisabled(Prefix + "add", disabled || runtimeView.scripts.componentClasses.empty() || nodes.size() >= 128);
    SetDisabled(Prefix + "add-class", disabled || runtimeView.scripts.componentClasses.empty());
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        const auto& node = nodes[index];
        const bool selected = node.at("id") == blueprintAssetSelectedLogicComponent;
        widgetDocument.SetProperty(ControlId(index, "details"), "display", selected ? "block" : "none");
        if (auto* row = document->QuerySelector("#" + ControlId(index, "tree") + " .list-item"))
            if (row->IsClassSet("selected") != selected) row->SetClass("selected", selected);
        SetControlValue(ControlId(index, "class"), node.at("class"));
        SetControlValue(ControlId(index, "enabled"), node.value("enabled", true) ? "true" : "false");
        SetDisabled(ControlId(index, "class"), disabled);
        SetDisabled(ControlId(index, "enabled"), disabled);
        SetDisabled(ControlId(index, "remove"), disabled);
        const auto fields = LogicFields(runtimeView.scripts.componentClasses, node);
        for (size_t property = 0; property < fields.size(); ++property)
        {
            const auto& field = fields[property];
            const auto value = CompleteScriptPropertyValue(node, field);
            const auto suffix = std::to_string(property);
            const auto id = ControlId(index, "property-" + suffix);
            RefreshScriptPropertyControl(*document, widgetDocument, id,
                {Prefix + "property-" + suffix, index}, field, value, disabled, false, Json::array(),
                node.at("properties").value(field.at("name").get<std::string>(), Json(nullptr)));
            SetDisabled(ControlId(index, "reset-" + suffix),
                disabled || !node.at("properties").contains(field.at("name").get<std::string>()));
            const auto permission = ControlId(index, "instance-" + suffix);
            SetDisabled(permission, disabled);
            if (auto* input = document->GetElementById(permission))
            {
                const bool allowed = InstanceAllowed(node, field.at("name"));
                if (allowed && !input->HasAttribute("checked")) input->SetAttribute("checked", "checked");
                else if (!allowed && input->HasAttribute("checked")) input->RemoveAttribute("checked");
            }
        }
    }
}

bool MaterialInstanceAssetEditorPanel::HandleBlueprintLogicEvent(Rml::Element& element,
    const std::string& action, bool click, Json& draft)
{
    if (action.rfind(Prefix, 0) != 0) return false;
    auto nodes = draft.value("scriptComponents", Json::array());
    const size_t index = element.GetAttribute<size_t>("data-index", nodes.size());
    const auto field = action.substr(Prefix.size());
    if (field == "select" && click && index < nodes.size())
    {
        CancelBlueprintViewportDrag();
        blueprintAssetSelectedComponent.clear();
        blueprintAssetSelectedLogicComponent = nodes[index].at("id");
        if (auto* scroll = document->GetElementById("blueprint-asset-details-scroll")) scroll->SetScrollTop(0);
        return true;
    }
    if (click && field.rfind("property-", 0) == 0 && index < nodes.size())
    {
        const auto operation = element.GetAttribute<std::string>("data-script-operation", "");
        if (operation == "toggle" || operation == "previous" || operation == "next")
        {
            const auto fields = LogicFields(runtimeView.scripts.componentClasses, nodes[index]);
            const auto property = std::stoull(field.substr(field.find('-') + 1));
            if (property < fields.size())
            {
                Json request;
                ReadScriptCompositeEvent(element, fields[property],
                    CompleteScriptPropertyValue(nodes[index], fields[property]), true, request);
            }
            return true;
        }
    }
    if (!runtimeView.scripts.available || runtimeView.scripts.state != ScriptPlayState::Editing ||
        blueprintCommandAwaitingSnapshot) return true;
    if (field == "add" && click && nodes.size() < 128)
    {
        const auto className = ControlText(*document->GetElementById(Prefix + "add-class"));
        bool known = false;
        for (const auto& type : runtimeView.scripts.componentClasses)
            if (ScriptTypeNameMatches(type, className)) known = true;
        if (!known) return true;
        size_t suffix = nodes.size();
        std::string id;
        bool exists;
        do
        {
            id = "script-" + std::to_string(++suffix);
            exists = false;
            for (const auto& node : nodes) if (node.at("id") == id) exists = true;
        } while (exists);
        nodes.push_back({{"id", id}, {"class", className}, {"properties", Json::object()}});
        blueprintAssetSelectedComponent.clear();
        blueprintAssetSelectedLogicComponent = id;
    }
    else if (index < nodes.size())
    {
        auto& node = nodes[index];
        const auto fields = LogicFields(runtimeView.scripts.componentClasses, node);
        const auto text = ControlText(element);
        if (field == "remove" && click) nodes.erase(index);
        else if (field == "class" && !click)
        {
            if (node.at("class") == text) return true;
            node["class"] = text;
            node["properties"] = Json::object();
            node.erase("instanceEditable");
        }
        else if (field == "enabled" && !click)
        {
            if (node.value("enabled", true) == (text == "true")) return true;
            node["enabled"] = text == "true";
        }
        else if (field.rfind("property-", 0) == 0 || field.rfind("instance-", 0) == 0 || field.rfind("reset-", 0) == 0)
        {
            const auto separator = field.find('-');
            const size_t property = std::stoull(field.substr(separator + 1));
            if (property >= fields.size()) return true;
            const auto& schema = fields[property];
            const auto name = schema.at("name").get<std::string>();
            if (field.rfind("reset-", 0) == 0 && click) node["properties"].erase(name);
            else if (field.rfind("instance-", 0) == 0 && !click)
            {
                const bool allowed = element.HasAttribute("checked");
                if (InstanceAllowed(node, name) == allowed) return true;
                auto permissions = node.value("instanceEditable", Json::array());
                for (auto item = permissions.begin(); item != permissions.end();)
                    if (*item == name) item = permissions.erase(item); else ++item;
                if (allowed) permissions.push_back(name);
                if (permissions.empty()) node.erase("instanceEditable");
                else node["instanceEditable"] = std::move(permissions);
            }
            else if (field.rfind("property-", 0) == 0 && ScriptPropertyIsComposite(schema))
            {
                const auto authored = node.at("properties").value(name, Json(nullptr));
                const auto effective = CompleteScriptPropertyValue(node, schema);
                Json request;
                if (ReadScriptCompositeEvent(element, schema, effective, click, request) != ScriptPropertyEvent::Edit)
                    return true;
                const auto updated = ApplyScriptPropertyEdit(schema, authored, effective, request);
                if (updated.is_null()) node["properties"].erase(name);
                else node["properties"][name] = updated;
            }
            else if (field.rfind("property-", 0) == 0 && !click)
            {
                const auto previous = node.at("properties").value(name, schema.at("value"));
                auto value = previous;
                if (schema.at("type") == "quaternion")
                {
                    if (!ParseScriptQuaternion(text, value)) return true;
                }
                else if (value.is_array())
                {
                    const size_t axis = element.GetAttribute<size_t>("data-component", value.size());
                    size_t end = 0;
                    const double number = std::stod(text, &end);
                    if (axis >= value.size() || end != text.size() || !std::isfinite(number)) return true;
                    value[axis] = number;
                }
                else value = value.is_string() ? Json(text) : schema.at("type") == "bool" ? Json(text == "true") : Json::parse(text);
                if (value == previous) return true;
                if (value == schema.at("value")) node["properties"].erase(name);
                else node["properties"][name] = std::move(value);
            }
            else return true;
        }
        else return true;
    }
    else return true;
    if (nodes.empty()) draft.erase("scriptComponents");
    else draft["scriptComponents"] = std::move(nodes);
    QueueBlueprintAssetDraft(std::move(draft));
    return true;
}
} // namespace VL::EditorUi
