#include "materialAssetValidator.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include "../materialAssetUtils.h"

namespace
{
    void RequireObjectField(const nlohmann::json& json, std::string_view field, std::string_view context)
    {
        const std::string fieldName(field);
        if (!json.contains(fieldName) || !json.at(fieldName).is_object())
        {
            throw std::runtime_error(std::string(context) + " is missing object field \"" + fieldName + "\"");
        }
    }
}

void MaterialAssetValidator::ValidateDefinition(
    const nlohmann::json& materialJson,
    std::string_view materialPath)
{
    if (!materialJson.is_object())
    {
        throw std::runtime_error("Material definition must be a JSON object: " + std::string(materialPath));
    }
    if (materialJson.value("type", std::string()) != "material")
    {
        throw std::runtime_error("Material definition type must be \"material\": " + std::string(materialPath));
    }
    if (!materialJson.contains("name") || !materialJson["name"].is_string())
    {
        throw std::runtime_error("Material definition is missing string field \"name\": " + std::string(materialPath));
    }
    if (!materialJson.contains("shadingModel") || !materialJson["shadingModel"].is_string())
    {
        throw std::runtime_error("Material definition is missing string field \"shadingModel\": " + std::string(materialPath));
    }
    RequireObjectField(materialJson, "renderStates", materialPath);
    RequireObjectField(materialJson, "macros", materialPath);
    RequireObjectField(materialJson, "parameters", materialPath);
    RequireObjectField(materialJson, "textures", materialPath);
    MaterialAssetUtils::ShadingModelToId(materialJson["shadingModel"].get<std::string>());

    for (const auto& [macroName, macroValue] : materialJson["macros"].items())
    {
        if (MaterialAssetUtils::Trim(macroName).empty() || !MaterialAssetUtils::IsNumericMacroValue(macroValue))
        {
            throw std::runtime_error("Material macros must be non-empty numeric values: " + std::string(materialPath));
        }
    }
}

void MaterialAssetValidator::ValidateInstanceHeader(
    const nlohmann::json& materialInstanceJson,
    std::string_view materialInstancePath)
{
    if (!materialInstanceJson.is_object())
    {
        throw std::runtime_error("Material instance must be a JSON object: " + std::string(materialInstancePath));
    }

    static const std::vector<std::string> allowedFields = {
        "name",
        "type",
        "configHelp",
        "material",
        "renderStateOverrides",
        "macros",
        "parameters",
        "textures"
    };
    for (const auto& [field, value] : materialInstanceJson.items())
    {
        if (std::find(allowedFields.begin(), allowedFields.end(), field) == allowedFields.end())
        {
            throw std::runtime_error("Unknown material instance field \"" + field + "\": " + std::string(materialInstancePath));
        }
    }

    if (materialInstanceJson.value("type", std::string()) != "materialInstance")
    {
        throw std::runtime_error("Material instance type must be \"materialInstance\": " + std::string(materialInstancePath));
    }
    if (!materialInstanceJson.contains("material") || !materialInstanceJson["material"].is_string())
    {
        throw std::runtime_error("Material instance is missing string field \"material\": " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("renderStateOverrides") && !materialInstanceJson["renderStateOverrides"].is_object())
    {
        throw std::runtime_error("renderStateOverrides must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("macros") && !materialInstanceJson["macros"].is_object())
    {
        throw std::runtime_error("Material instance macros must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("parameters") && !materialInstanceJson["parameters"].is_object())
    {
        throw std::runtime_error("Material instance parameters must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("textures") && !materialInstanceJson["textures"].is_object())
    {
        throw std::runtime_error("Material instance textures must be an object: " + std::string(materialInstancePath));
    }
}

void MaterialAssetValidator::EnsureKnownOverrideKeys(
    const nlohmann::json& overrides,
    const nlohmann::json& source,
    std::string_view field,
    std::string_view materialInstancePath)
{
    for (const auto& [name, value] : overrides.items())
    {
        if (!source.contains(name))
        {
            throw std::runtime_error(
                "Material instance overrides unknown " + std::string(field) + " \"" + name + "\": " +
                std::string(materialInstancePath));
        }
    }
}
