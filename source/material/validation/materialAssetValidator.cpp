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

    void ValidateMaterialEvaluationPaths(
        const nlohmann::json& evaluation,
        std::string_view materialPath)
    {
        const std::string materialGlslPath =
            MaterialAssetUtils::NormalizeShaderGlslRelativePath(materialPath);
        const std::filesystem::path materialFilePath(materialGlslPath);
        const std::string materialStem = materialFilePath.stem().string();
        const std::filesystem::path materialDirectory = materialFilePath.parent_path();
        const std::string expectedVertex =
            (materialDirectory / (materialStem + ".vertex.glsl")).generic_string();
        const std::string expectedSurface =
            (materialDirectory / (materialStem + ".surface.glsl")).generic_string();
        const std::string actualVertex =
            std::filesystem::path(evaluation.at("vertex").get<std::string>())
                .lexically_normal().generic_string();
        const std::string actualSurface =
            std::filesystem::path(evaluation.at("surface").get<std::string>())
                .lexically_normal().generic_string();
        if (actualVertex != expectedVertex || actualSurface != expectedSurface)
        {
            throw std::runtime_error(
                "shaderEvaluation must point to the material-local public entries " +
                expectedVertex + " and " + expectedSurface + ": " +
                std::string(materialPath));
        }
    }

    void ValidateSparseOverrideObject(
        const nlohmann::json& overrides,
        std::string_view field,
        std::string_view materialInstancePath)
    {
        if (overrides.is_object() && overrides.empty())
        {
            throw std::runtime_error(
                "Empty material instance " + std::string(field) +
                " object must be omitted: " + std::string(materialInstancePath));
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
        ValidateMaterialEvaluationPaths(evaluation, materialPath);
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
        "textures",
        "subsurfaceProfile",
        "skinLut",
        "eyeProfile"
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
    if (materialInstanceJson.contains("subsurfaceProfile") &&
        (!materialInstanceJson["subsurfaceProfile"].is_string() ||
         materialInstanceJson["subsurfaceProfile"].get<std::string>().empty()))
    {
        throw std::runtime_error(
            "subsurfaceProfile must be a non-empty asset path string: " +
            std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("skinLut") &&
        (!materialInstanceJson["skinLut"].is_string() ||
         materialInstanceJson["skinLut"].get<std::string>().empty()))
    {
        throw std::runtime_error(
            "skinLut must be a non-empty asset path string: " +
            std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("eyeProfile") &&
        (!materialInstanceJson["eyeProfile"].is_string() ||
         materialInstanceJson["eyeProfile"].get<std::string>().empty()))
    {
        throw std::runtime_error(
            "eyeProfile must be a non-empty asset path string: " +
            std::string(materialInstancePath));
    }
}

void MaterialAssetValidator::ValidateInstanceOverrides(
    const nlohmann::json& materialJson,
    const nlohmann::json& materialInstanceJson,
    std::string_view materialInstancePath)
{
    const nlohmann::json emptyObject = nlohmann::json::object();
    const auto& macroOverrides = materialInstanceJson.value("macros", emptyObject);
    const auto& parameterOverrides = materialInstanceJson.value("parameters", emptyObject);
    const auto& textureOverrides = materialInstanceJson.value("textures", emptyObject);
    const auto& renderStateOverrides = materialInstanceJson.value("renderStateOverrides", emptyObject);

    if (materialInstanceJson.contains("macros"))
    {
        ValidateSparseOverrideObject(macroOverrides, "macros", materialInstancePath);
    }
    if (materialInstanceJson.contains("parameters"))
    {
        ValidateSparseOverrideObject(parameterOverrides, "parameters", materialInstancePath);
    }
    if (materialInstanceJson.contains("textures"))
    {
        ValidateSparseOverrideObject(textureOverrides, "textures", materialInstancePath);
    }
    if (materialInstanceJson.contains("renderStateOverrides"))
    {
        ValidateSparseOverrideObject(
            renderStateOverrides,
            "renderStateOverrides",
            materialInstancePath);
    }

    EnsureKnownOverrideKeys(
        macroOverrides,
        materialJson["macros"],
        "macro",
        materialInstancePath);
    EnsureKnownOverrideKeys(
        parameterOverrides,
        materialJson["parameters"],
        "parameter",
        materialInstancePath);
    EnsureKnownOverrideKeys(
        textureOverrides,
        materialJson["textures"],
        "texture",
        materialInstancePath);

    for (const auto& [name, value] : macroOverrides.items())
    {
        if (!MaterialAssetUtils::IsNumericMacroValue(value))
        {
            throw std::runtime_error(
                "Material instance macro overrides must be numeric: " +
                std::string(materialInstancePath));
        }
        if (value == materialJson["macros"].at(name))
        {
            throw std::runtime_error(
                "Material instance macro redundantly repeats M_ default \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    for (const auto& [name, value] : renderStateOverrides.items())
    {
        if (name == "shadingModel")
        {
            if (!value.is_string())
            {
                throw std::runtime_error(
                    "renderStateOverrides.shadingModel must be a string: " +
                    std::string(materialInstancePath));
            }
            MaterialAssetUtils::ShadingModelToId(value.get<std::string>());
            if (value == materialJson["shadingModel"])
            {
                throw std::runtime_error(
                    "Material instance redundantly repeats M_ shadingModel: " +
                    std::string(materialInstancePath));
            }
            continue;
        }

        if (!materialJson["renderStates"].contains(name))
        {
            throw std::runtime_error(
                "Material instance overrides unknown render state \"" + name + "\": " +
                std::string(materialInstancePath));
        }
        if (value == materialJson["renderStates"].at(name))
        {
            throw std::runtime_error(
                "Material instance redundantly repeats M_ render state \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    for (const auto& [name, value] : parameterOverrides.items())
    {
        const auto& descriptor = materialJson["parameters"].at(name);
        if (value == descriptor.at("default"))
        {
            throw std::runtime_error(
                "Material instance parameter redundantly repeats M_ default \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    for (const auto& [name, value] : textureOverrides.items())
    {
        const auto& descriptor = materialJson["textures"].at(name);
        if (value == descriptor.at("default"))
        {
            throw std::runtime_error(
                "Material instance texture redundantly repeats M_ default \"" + name +
                "\": " + std::string(materialInstancePath));
        }
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
