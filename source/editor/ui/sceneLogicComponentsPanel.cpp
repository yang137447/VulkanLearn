#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <cmath>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptTypeIdentity.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
using Json = nlohmann::json;
using namespace UiWidgets;
const std::string Prefix = "script-logic-";

std::string ControlId(size_t component, const std::string& field, size_t property = 0)
{
    return Prefix + std::to_string(component) + "-" + field + "-" + std::to_string(property);
}

Json Fields(const Json& classes, const Json& node)
{
    for (const auto& type : classes)
        if (ScriptTypeNameMatches(type, node.at("class").get<std::string>())) return type.at("properties");
    Json fields = Json::array();
    for (const auto& property : node.at("properties").items())
        fields.push_back(ScriptPropertyField(classes, node.at("class"), property.key(), property.value()));
    return fields;
}

bool Allowed(const EditorSceneActorSnapshot& actor, const Json& node, const Json& field)
{
    if (actor.scriptBlueprintPath.empty()) return field.value("instanceEditable", false);
    const auto names = node.value("instanceEditable", Json::array());
    return std::find(names.begin(), names.end(), field.at("name")) != names.end();
}

bool Overridden(const EditorSceneActorSnapshot& actor, const Json& node, const Json& field)
{
    const auto id = node.at("id").get<std::string>();
    return actor.scriptLogicOverrides.contains(id) &&
        actor.scriptLogicOverrides.at(id).contains(field.at("name").get<std::string>());
}

std::string InputText(Rml::Element& element)
{
    const auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    return control ? control->GetValue() : "";
}
}

std::string MaterialInstanceAssetEditorPanel::SceneLogicStructure(const EditorSceneActorSnapshot& actor) const
{
    std::string signature;
    for (const auto& node : actor.scriptLogicComponents)
    {
        auto fields = Fields(runtimeView.scripts.componentClasses, node);
        for (auto& field : fields) field.erase("value");
        signature += node.at("id").get<std::string>() + "|" + node.at("class").get<std::string>() +
            "|" + node.value("instanceEditable", Json::array()).dump() + "|" +
            fields.dump() + ";";
    }
    // 只把 schema/权限/身份放进结构签名；属性值刷新保留输入焦点和半成品文本。
    signature += runtimeView.scripts.componentClasses.dump();
    return signature;
}

std::string MaterialInstanceAssetEditorPanel::SceneLogicComponentsMarkup(const EditorSceneActorSnapshot& actor) const
{
    std::string body = "<h3>C# Components</h3>";
    for (size_t index = 0; index < actor.scriptLogicComponents.size(); ++index)
    {
        const auto& node = actor.scriptLogicComponents[index];
        body += PropertyInfo("Component", node.at("id"));
        body += PropertyInfo("Class", node.at("class"));
        body += PropertyControlRow("Enabled", Select(ControlId(index, "enabled"),
            {Prefix + "enabled", index}, Option("true", "True") + Option("false", "False")) +
            "<button id=\"" + ControlId(index, "remove") + "\" class=\"browser-icon\" data-action=\"" +
            Prefix + "remove\" data-index=\"" + std::to_string(index) +
            "\" title=\"Remove C# component\"><img src=\"icons/trash.png\"/></button>");
        const auto fields = Fields(runtimeView.scripts.componentClasses, node);
        for (size_t property = 0; property < fields.size(); ++property)
        {
            const auto& field = fields[property];
            const auto value = node.at("properties").value(field.at("name").get<std::string>(), field.at("value"));
            body += ScriptPropertyRow(field.at("name"), ControlId(index, "property", property),
                    {Prefix + "property-" + std::to_string(property), index}, field, value,
                "<button id=\"" + ControlId(index, "reset", property) +
                "\" class=\"browser-icon\" data-action=\"" + Prefix + "reset-" + std::to_string(property) +
                "\" data-index=\"" + std::to_string(index) +
                "\" title=\"Restore inherited value\"><img src=\"icons/refresh.png\"/></button>");
        }
    }
    std::string options;
    for (const auto& type : runtimeView.scripts.componentClasses)
        options += Option(type.at("name"), type.at("name"));
    body += PropertyControlRow("Add C# Component",
        Select(Prefix + "class", {Prefix + "class"}, options) +
        "<button id=\"" + Prefix + "add\" class=\"browser-icon\" data-action=\"" + Prefix +
        "add\" title=\"Add C# component\"><img src=\"icons/plus.png\"/></button>");
    return body;
}

void MaterialInstanceAssetEditorPanel::RefreshSceneLogicComponents(const EditorSceneActorSnapshot& actor)
{
    const bool editing = runtimeView.scripts.state == ScriptPlayState::Editing &&
        !actor.scriptSpawned && !runtimeView.offlineMode;
    const bool inlineEdit = editing && actor.scriptBlueprintPath.empty();
    SetDisabled(Prefix + "class", !inlineEdit || runtimeView.scripts.componentClasses.empty());
    SetDisabled(Prefix + "add", !inlineEdit || runtimeView.scripts.componentClasses.empty() ||
        actor.scriptLogicComponents.size() >= 128);
    for (size_t index = 0; index < actor.scriptLogicComponents.size(); ++index)
    {
        const auto& node = actor.scriptLogicComponents[index];
        SetControlValue(ControlId(index, "enabled"), node.value("enabled", true) ? "true" : "false");
        SetDisabled(ControlId(index, "enabled"), !inlineEdit);
        SetDisabled(ControlId(index, "remove"), !inlineEdit);
        const auto fields = Fields(runtimeView.scripts.componentClasses, node);
        for (size_t property = 0; property < fields.size(); ++property)
        {
            const auto& field = fields[property];
            const bool allowed = Allowed(actor, node, field);
            const bool overridden = Overridden(actor, node, field);
            const auto value = node.at("properties").value(field.at("name").get<std::string>(), field.at("value"));
            const auto control = ControlId(index, "property", property);
            RefreshScriptPropertyControl(*document, widgetDocument, control,
                {Prefix + "property-" + std::to_string(property), index}, field, value,
                !editing || !allowed, overridden, Json::array(),
                actor.scriptLogicOverrides.contains(node.at("id").get<std::string>())
                    ? actor.scriptLogicOverrides.at(node.at("id").get<std::string>()).value(
                        field.at("name").get<std::string>(), Json(nullptr)) : Json(nullptr));
            SetDisabled(ControlId(index, "reset", property), !editing || !allowed || !overridden);
        }
    }
}

bool MaterialInstanceAssetEditorPanel::HandleSceneLogicEvent(Rml::Element& element,
    const std::string& action, bool click)
{
    if (action.rfind(Prefix, 0) != 0) return false;
    const auto* actor = SelectedActor();
    if (!actor || actor->type != EditorSceneActorType::Script) return true;
    const bool editing = !actor->scriptSpawned && !runtimeView.offlineMode &&
        runtimeView.scripts.state == ScriptPlayState::Editing;
    const auto field = action.substr(Prefix.size());
    const bool inherited = !actor->scriptBlueprintPath.empty();
    if (field == "add" && click && !inherited && editing)
    {
        const auto name = InputText(*document->GetElementById(Prefix + "class"));
        for (const auto& type : runtimeView.scripts.componentClasses)
            if (type.at("name") == name && actor->scriptLogicComponents.size() < 128)
                RequestScriptEdit(UiActionType::AddScriptLogicComponent, {{"class", name}});
        return true;
    }
    const size_t index = element.GetAttribute<size_t>("data-index", actor->scriptLogicComponents.size());
    if (index >= actor->scriptLogicComponents.size()) return true;
    const auto& node = actor->scriptLogicComponents[index];
    const auto id = node.at("id").get<std::string>();
    if (field == "remove" && click && !inherited && editing)
        RequestScriptEdit(UiActionType::RemoveScriptLogicComponent, {{"component", id}});
    else if (field == "enabled" && !click && !inherited && editing)
    {
        const bool enabled = InputText(element) == "true";
        if (enabled != node.value("enabled", true))
            RequestScriptEdit(UiActionType::SetScriptLogicComponent,
                {{"component", id}, {"field", "enabled"}, {"value", enabled}});
    }
    else if (field.rfind("property-", 0) == 0 ||
        (field.rfind("reset-", 0) == 0 && click))
    {
        const size_t property = std::stoull(field.substr(field.find('-') + 1));
        const auto fields = Fields(runtimeView.scripts.componentClasses, node);
        if (property >= fields.size()) return true;
        const auto& schema = fields[property];
        const auto name = schema.at("name").get<std::string>();
        Json payload{{"component", id}, {"property", name}};
        if (field.rfind("property-", 0) == 0 && ScriptPropertyIsComposite(schema))
        {
            Json request;
            if (ReadScriptCompositeEvent(element, schema,
                node.at("properties").value(name, schema.at("value")), click, request) != ScriptPropertyEvent::Edit)
                return true;
            if (editing && Allowed(*actor, node, schema))
            {
                payload.update(request);
                RequestScriptEdit(UiActionType::SetScriptLogicComponent, std::move(payload));
            }
            return true;
        }
        if (!editing || !Allowed(*actor, node, schema)) return true;
        if (click)
        {
            if (field.rfind("reset-", 0) != 0) return true;
            if (!Overridden(*actor, node, schema)) return true;
            payload["reset"] = true;
        }
        else
        {
            const auto previous = node.at("properties").value(name, schema.at("value"));
            const auto text = InputText(element);
            Json value;
            if (schema.at("type") == "quaternion")
            {
                if (!ParseScriptQuaternion(text, value)) return true;
            }
            else value = previous.is_string() ? Json(text) :
                schema.at("type") == "bool" ? Json(text == "true") : Json::parse(text);
            if (ScriptPropertyUsesComponents(schema, previous))
            {
                const size_t axis = element.GetAttribute<size_t>("data-component", previous.size());
                if (axis >= previous.size() || !value.is_number() || !std::isfinite(value.get<double>()) ||
                    previous[axis] == value) return true;
                payload["axis"] = axis;
            }
            else if (value == previous || (value.is_number() && !std::isfinite(value.get<double>()))) return true;
            payload["value"] = value;
        }
        RequestScriptEdit(UiActionType::SetScriptLogicComponent, std::move(payload));
    }
    return true;
}
bool MaterialInstanceAssetEditorPanel::HandleSceneCompositePropertyEvent(Rml::Element& element,
    const std::string& action, bool click)
{
    if (action != "script-property") return false;
    const auto* actor = SelectedActor();
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    if (!actor || actor->type != EditorSceneActorType::Script || index >= actor->scriptProperties.size()) return false;
    auto property = actor->scriptProperties.begin();
    std::advance(property, index);
    const auto schema = ScriptPropertyField(runtimeView.scripts.classes,
        actor->scriptClass, property.key(), property.value());
    if (!ScriptPropertyIsComposite(schema)) return false;
    Json request;
    if (ReadScriptCompositeEvent(element, schema, property.value(), click, request) != ScriptPropertyEvent::Edit)
        return true;
    if (actor->scriptSpawned || runtimeView.offlineMode ||
        runtimeView.scripts.state != ScriptPlayState::Editing ||
        std::find(actor->scriptInstanceEditable.begin(), actor->scriptInstanceEditable.end(),
            property.key()) == actor->scriptInstanceEditable.end()) return true;
    request["property"] = property.key();
    RequestScriptEdit(UiActionType::SetScriptProperty, std::move(request));
    return true;
}
} // namespace VL::EditorUi
