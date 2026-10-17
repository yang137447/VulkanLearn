#include "editor/ui/scriptPropertyControls.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/widgetDocument.h"
#include "script/scriptTypeIdentity.h"

namespace VL::EditorUi
{
// 蓝图和 Scene 使用同一份类目录生成控件；向量分量只投递 axis，由各自文档 owner 合成。
nlohmann::json ScriptPropertyField(const nlohmann::json& classes,
    const std::string& className, const std::string& name, const nlohmann::json& value)
{
    for (const auto& type : classes)
        if (ScriptTypeNameMatches(type, className))
            for (const auto& field : type.at("properties"))
                if (field.at("name") == name) return field;
    return {{"name", name}, {"type", value.is_boolean() ? "bool" : value.is_string() ? "string" :
        value.is_array() ? "vector" + std::to_string(value.size()) : "number"}, {"value", value}};
}

std::string ScriptPropertyText(const nlohmann::json& value)
{
    return value.is_string() ? value.get<std::string>() : value.dump();
}

bool ScriptPropertyUsesComponents(const nlohmann::json& field, const nlohmann::json& value)
{
    const auto type = field.at("type").get<std::string>();
    return value.is_array() && (type == "vector2" || type == "vector3" || type == "vector4") &&
        value.size() >= 2 && value.size() <= 4;
}

bool ScriptPropertyIsComposite(const nlohmann::json& field)
{
    const auto type = field.at("type").get<std::string>();
    return type == "object" || type == "array" || type == "dictionary";
}

std::string ScriptPropertyPathId(const std::string& root, const nlohmann::json& path)
{
    if (path.empty()) return root;
    std::string suffix;
    for (const unsigned char value : path.dump())
    {
        suffix += "0123456789abcdef"[value >> 4];
        suffix += "0123456789abcdef"[value & 15];
    }
    return root + "-path-" + suffix;
}

std::string ScriptPropertyActionAttributes(UiWidgets::WidgetAction action)
{
    return " data-action=\"" + UiWidgets::Text(action.name) + "\" data-index=\"" +
        std::to_string(action.index) + "\"";
}

std::string ScriptCompositeShell(const std::string& root, UiWidgets::WidgetAction action,
    const nlohmann::json& field, const nlohmann::json& path)
{
    const auto id = ScriptPropertyPathId(root, path);
    const auto address = ScriptPropertyActionAttributes(action);
    const auto type = field.at("type").get<std::string>();
    std::string body = "<div id=\"" + id + "\" class=\"script-composite\" data-script-path=\"" +
        UiWidgets::Text(path.dump()) + "\" data-expanded=\"" + (path.empty() ? "true" : "false") +
        "\"><div class=\"script-composite-head\"><button id=\"" + id +
        "-toggle\" class=\"browser-icon\"" + address +
        " data-script-operation=\"toggle\" title=\"Expand or collapse\"><img src=\"icons/arrow-right.png\"/></button>"
        "<span id=\"" + id + "-count\" class=\"script-composite-count\"></span>";
    if (type == "dictionary")
        body += "<input id=\"" + id + "-key\" type=\"text\" placeholder=\"Key\" title=\"New dictionary key\"/>";
    if (type != "object")
        body += "<button id=\"" + id + "-add\" class=\"browser-icon\"" + address +
            " data-script-operation=\"" + (type == "array" ? "append" : "insert") +
            "\" title=\"Add item\"><img src=\"icons/plus.png\"/></button>"
            "<button id=\"" + id + "-previous\" class=\"browser-icon\"" + address +
            " data-script-operation=\"previous\" title=\"Previous items\"><img src=\"icons/arrow-left.png\"/></button>"
            "<button id=\"" + id + "-next\" class=\"browser-icon\"" + address +
            " data-script-operation=\"next\" title=\"Next items\"><img src=\"icons/arrow-right.png\"/></button>";
    return body + "</div><div id=\"" + id + "-body\" class=\"script-composite-body\"></div></div>";
}

bool ParseScriptQuaternion(const std::string& text, nlohmann::json& value)
{
    auto candidate = nlohmann::json::parse(text, nullptr, false);
    if (!candidate.is_array() || candidate.size() != 4) return false;
    double norm = 0;
    for (const auto& axis : candidate)
    {
        if (!axis.is_number()) return false;
        const double number = axis.get<double>();
        if (!std::isfinite(number)) return false;
        norm += number * number;
    }
    if (!std::isfinite(norm) || std::abs(norm - 1) > 1e-4) return false;
    value = std::move(candidate);
    return true;
}

std::string ScriptPropertyControl(const std::string& id, UiWidgets::WidgetAction action,
    const nlohmann::json& field, const nlohmann::json& value)
{
    using namespace UiWidgets;
    if (ScriptPropertyIsComposite(field))
        return ScriptCompositeShell(id, action, field, nlohmann::json::array());
    // 单位四元数必须整组编辑；逐轴提交会让合法旋转途中经过非单位候选而始终被拒绝。
    if (ScriptPropertyUsesComponents(field, value))
    {
        std::vector<NumericComponent> controls;
        for (size_t axis = 0; axis < value.size(); ++axis)
        {
            NumericComponent control;
            control.id = id + "-" + std::to_string(axis);
            control.label = std::string(1, "XYZW"[axis]);
            control.value = value[axis].dump();
            control.change = {action.name, action.index, static_cast<uint32_t>(axis)};
            controls.push_back(std::move(control));
        }
        return NumericComponents(controls);
    }
    if (field.at("type") == "bool")
        return Select(id, action, Option("false", "False") + Option("true", "True"));
    if (field.at("type") == "enum")
    {
        std::string options;
        for (const auto& option : field.at("options")) options += Option(option, option);
        return Select(id, action, options);
    }
    return Input(id, action, ScriptPropertyText(value));
}

std::string ScriptPropertyRow(const std::string& label, const std::string& id,
    UiWidgets::WidgetAction action, const nlohmann::json& field, const nlohmann::json& value,
    const std::string& trailing)
{
    if (!ScriptPropertyIsComposite(field))
        return UiWidgets::PropertyControlRow(label, ScriptPropertyControl(id, action, field, value) + trailing);
    return "<div class=\"script-composite-property\">" + UiWidgets::PropertyControlRow(label, trailing) +
        ScriptPropertyControl(id, action, field, value) + "</div>";
}

const nlohmann::json* ScriptPropertyPathSchema(const nlohmann::json& root,
    const nlohmann::json& path)
{
    const auto* field = &root;
    for (const auto& part : path)
    {
        const auto type = field->at("type").get<std::string>();
        if (type == "object")
        {
            const nlohmann::json* next = nullptr;
            for (const auto& child : field->at("fields"))
                if (part.is_string() && child.at("name") == part) next = &child;
            if (!next) return nullptr;
            field = next;
        }
        else if (type == "array" || type == "dictionary") field = &field->at("element");
        else return nullptr;
    }
    return field;
}

const nlohmann::json* ScriptPropertyPathValue(const nlohmann::json& root,
    const nlohmann::json& path)
{
    const auto* value = &root;
    for (const auto& part : path)
    {
        if (value->is_array() && part.is_number_unsigned() && part.get<size_t>() < value->size())
            value = &value->at(part.get<size_t>());
        else if (value->is_object() && part.is_string() && value->contains(part.get<std::string>()))
            value = &value->at(part.get<std::string>());
        else return nullptr;
    }
    return value;
}

void RefreshScriptPropertyControl(Rml::ElementDocument& document, UiWidgets::WidgetDocument& widgets,
    const std::string& root, UiWidgets::WidgetAction action, const nlohmann::json& field,
    const nlohmann::json& value, bool disabled, bool overridden,
    const nlohmann::json& path, const nlohmann::json& authored, bool allowNestedReset)
{
    using Json = nlohmann::json;
    const auto id = ScriptPropertyPathId(root, path);
    if (!ScriptPropertyIsComposite(field))
    {
        const bool components = ScriptPropertyUsesComponents(field, value);
        for (size_t axis = 0; axis < (components ? value.size() : 1u); ++axis)
        {
            const auto control = id + (components ? "-" + std::to_string(axis) : "");
            widgets.SetControlValue(control, ScriptPropertyText(components ? value[axis] : value));
            widgets.SetDisabled(control, disabled);
            widgets.SetClass(control, "script-overridden", overridden);
        }
        return;
    }
    auto* group = document.GetElementById(id);
    if (!group) return;
    constexpr size_t PageSize = 16;
    const auto type = field.at("type").get<std::string>();
    const bool expanded = group->GetAttribute<std::string>("data-expanded", "false") == "true";
    const size_t count = type == "object" ? field.at("fields").size() : value.size();
    const size_t page = std::min(group->GetAttribute<size_t>("data-page", 0),
        count == 0 ? 0u : (count - 1) / PageSize);
    if (group->GetAttribute<size_t>("data-page", 0) != page) group->SetAttribute("data-page", page);
    widgets.SetProperty(id + "-body", "display", expanded ? "block" : "none");
    widgets.SetClass(id + "-toggle", "selected", expanded);
    widgets.SetMarkup(id + "-count", UiWidgets::Text(field.value("typeName", type)) +
        " (" + std::to_string(count) + ")" +
        (type != "object" && count > PageSize ? " " + std::to_string(page + 1) + "/" +
            std::to_string((count + PageSize - 1) / PageSize) : ""));
    widgets.SetDisabled(id + "-key", disabled || count >= 1024);
    widgets.SetDisabled(id + "-add", disabled || count >= 1024);
    widgets.SetDisabled(id + "-previous", page == 0);
    widgets.SetDisabled(id + "-next", (page + 1) * PageSize >= count);
    // 折叠子树延迟挂载，集合按页显示；刷新叶子值时保留既有控件和未完成输入。
    if (!expanded) return;
    Json children = Json::array();
    if (type == "object")
        for (const auto& child : field.at("fields"))
            children.push_back({{"key", child.at("name")}, {"field", child}});
    else if (type == "array")
        for (size_t index = page * PageSize; index < std::min(count, (page + 1) * PageSize); ++index)
            children.push_back({{"key", index}, {"field", field.at("element")}});
    else
    {
        size_t index = 0;
        for (const auto& child : value.items())
        {
            if (index >= page * PageSize && index < (page + 1) * PageSize)
                children.push_back({{"key", child.key()}, {"field", field.at("element")}});
            ++index;
        }
    }
    const auto structure = children.dump();
    if (group->GetAttribute<std::string>("data-script-structure", "") != structure)
    {
        std::string markup;
        for (const auto& child : children)
        {
            auto childPath = path; childPath.push_back(child.at("key"));
            const auto childId = ScriptPropertyPathId(root, childPath);
            const auto& schema = child.at("field");
            const auto* childValue = ScriptPropertyPathValue(value, Json::array({child.at("key")}));
            if (!childValue) continue;
            std::string remove;
            if (type != "object")
                remove = "<button id=\"" + childId + "-remove\" class=\"browser-icon\"" +
                    ScriptPropertyActionAttributes(action) + " data-script-operation=\"remove\" data-script-path=\"" +
                    UiWidgets::Text(path.dump()) + "\" data-script-item=\"" + UiWidgets::Text(child.at("key").dump()) +
                    "\" title=\"Remove item\"><img src=\"icons/trash.png\"/></button>";
            else if (allowNestedReset)
                remove = "<button id=\"" + childId + "-reset\" class=\"browser-icon\"" +
                    ScriptPropertyActionAttributes(action) + " data-script-operation=\"reset\" data-script-path=\"" +
                    UiWidgets::Text(childPath.dump()) +
                    "\" title=\"Restore inherited value\"><img src=\"icons/refresh.png\"/></button>";
            const auto label = child.at("key").is_string()
                ? (child.at("key").get_ref<const std::string&>().empty() ? "\"\"" : child.at("key").get<std::string>())
                : "[" + child.at("key").dump() + "]";
            std::string control;
            if (ScriptPropertyIsComposite(schema))
                control = "<div class=\"script-composite-property\">" + UiWidgets::PropertyControlRow(label, remove) +
                    ScriptCompositeShell(root, action, schema, childPath) + "</div>";
            else
                control = UiWidgets::PropertyControlRow(label,
                    ScriptPropertyControl(childId, action, schema, *childValue) + remove);
            markup += "<div data-script-path=\"" + UiWidgets::Text(childPath.dump()) + "\">" + control + "</div>";
        }
        widgets.SetMarkup(id + "-body", std::move(markup));
        group->SetAttribute("data-script-structure", structure);
    }
    for (const auto& child : children)
    {
        auto childPath = path; childPath.push_back(child.at("key"));
        const auto* childValue = ScriptPropertyPathValue(value, Json::array({child.at("key")}));
        if (!childValue) continue;
        const bool childOverridden = allowNestedReset && type == "object"
            ? ScriptPropertyPathValue(authored, childPath) != nullptr : overridden;
        RefreshScriptPropertyControl(document, widgets, root, action, child.at("field"),
            *childValue, disabled, childOverridden, childPath, authored, allowNestedReset && type == "object");
        widgets.SetDisabled(ScriptPropertyPathId(root, childPath) + "-remove", disabled);
        widgets.SetDisabled(ScriptPropertyPathId(root, childPath) + "-reset",
            disabled || !ScriptPropertyPathValue(authored, childPath));
    }
}

ScriptPropertyEvent ReadScriptCompositeEvent(Rml::Element& element,
    const nlohmann::json& schema, const nlohmann::json& effective, bool click,
    nlohmann::json& request)
{
    using Json = nlohmann::json;
    if (!ScriptPropertyIsComposite(schema)) return ScriptPropertyEvent::Ignored;
    auto* address = &element;
    while (address && !address->HasAttribute("data-script-path")) address = address->GetParentNode();
    if (!address) return ScriptPropertyEvent::Local;
    auto path = Json::parse(address->GetAttribute<std::string>("data-script-path", "[]"));
    const auto operation = element.GetAttribute<std::string>("data-script-operation", "set");
    const auto* field = ScriptPropertyPathSchema(schema, path);
    const auto* previous = ScriptPropertyPathValue(effective, path);
    if (!field || !previous) return ScriptPropertyEvent::Local;
    if (click && (operation == "toggle" || operation == "previous" || operation == "next"))
    {
        if (operation == "toggle")
            address->SetAttribute("data-expanded", address->GetAttribute<std::string>("data-expanded", "false") == "true" ? "false" : "true");
        else
        {
            size_t page = address->GetAttribute<size_t>("data-page", 0);
            address->SetAttribute("data-page", operation == "next" ? page + 1 : page == 0 ? 0 : page - 1);
        }
        return ScriptPropertyEvent::Local;
    }
    request = {{"path", path}, {"operation", operation}};
    const auto type = field->at("type").get<std::string>();
    // 只发送作者路径；Scene owner 会基于最新文档合成，不能用旧快照覆盖兄弟值。
    if (click && operation == "reset") return ScriptPropertyEvent::Edit;
    if (click && (operation == "append" || operation == "insert" || operation == "remove"))
    {
        if (operation != "remove" && previous->size() >= 1024) return ScriptPropertyEvent::Local;
        if (operation == "remove")
        {
            const auto key = Json::parse(element.GetAttribute<std::string>("data-script-item", "null"));
            request[type == "array" ? "index" : "key"] = key;
        }
        if (operation == "insert")
        {
            const auto id = address->GetId() + "-key";
            const auto* input = dynamic_cast<Rml::ElementFormControl*>(address->GetOwnerDocument()->GetElementById(id));
            if (!input) return ScriptPropertyEvent::Local;
            const auto key = input->GetValue();
            if (previous->contains(key)) return ScriptPropertyEvent::Local;
            request["key"] = key;
        }
        return ScriptPropertyEvent::Edit;
    }
    if (click || operation != "set" || ScriptPropertyIsComposite(*field)) return ScriptPropertyEvent::Local;
    const auto* input = dynamic_cast<Rml::ElementFormControl*>(&element);
    if (!input) return ScriptPropertyEvent::Local;
    const auto text = input->GetValue();
    Json value;
    if (type == "quaternion")
    {
        if (!ParseScriptQuaternion(text, value)) return ScriptPropertyEvent::Local;
    }
    else if (previous->is_string()) value = text;
    else if (type == "bool") value = text == "true";
    else value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || value.is_null() || (value.is_number() && !std::isfinite(value.get<double>())))
        return ScriptPropertyEvent::Local;
    if (ScriptPropertyUsesComponents(*field, *previous))
    {
        const size_t axis = element.GetAttribute<size_t>("data-component", previous->size());
        if (axis >= previous->size() || !value.is_number() || previous->at(axis) == value) return ScriptPropertyEvent::Local;
        request["path"].push_back(axis);
    }
    else if (value == *previous) return ScriptPropertyEvent::Local;
    request["value"] = std::move(value);
    return ScriptPropertyEvent::Edit;
}
} // namespace VL::EditorUi
