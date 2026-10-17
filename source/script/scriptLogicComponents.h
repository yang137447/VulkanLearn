#pragma once

#include <set>
#include "script/scriptPropertySchema.h"

namespace VL
{
// 逻辑组件使用独立稳定 ID，不参与渲染组件父链；实例只保存获授权属性的稀疏覆写。
inline void ValidateScriptLogicComponents(const nlohmann::json& components)
{
    if (!components.is_array() || components.size() > 128)
        throw std::invalid_argument("scriptComponents requires at most 128 components");
    std::set<std::string> ids;
    for (const auto& component : components)
    {
        if (!component.is_object()) throw std::invalid_argument("Script component must be an object");
        for (const auto& field : component.items())
            if (field.key() != "id" && field.key() != "class" && field.key() != "enabled" &&
                field.key() != "properties" && field.key() != "instanceEditable")
                throw std::invalid_argument("Unsupported script component field: " + field.key());
        for (const char* field : {"id", "class"})
            if (!component.contains(field) || !component.at(field).is_string() ||
                component.at(field).get<std::string>().empty() ||
                component.at(field).get<std::string>().find('\0') != std::string::npos)
                throw std::invalid_argument("Script component requires stable ID and class");
        const auto id = component.at("id").get<std::string>();
        if (id.find_first_of("/\\|") != std::string::npos || !ids.insert(id).second)
            throw std::invalid_argument("Duplicate or invalid script component ID");
        if (component.contains("enabled") && !component.at("enabled").is_boolean())
            throw std::invalid_argument("Script component enabled must be boolean");
        if (!component.contains("properties"))
            throw std::invalid_argument("Script component properties must be an object");
        ValidateScriptPropertyObject(component.at("properties"));
        const auto names = component.value("instanceEditable", nlohmann::json::array());
        if (!names.is_array() || names.size() > 128)
            throw std::invalid_argument("Script component instanceEditable requires property names");
        std::set<std::string> unique;
        for (const auto& name : names)
            if (!name.is_string() || name.get<std::string>().empty() ||
                name.get<std::string>().find('\0') != std::string::npos || !unique.insert(name.get<std::string>()).second)
                throw std::invalid_argument("Script component instanceEditable requires unique names");
    }
}

inline void ValidateScriptLogicOverrideShape(const nlohmann::json& overrides)
{
    if (!overrides.is_object() || overrides.size() > 128)
        throw std::invalid_argument("scriptComponentOverrides requires an ID to properties object");
    for (const auto& item : overrides.items())
    {
        if (item.key().empty() || item.key().find_first_of("/\\|") != std::string::npos ||
            item.key().find('\0') != std::string::npos || !item.value().is_object() || item.value().size() > 128)
            throw std::invalid_argument("Invalid script component override shape");
        ValidateScriptPropertyObject(item.value());
    }
}

inline void ApplyScriptLogicOverrides(nlohmann::json& components, const nlohmann::json& overrides)
{
    ValidateScriptLogicOverrideShape(overrides);
    for (const auto& item : overrides.items())
    {
        nlohmann::json* component = nullptr;
        for (auto& node : components) if (node.at("id") == item.key()) component = &node;
        if (!component) throw std::invalid_argument("Unknown overridden script component");
        const auto names = component->value("instanceEditable", nlohmann::json::array());
        for (const auto& property : item.value().items())
        {
            if (std::find(names.begin(), names.end(), property.key()) == names.end())
                throw std::invalid_argument("Script component property is not Instance Editable: " + property.key());
            (*component)["properties"][property.key()] = property.value();
        }
    }
    ValidateScriptLogicComponents(components);
}

inline void ValidateScriptLogicPropertySchema(const nlohmann::json& blueprint, const nlohmann::json& catalog)
{
    const auto nodes = blueprint.value("scriptComponents", nlohmann::json::array());
    ValidateScriptLogicComponents(nodes);
    for (const auto& node : nodes)
    {
        auto layered = node;
        if (blueprint.contains("__logicPropertyDefaults"))
            layered["__propertyDefaults"] = blueprint.at("__logicPropertyDefaults")
                .value(node.at("id").get<std::string>(), nlohmann::json::object());
        ValidateScriptPropertySchema(layered, catalog);
    }
}

inline void CompleteScriptLogicComponents(nlohmann::json& blueprint, const nlohmann::json& catalog)
{
    ValidateScriptLogicPropertySchema(blueprint, catalog);
    if (!blueprint.contains("scriptComponents")) return;
    for (auto& node : blueprint["scriptComponents"])
    {
        const auto authored = node["properties"];
        for (const auto& type : catalog)
            if (ScriptTypeNameMatches(type, node.at("class").get<std::string>()))
            {
                // BP 已提供自己的白名单；inline 的权限只能来自 C# Expose 标注。
                if (!blueprint.value("__scriptComponentAsset", false) && !blueprint.contains("__templatePath"))
                    for (const auto& item : authored.items())
                    {
                        bool allowed = false;
                        for (const auto& property : type.at("properties"))
                            if (property.at("name") == item.key()) allowed = property.value("instanceEditable", false);
                        if (!allowed) throw std::invalid_argument("Inline script component property is not Instance Editable");
                    }
                auto layered = node;
                if (blueprint.contains("__logicPropertyDefaults"))
                    layered["__propertyDefaults"] = blueprint.at("__logicPropertyDefaults")
                        .value(node.at("id").get<std::string>(), nlohmann::json::object());
                for (const auto& property : type.at("properties"))
                    node["properties"][property.at("name").get<std::string>()] =
                        CompleteScriptPropertyValue(layered, property);
            }
    }
}
} // namespace VL
