#include "materialAssetValidator.h"

#include <algorithm>
#include <filesystem>
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

    bool IsShaderRootRelativePath(const nlohmann::json& value)
    {
        if (!value.is_string())
        {
            return false;
        }
        const std::filesystem::path path(value.get<std::string>());
        if (path.empty() || path.is_absolute())
        {
            return false;
        }
        const std::filesystem::path normalized = path.lexically_normal();
        return normalized.begin() == normalized.end() || *normalized.begin() != "..";
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

    if (materialJson.contains("features"))
    {
        RequireObjectField(materialJson, "features", materialPath);
        for (const auto& [featureName, featureValue] : materialJson["features"].items())
        {
            if (featureName != "modifiesMeshPosition" || !featureValue.is_boolean())
            {
                throw std::runtime_error(
                    "Material features only supports boolean modifiesMeshPosition: " +
                    std::string(materialPath));
            }
        }
    }

    if (materialJson.contains("shaderEvaluation"))
    {
        RequireObjectField(materialJson, "shaderEvaluation", materialPath);
        const auto& evaluation = materialJson["shaderEvaluation"];
        if (evaluation.size() != 2 ||
            !evaluation.contains("vertex") || !IsShaderRootRelativePath(evaluation["vertex"]) ||
            !evaluation.contains("surface") || !IsShaderRootRelativePath(evaluation["surface"]))
        {
            throw std::runtime_error(
                "shaderEvaluation must contain shader/glsl-relative vertex and surface paths: " +
                std::string(materialPath));
        }
    }

    for (const auto& [macroName, macroValue] : materialJson["macros"].items())
    {
        if (MaterialAssetUtils::Trim(macroName).empty() || !MaterialAssetUtils::IsNumericMacroValue(macroValue))
        {
            throw std::runtime_error("Material macros must be non-empty numeric values: " + std::string(materialPath));
        }
    }

    for (const auto& [parameterName, parameterJson] : materialJson["parameters"].items())
    {
        if (!parameterJson.is_object() ||
            !parameterJson.contains("type") || !parameterJson["type"].is_string() ||
            !parameterJson.contains("default"))
        {
            throw std::runtime_error(
                "Material parameter must contain type and default: " + parameterName +
                " in " + std::string(materialPath));
        }
        const std::string type = parameterJson["type"].get<std::string>();
        if (!MaterialAssetUtils::IsMaterialParameterType(type) ||
            !MaterialAssetUtils::MaterialParameterValueMatchesType(parameterJson["default"], type))
        {
            throw std::runtime_error(
                "Material parameter type/default mismatch: " + parameterName +
                " in " + std::string(materialPath));
        }
    }

    for (const auto& [textureName, textureJson] : materialJson["textures"].items())
    {
        if (!textureJson.is_object() ||
            textureJson.value("type", std::string()) != "sampler2D" ||
            !textureJson.contains("default"))
        {
            throw std::runtime_error(
                "Material texture must be sampler2D and contain default: " + textureName +
                " in " + std::string(materialPath));
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
