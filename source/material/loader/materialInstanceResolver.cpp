#include "materialInstanceResolver.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../commonFunction.h"
#include "../materialAssetUtils.h"
#include "../validation/materialAssetValidator.h"

namespace
{
    std::string InferShaderNameFromMaterialPath(std::string_view materialPath)
    {
        std::filesystem::path path{std::string(materialPath)};
        std::vector<std::string> parts;
        bool foundGlslSegment = false;
        for (const auto& part : path)
        {
            const std::string partString = part.string();
            if (foundGlslSegment)
            {
                parts.push_back(partString);
            }
            else if (partString == "glsl")
            {
                foundGlslSegment = true;
            }
        }
        if (parts.empty())
        {
            throw std::runtime_error("Material definition must live under shader/glsl with its shader pair: " + std::string(materialPath));
        }

        std::filesystem::path shaderPath;
        for (size_t i = 0; i + 1 < parts.size(); ++i)
        {
            shaderPath /= parts[i];
        }

        std::string stem = std::filesystem::path(parts.back()).stem().string();
        if (stem.rfind("M_", 0) != 0)
        {
            throw std::runtime_error("Material definition filename must start with M_: " + std::string(materialPath));
        }
        stem = stem.substr(2);
        shaderPath /= stem;
        return shaderPath.generic_string();
    }

    nlohmann::json BuildEffectiveParameters(
        const nlohmann::json& materialJson,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath)
    {
        const nlohmann::json instanceOverrides = materialInstanceJson.value("parameters", nlohmann::json::object());
        MaterialAssetValidator::EnsureKnownOverrideKeys(instanceOverrides, materialJson["parameters"], "parameter", materialInstancePath);

        nlohmann::json parameters = nlohmann::json::object();
        for (const auto& [name, paramDesc] : materialJson["parameters"].items())
        {
            parameters[name] = instanceOverrides.contains(name) ? instanceOverrides[name] : paramDesc["default"];
        }
        return parameters;
    }

    nlohmann::json BuildEffectiveTextures(
        const nlohmann::json& materialJson,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath)
    {
        const nlohmann::json instanceOverrides = materialInstanceJson.value("textures", nlohmann::json::object());
        MaterialAssetValidator::EnsureKnownOverrideKeys(instanceOverrides, materialJson["textures"], "texture", materialInstancePath);

        nlohmann::json textures = nlohmann::json::object();
        for (const auto& [name, textureDesc] : materialJson["textures"].items())
        {
            const nlohmann::json value = instanceOverrides.contains(name) ? instanceOverrides[name] : textureDesc["default"];
            if (!value.is_null())
            {
                textures[name] = value;
            }
        }
        return textures;
    }

    nlohmann::json BuildEffectiveMacros(
        const nlohmann::json& materialJson,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath)
    {
        const nlohmann::json instanceOverrides = materialInstanceJson.value("macros", nlohmann::json::object());
        MaterialAssetValidator::EnsureKnownOverrideKeys(instanceOverrides, materialJson["macros"], "macro", materialInstancePath);

        nlohmann::json macros = materialJson["macros"];
        for (const auto& [name, value] : instanceOverrides.items())
        {
            if (!MaterialAssetUtils::IsNumericMacroValue(value))
            {
                throw std::runtime_error("Material instance macro overrides must be numeric: " + std::string(materialInstancePath));
            }
            macros[name] = value;
        }
        return macros;
    }

    nlohmann::json BuildRenderStates(
        const nlohmann::json& materialJson,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath)
    {
        nlohmann::json renderStates = materialJson["renderStates"];
        if (materialInstanceJson.contains("renderStateOverrides"))
        {
            for (const auto& [name, value] : materialInstanceJson["renderStateOverrides"].items())
            {
                if (name == "shadingModel")
                {
                    if (!value.is_string())
                    {
                        throw std::runtime_error("renderStateOverrides.shadingModel must be a string: " + std::string(materialInstancePath));
                    }
                    MaterialAssetUtils::ShadingModelToId(value.get<std::string>());
                    continue;
                }
                if (!materialJson["renderStates"].contains(name))
                {
                    throw std::runtime_error(
                        "Material instance overrides unknown render state \"" + name + "\": " +
                        std::string(materialInstancePath));
                }
                renderStates[name] = value;
            }
        }
        return renderStates;
    }

    std::string BuildEffectiveShadingModel(
        const nlohmann::json& materialJson,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath)
    {
        std::string shadingModel = materialJson["shadingModel"].get<std::string>();
        if (!materialInstanceJson.contains("renderStateOverrides"))
        {
            return shadingModel;
        }

        const auto& renderStateOverrides = materialInstanceJson["renderStateOverrides"];
        if (!renderStateOverrides.contains("shadingModel"))
        {
            return shadingModel;
        }

        const auto& overrideValue = renderStateOverrides["shadingModel"];
        if (!overrideValue.is_string())
        {
            throw std::runtime_error("renderStateOverrides.shadingModel must be a string: " + std::string(materialInstancePath));
        }

        shadingModel = overrideValue.get<std::string>();
        MaterialAssetUtils::ShadingModelToId(shadingModel);
        return shadingModel;
    }
}

nlohmann::json MaterialInstanceResolver::LoadDefinition(std::string_view materialPath)
{
    std::ifstream materialFile(CommonFunction::Path(std::string(materialPath)));
    if (!materialFile.is_open())
    {
        throw std::runtime_error("Failed to open material definition: " + std::string(materialPath));
    }

    nlohmann::json materialJson;
    materialFile >> materialJson;
    MaterialAssetValidator::ValidateDefinition(materialJson, materialPath);
    return materialJson;
}

MaterialInstanceResolveResult MaterialInstanceResolver::Resolve(
    std::string_view materialInstancePath,
    const nlohmann::json& materialInstanceJson)
{
    MaterialAssetValidator::ValidateInstanceHeader(materialInstanceJson, materialInstancePath);

    MaterialInstanceResolveResult result;
    result.materialPath = materialInstanceJson["material"].get<std::string>();
    result.materialJson = LoadDefinition(result.materialPath);
    result.shaderName = InferShaderNameFromMaterialPath(result.materialPath);

    result.effectiveMaterialInstanceJson = nlohmann::json::object();
    result.effectiveMaterialInstanceJson["type"] = "materialInstance";
    result.effectiveMaterialInstanceJson["name"] = materialInstanceJson.value("name", std::string(materialInstancePath));
    result.effectiveMaterialInstanceJson["material"] = result.materialPath;
    result.effectiveMaterialInstanceJson["shaderName"] = result.shaderName;
    result.effectiveMaterialInstanceJson["shadingModel"] = BuildEffectiveShadingModel(result.materialJson, materialInstanceJson, materialInstancePath);
    result.effectiveMaterialInstanceJson["renderStates"] = BuildRenderStates(result.materialJson, materialInstanceJson, materialInstancePath);
    result.effectiveMaterialInstanceJson["macros"] = BuildEffectiveMacros(result.materialJson, materialInstanceJson, materialInstancePath);
    result.effectiveMaterialInstanceJson["parameters"] = BuildEffectiveParameters(result.materialJson, materialInstanceJson, materialInstancePath);
    result.effectiveMaterialInstanceJson["textures"] = BuildEffectiveTextures(result.materialJson, materialInstanceJson, materialInstancePath);
    return result;
}
