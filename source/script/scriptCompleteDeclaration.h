#pragma once

#include "script/scriptLogicComponents.h"
#include "script/scriptPropertySchema.h"
#include "script/scriptBlueprintAsset.h"

namespace VL
{
inline nlohmann::json CompleteBlueprintDeclaration(nlohmann::json blueprint,
    const nlohmann::json& catalog, const nlohmann::json& componentCatalog)
{
    blueprint["enabled"] = blueprint.value("enabled", true);
    ValidateScriptPropertySchema(blueprint, catalog);
    // 资产与 inline 分别拥有实例权限，补默认值不能把未覆写项写回 Scene。
    if (!blueprint.contains("__instanceEditable"))
    {
        blueprint["__instanceEditable"] = nlohmann::json::array();
        for (const auto& type : catalog)
            if (ScriptTypeNameMatches(type, blueprint.at("class").get<std::string>()))
                for (const auto& property : type.at("properties"))
                    if (property.value("instanceEditable", false))
                        blueprint["__instanceEditable"].push_back(property.at("name"));
        if (!blueprint.contains("__templatePath"))
            ValidateScriptInstanceOverrides({{"instanceEditable", blueprint["__instanceEditable"]}}, blueprint);
    }
    for (const auto& type : catalog)
        if (ScriptTypeNameMatches(type, blueprint.at("class").get<std::string>()))
            for (const auto& property : type.at("properties"))
            {
                const auto name = property.at("name").get<std::string>();
                blueprint["properties"][name] = CompleteScriptPropertyValue(blueprint, property);
            }
    CompleteScriptLogicComponents(blueprint, componentCatalog);
    return blueprint;
}
} // namespace VL
