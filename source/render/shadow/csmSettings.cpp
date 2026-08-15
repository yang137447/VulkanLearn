#include "render/shadow/csmSettings.h"

#include <cmath>
#include <string>
#include <utility>

namespace VL
{

namespace
{

RuntimeResult<CsmSettings> InvalidSceneCsm(
    std::string message,
    std::string_view scenePath,
    std::string field)
{
    return RuntimeResult<CsmSettings>::Failure(MakeRuntimeError(
        "Scene.InvalidDirectionalLightShadow",
        std::move(message),
        std::string(scenePath),
        std::move(field)));
}

bool IsFiniteNumber(const nlohmann::json& value)
{
    return value.is_number() &&
        std::isfinite(value.get<double>());
}

bool IsSupportedShadowField(std::string_view field)
{
    return field == "castShadows" ||
        field == "dynamicShadowDistance" ||
        field == "dynamicShadowCascades" ||
        field == "cascadeDistributionExponent" ||
        field == "cascadeTransitionFraction" ||
        field == "shadowDistanceFadeoutFraction" ||
        field == "shadowBias" ||
        field == "shadowSlopeBias" ||
        field == "shadowCascadeBiasDistribution";
}

RuntimeResult<void> ReadOptionalUnitFloat(
    const nlohmann::json& objectJson,
    const char* fieldName,
    float& value,
    std::string_view scenePath)
{
    if (!objectJson.contains(fieldName))
    {
        return RuntimeResult<void>::Success();
    }
    if (!IsFiniteNumber(objectJson[fieldName]))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "Scene.InvalidDirectionalLightShadow",
            std::string("DirectionalLight.shadow.") + fieldName +
                " must be a finite number.",
            std::string(scenePath),
            std::string("directionalLight.shadow.") + fieldName));
    }

    value = objectJson[fieldName].get<float>();
    if (value < 0.0f || value > 1.0f)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "Scene.InvalidDirectionalLightShadow",
            std::string("DirectionalLight.shadow.") + fieldName +
                " must be in the range [0, 1].",
            std::string(scenePath),
            std::string("directionalLight.shadow.") + fieldName));
    }
    return RuntimeResult<void>::Success();
}

} // namespace

RuntimeResult<CsmSettings> BuildDirectionalLightCsmSettings(
    const nlohmann::json& directionalLightJson,
    uint32_t shadowCascadeCapacity,
    std::string_view scenePath)
{
    CsmSettings settings;
    settings.cascadeCount = shadowCascadeCapacity;
    if (shadowCascadeCapacity != CsmSettings::MaxCascadeCount)
    {
        return InvalidSceneCsm(
            "M1 directional-light CSM requires renderGraph shadowMap.arrayLayers == 4.",
            scenePath,
            "renderGraph.resources.shadowMap.arrayLayers");
    }

    if (!directionalLightJson.contains("shadow"))
    {
        return RuntimeResult<CsmSettings>::Success(std::move(settings));
    }

    const nlohmann::json& shadowJson =
        directionalLightJson["shadow"];
    if (!shadowJson.is_object())
    {
        return InvalidSceneCsm(
            "DirectionalLight.shadow must be an object.",
            scenePath,
            "directionalLight.shadow");
    }

    for (const auto& [field, value] : shadowJson.items())
    {
        (void)value;
        if (!IsSupportedShadowField(field))
        {
            return InvalidSceneCsm(
                std::string("Unsupported DirectionalLight.shadow field: ") +
                    field,
                scenePath,
                std::string("directionalLight.shadow.") +
                    field);
        }
    }

    if (shadowJson.contains("castShadows"))
    {
        if (!shadowJson["castShadows"].is_boolean())
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.castShadows must be a boolean.",
                scenePath,
                "directionalLight.shadow.castShadows");
        }
        settings.castShadows =
            shadowJson["castShadows"].get<bool>();
    }

    if (shadowJson.contains("dynamicShadowDistance"))
    {
        if (!IsFiniteNumber(
                shadowJson["dynamicShadowDistance"]))
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.dynamicShadowDistance must be a finite number.",
                scenePath,
                "directionalLight.shadow.dynamicShadowDistance");
        }
        settings.dynamicShadowDistance =
            shadowJson["dynamicShadowDistance"].get<float>();
        if (settings.dynamicShadowDistance <= 0.0f)
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.dynamicShadowDistance must be positive.",
                scenePath,
                "directionalLight.shadow.dynamicShadowDistance");
        }
    }

    if (shadowJson.contains("dynamicShadowCascades"))
    {
        if (!shadowJson["dynamicShadowCascades"].is_number_unsigned() &&
            !shadowJson["dynamicShadowCascades"].is_number_integer())
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.dynamicShadowCascades must be an integer.",
                scenePath,
                "directionalLight.shadow.dynamicShadowCascades");
        }
        const int cascadeCount =
            shadowJson["dynamicShadowCascades"].get<int>();
        if (cascadeCount < 1 ||
            cascadeCount > static_cast<int>(shadowCascadeCapacity))
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.dynamicShadowCascades must be between 1 and the RenderGraph cascade capacity.",
                scenePath,
                "directionalLight.shadow.dynamicShadowCascades");
        }
        settings.cascadeCount =
            static_cast<uint32_t>(cascadeCount);
    }

    if (shadowJson.contains("cascadeDistributionExponent"))
    {
        if (!IsFiniteNumber(
                shadowJson["cascadeDistributionExponent"]))
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.cascadeDistributionExponent must be a finite number.",
                scenePath,
                "directionalLight.shadow.cascadeDistributionExponent");
        }
        settings.cascadeDistributionExponent =
            shadowJson["cascadeDistributionExponent"].get<float>();
        if (settings.cascadeDistributionExponent < 1.0f ||
            settings.cascadeDistributionExponent > 10.0f)
        {
            return InvalidSceneCsm(
                "DirectionalLight.shadow.cascadeDistributionExponent must be in the range [1, 10].",
                scenePath,
                "directionalLight.shadow.cascadeDistributionExponent");
        }
    }
    RuntimeResult<void> valueResult = ReadOptionalUnitFloat(
        shadowJson,
        "cascadeTransitionFraction",
        settings.cascadeTransitionFraction,
        scenePath);
    if (valueResult.IsFailure())
    {
        return RuntimeResult<CsmSettings>::Failure(valueResult.Error());
    }
    valueResult = ReadOptionalUnitFloat(
        shadowJson,
        "shadowDistanceFadeoutFraction",
        settings.shadowDistanceFadeoutFraction,
        scenePath);
    if (valueResult.IsFailure())
    {
        return RuntimeResult<CsmSettings>::Failure(valueResult.Error());
    }
    valueResult = ReadOptionalUnitFloat(
        shadowJson,
        "shadowBias",
        settings.shadowBias,
        scenePath);
    if (valueResult.IsFailure())
    {
        return RuntimeResult<CsmSettings>::Failure(valueResult.Error());
    }
    valueResult = ReadOptionalUnitFloat(
        shadowJson,
        "shadowSlopeBias",
        settings.shadowSlopeBias,
        scenePath);
    if (valueResult.IsFailure())
    {
        return RuntimeResult<CsmSettings>::Failure(valueResult.Error());
    }
    valueResult = ReadOptionalUnitFloat(
        shadowJson,
        "shadowCascadeBiasDistribution",
        settings.shadowCascadeBiasDistribution,
        scenePath);
    if (valueResult.IsFailure())
    {
        return RuntimeResult<CsmSettings>::Failure(valueResult.Error());
    }

    return RuntimeResult<CsmSettings>::Success(std::move(settings));
}

} // namespace VL
