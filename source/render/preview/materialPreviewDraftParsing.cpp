#include "render/preview/materialPreviewDraftParsing.h"

#include <stdexcept>
#include <utility>

#include <Eigen/Dense>

#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"

namespace VL
{

PreviewRenderStateValues BuildPreviewRenderStateValues(
    const nlohmann::json& effectiveMaterialInstanceJson)
{
    PreviewRenderStateValues result;
    result.emplace(
        "shadingModel",
        effectiveMaterialInstanceJson.at("shadingModel").get<std::string>());
    const nlohmann::json& renderStates =
        effectiveMaterialInstanceJson.at("renderStates");
    if (!renderStates.is_object())
    {
        throw std::runtime_error(
            "Material instance preview render states must be a JSON object.");
    }
    for (const auto& [name, value] : renderStates.items())
    {
        if (!value.is_string())
        {
            throw std::runtime_error(
                "Material instance preview render state must be a string: " +
                name);
        }
        result.emplace(name, value.get<std::string>());
    }
    return result;
}

bool ReadFinitePreviewNumber(
    const nlohmann::json& value,
    float& outValue)
{
    if (!value.is_number())
    {
        return false;
    }

    const double doubleValue = value.get<double>();
    const float floatValue = static_cast<float>(doubleValue);
    if (!std::isfinite(doubleValue) || !std::isfinite(floatValue))
    {
        return false;
    }
    outValue = floatValue;
    return true;
}
NumericPreviewDraftParseResult ParseNumericPreviewDraft(
    const VL::MaterialInstancePreviewDraft& draft,
    const VL::NormalizedMaterialInstancePath& expectedPath,
    const MaterialInstanceStateSnapshot::TextureMap& activeTextures,
    const nlohmann::json& frozenDefinition)
{
    NumericPreviewDraftParseResult result;
    const VL::MaterialInstancePreviewPathNormalizationResult
        draftPath =
            VL::NormalizeMaterialInstancePath(
                draft.materialInstancePath);
    if (!draftPath.Succeeded())
    {
        result.errorMessage = draftPath.errorMessage;
        return result;
    }
    if (draftPath.path->value != expectedPath.value)
    {
        result.errorMessage =
            "Material instance preview draft does not match the connected MI path.";
        return result;
    }
    if (draft.serializedWorkingDraft.empty())
    {
        result.errorMessage =
            "Material instance preview draft must contain serialized content.";
        return result;
    }

    try
    {
        const nlohmann::json materialInstanceJson =
            nlohmann::json::parse(draft.serializedWorkingDraft);
        if (!materialInstanceJson.is_object())
        {
            result.errorMessage =
                "Material instance preview draft must be a JSON object.";
            return result;
        }
        if (materialInstanceJson.contains("textures"))
        {
            const nlohmann::json& textures = materialInstanceJson.at("textures");
            if (!textures.is_object())
            {
                result.errorMessage =
                    "Material instance preview textures must be a JSON object.";
                return result;
            }

            // 数值 preview 不创建 descriptor；只有能用 active MI 的稳定 asset
            // identity 证明纹理未变化时，才允许携带原有 texture override。
            for (const auto& [textureName, textureValue] : textures.items())
            {
                if (!textureValue.is_string())
                {
                    result.errorMessage =
                        "Material instance texture preview changes are unavailable: " +
                        textureName;
                    return result;
                }

                const auto activeTextureIt = activeTextures.find(textureName);
                if (activeTextureIt == activeTextures.end() ||
                    !activeTextureIt->second.textureAssetIdentity.has_value() ||
                    MaterialAssetUtils::NormalizeAssetPath(
                        textureValue.get<std::string>()) !=
                        MaterialAssetUtils::NormalizeAssetPath(
                            *activeTextureIt->second.textureAssetIdentity))
                {
                    result.errorMessage =
                        "Material instance texture preview changes are unavailable: " +
                        textureName;
                    return result;
                }
            }
        }

        const MaterialInstanceResolveResult resolved =
            MaterialInstanceResolver::ResolveWithDefinition(
                expectedPath.value,
                materialInstanceJson, frozenDefinition);
        PreviewRenderStateValues renderStates =
            BuildPreviewRenderStateValues(
                resolved.effectiveMaterialInstanceJson);
        result.renderStates = renderStates;
        result.serializedJson = materialInstanceJson;
        const nlohmann::json& parameters =
            resolved.effectiveMaterialInstanceJson.at("parameters");
        if (!parameters.is_object())
        {
            result.errorMessage =
                "Material instance preview parameters must be a JSON object.";
            return result;
        }

        MaterialInstanceNumericParameterValues values;
        for (const auto& [parameterName, parameterValue] : parameters.items())
        {
            if (parameterValue.is_number())
            {
                float scalarValue = 0.0f;
                if (!ReadFinitePreviewNumber(parameterValue, scalarValue))
                {
                    result.errorMessage =
                        "Material instance preview numeric value is invalid: " +
                        parameterName;
                    return result;
                }
                values.emplace(parameterName, scalarValue);
                continue;
            }

            if (!parameterValue.is_array() ||
                (parameterValue.size() != 2 &&
                 parameterValue.size() != 3 &&
                 parameterValue.size() != 4))
            {
                result.errorMessage =
                    "Material instance preview parameter must be float, vec2, vec3, or vec4: " +
                    parameterName;
                return result;
            }

            float components[4] = {};
            for (size_t componentIndex = 0;
                 componentIndex < parameterValue.size();
                 ++componentIndex)
            {
                if (!ReadFinitePreviewNumber(
                        parameterValue.at(componentIndex),
                        components[componentIndex]))
                {
                    result.errorMessage =
                        "Material instance preview numeric value is invalid: " +
                        parameterName;
                    return result;
                }
            }

            if (parameterValue.size() == 2)
            {
                values.emplace(
                    parameterName,
                    Eigen::Vector2f(components[0], components[1]));
            }
            else if (parameterValue.size() == 3)
            {
                values.emplace(
                    parameterName,
                    Eigen::Vector3f(
                        components[0],
                        components[1],
                        components[2]));
            }
            else
            {
                values.emplace(
                    parameterName,
                    Eigen::Vector4f(
                        components[0],
                        components[1],
                        components[2],
                        components[3]));
            }
        }
        result.values = std::move(values);
    }
    catch (const std::exception& exception)
    {
        result.errorMessage =
            std::string("Material instance preview draft validation failed: ") +
            exception.what();
    }
    return result;
}
} // namespace VL

