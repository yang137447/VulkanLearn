#include "render/eye/eyeMaterialContract.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include "materialInstance.h"
#include "render/eye/eyeResourceSet.h"
#include "render/subsurface/subsurfaceResourceSet.h"

namespace VL
{
namespace
{

std::array<float, 4> RequireVec4Parameter(
    const nlohmann::json& effectiveJson,
    std::string_view parameterName,
    std::string_view materialInstancePath)
{
    const std::string name(parameterName);
    if (!effectiveJson.contains("parameters") ||
        !effectiveJson.at("parameters").is_object() ||
        !effectiveJson.at("parameters").contains(name) ||
        !effectiveJson.at("parameters").at(name).is_array() ||
        effectiveJson.at("parameters").at(name).size() != 4)
    {
        throw std::runtime_error(
            "Eye material requires vec4 parameter \"" + name + "\": " +
            std::string(materialInstancePath));
    }

    std::array<float, 4> value{};
    for (size_t index = 0; index < value.size(); ++index)
    {
        if (!effectiveJson.at("parameters").at(name).at(index).is_number())
        {
            throw std::runtime_error(
                "Eye vec4 parameter contains a non-number \"" + name +
                "\": " + std::string(materialInstancePath));
        }
        value[index] = effectiveJson.at("parameters").at(name).at(index).get<float>();
        if (!std::isfinite(value[index]))
        {
            throw std::runtime_error(
                "Eye vec4 parameter contains a non-finite value \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }
    return value;
}

float RequireFloatParameter(
    const nlohmann::json& effectiveJson,
    std::string_view parameterName,
    std::string_view materialInstancePath)
{
    const std::string name(parameterName);
    if (!effectiveJson.contains("parameters") ||
        !effectiveJson.at("parameters").is_object() ||
        !effectiveJson.at("parameters").contains(name) ||
        !effectiveJson.at("parameters").at(name).is_number())
    {
        throw std::runtime_error(
            "Eye material requires float parameter \"" + name + "\": " +
            std::string(materialInstancePath));
    }
    const float value = effectiveJson.at("parameters").at(name).get<float>();
    if (!std::isfinite(value))
    {
        throw std::runtime_error(
            "Eye material float parameter is non-finite \"" + name + "\": " +
            std::string(materialInstancePath));
    }
    return value;
}

void RequireRange(
    float value,
    float lower,
    float upper,
    const char* field,
    std::string_view materialInstancePath)
{
    if (value < lower || value > upper)
    {
        throw std::runtime_error(
            "Eye material parameter is outside the frozen range \"" +
            std::string(field) + "\": " + std::string(materialInstancePath));
    }
}

std::string RequireAssetReference(
    const nlohmann::json& materialInstanceJson,
    const char* field,
    std::string_view materialInstancePath)
{
    if (!materialInstanceJson.contains(field) ||
        !materialInstanceJson.at(field).is_string() ||
        materialInstanceJson.at(field).get<std::string>().empty())
    {
        throw std::runtime_error(
            "Eye material requires non-empty asset field \"" +
            std::string(field) + "\": " + std::string(materialInstancePath));
    }
    return materialInstanceJson.at(field).get<std::string>();
}

void SetDerivedParameter(
    nlohmann::json& effectiveJson,
    const char* name,
    float value)
{
    effectiveJson["parameters"][name] = value;
}

void ValidateEyeAuthoringParameters(
    const nlohmann::json& effectiveJson,
    const EyeProfileAsset& profile,
    std::string_view materialInstancePath)
{
    const std::array<float, 4> surface =
        RequireVec4Parameter(effectiveJson, "u_eyeSurface", materialInstancePath);
    const std::array<float, 4> geometry =
        RequireVec4Parameter(effectiveJson, "u_eyeGeometry", materialInstancePath);
    const std::array<float, 4> irisColor =
        RequireVec4Parameter(effectiveJson, "u_eyeIrisColor", materialInstancePath);
    const std::array<float, 4> scleraColor =
        RequireVec4Parameter(effectiveJson, "u_eyeScleraColor", materialInstancePath);
    const std::array<float, 4> gaze =
        RequireVec4Parameter(effectiveJson, "u_eyeGaze", materialInstancePath);
    const float eyeLayer = RequireFloatParameter(
        effectiveJson,
        "u_eyeLayer",
        materialInstancePath);
    const float contactVisibility = RequireFloatParameter(
        effectiveJson,
        "u_eyeContactVisibility",
        materialInstancePath);
    const float ciliaVisibility = RequireFloatParameter(
        effectiveJson,
        "u_eyeCiliaVisibility",
        materialInstancePath);
    const float uvHandedness = RequireFloatParameter(
        effectiveJson,
        "u_eyeUvHandedness",
        materialInstancePath);
    const float pupilDilation = RequireFloatParameter(
        effectiveJson,
        "u_eyePupilDilation",
        materialInstancePath);

    RequireRange(surface[0], 0.001f, 1.0f, "u_eyeSurface.x roughness", materialInstancePath);
    RequireRange(surface[1], 0.0f, 1.0f, "u_eyeSurface.y irisMask", materialInstancePath);
    RequireRange(surface[2], 0.0f, 1.0f, "u_eyeSurface.z ambientOcclusion", materialInstancePath);
    if (surface[3] != 0.0f)
    {
        throw std::runtime_error(
            "u_eyeSurface.w is reserved and must be zero: " +
            std::string(materialInstancePath));
    }
    RequireRange(eyeLayer, 0.0f, 2.0f, "u_eyeLayer", materialInstancePath);
    RequireRange(
        contactVisibility,
        0.0f,
        1.0f,
        "u_eyeContactVisibility",
        materialInstancePath);
    RequireRange(
        ciliaVisibility,
        0.0f,
        1.0f,
        "u_eyeCiliaVisibility",
        materialInstancePath);
    RequireRange(
        pupilDilation,
        0.0f,
        1.0f,
        "u_eyePupilDilation",
        materialInstancePath);
    if (std::abs(std::abs(uvHandedness) - 1.0f) > 1.0e-4f)
    {
        throw std::runtime_error(
            "u_eyeUvHandedness must be either -1 or 1: " +
            std::string(materialInstancePath));
    }
    const float gazeLength = std::sqrt(
        gaze[0] * gaze[0] + gaze[1] * gaze[1] + gaze[2] * gaze[2]);
    if (!(gazeLength > 1.0e-5f) || gaze[3] < 0.0f || gaze[3] > 1.0f)
    {
        throw std::runtime_error(
            "u_eyeGaze must contain a non-zero direction and weight in [0,1]: " +
            std::string(materialInstancePath));
    }

    if (!(geometry[0] > 0.0f && geometry[0] < profile.corneaRadius) ||
        !(geometry[1] > 0.0f && geometry[1] < profile.corneaRadius) ||
        !(geometry[2] > 0.0f && geometry[2] < geometry[1]) ||
        !(geometry[3] > 0.0f && geometry[3] < geometry[1]))
    {
        throw std::runtime_error(
            "Eye geometry must remain inside the selected profile domain: " +
            std::string(materialInstancePath));
    }
    if (geometry[2] < profile.pupilRadiusMin ||
        geometry[2] > profile.pupilRadiusMax)
    {
        throw std::runtime_error(
            "Eye pupil radius is outside the selected profile range: " +
            std::string(materialInstancePath));
    }

    for (size_t channel = 0; channel < 3; ++channel)
    {
        RequireRange(
            irisColor[channel],
            0.0f,
            1.0f,
            "u_eyeIrisColor",
            materialInstancePath);
        RequireRange(
            scleraColor[channel],
            0.0f,
            1.0f,
            "u_eyeScleraColor",
            materialInstancePath);
    }

    const std::string renderMode = effectiveJson.contains("renderStates")
        ? effectiveJson.at("renderStates").value(
            "renderMode",
            std::string("Opaque"))
        : std::string("Opaque");
    const float expectedLayer =
        renderMode == "ForwardEyeInner" ? 1.0f :
        renderMode == "ForwardEyeCornea" ? 2.0f : 0.0f;
    if (std::abs(eyeLayer - expectedLayer) > 1.0e-4f)
    {
        throw std::runtime_error(
            "Eye layer does not match renderMode: " +
            std::string(materialInstancePath));
    }
}

} // namespace

ResolvedEyeMaterialAssets ResolveEyeMaterialContract(
    const nlohmann::json& materialInstanceJson,
    nlohmann::json& effectiveMaterialInstanceJson,
    const EyeResourceSet* eyeResources,
    const SubsurfaceResourceSet& subsurfaceResources,
    std::string_view materialInstancePath)
{
    ResolvedEyeMaterialAssets resolvedAssets;
    const std::string shadingModel =
        effectiveMaterialInstanceJson.at("shadingModel").get<std::string>();
    const bool isEye = shadingModel == "Eye";

    if (!isEye)
    {
        if (materialInstanceJson.contains("eyeProfile"))
        {
            throw std::runtime_error(
                "eyeProfile requires the Eye shading model: " +
                std::string(materialInstancePath));
        }
        return resolvedAssets;
    }
    if (eyeResources == nullptr)
    {
        throw std::runtime_error(
            "Eye material requires an active Eye resource set: " +
            std::string(materialInstancePath));
    }

    const std::string profilePath = RequireAssetReference(
        materialInstanceJson,
        "eyeProfile",
        materialInstancePath);
    resolvedAssets.profileId = eyeResources->ResolveProfileId(profilePath);
    const EyeProfileAsset& profile =
        eyeResources->GetProfile(*resolvedAssets.profileId);
    resolvedAssets.corneaIor = profile.ior;
    resolvedAssets.causticStrength = profile.causticStrength;

    SetDerivedParameter(
        effectiveMaterialInstanceJson,
        "u_eyeProfileId",
        static_cast<float>(*resolvedAssets.profileId));
    SetDerivedParameter(
        effectiveMaterialInstanceJson,
        "u_eyeCorneaIor",
        profile.ior);
    SetDerivedParameter(
        effectiveMaterialInstanceJson,
        "u_eyeCausticStrength",
        profile.causticStrength);

    if (materialInstanceJson.contains("subsurfaceProfile"))
    {
        const std::string subsurfacePath = RequireAssetReference(
            materialInstanceJson,
            "subsurfaceProfile",
            materialInstancePath);
        resolvedAssets.scleraProfileId =
            subsurfaceResources.ResolveProfileId(subsurfacePath);
        SetDerivedParameter(
            effectiveMaterialInstanceJson,
            "u_eyeScleraProfileId",
            static_cast<float>(*resolvedAssets.scleraProfileId));
    }
    else
    {
        SetDerivedParameter(
            effectiveMaterialInstanceJson,
            "u_eyeScleraProfileId",
            0.0f);
    }
    if (materialInstanceJson.contains("skinLut"))
    {
        throw std::runtime_error(
            "Eye material cannot bind a preintegrated skin LUT: " +
            std::string(materialInstancePath));
    }

    ValidateEyeAuthoringParameters(
        effectiveMaterialInstanceJson,
        profile,
        materialInstancePath);
    return resolvedAssets;
}

void ReapplyResolvedEyeMaterialIds(
    const ResolvedEyeMaterialAssets& resolvedAssets,
    ::MaterialInstance& materialInstance)
{
    if (!resolvedAssets.profileId)
    {
        // 非 Eye 材质不能写入 Eye 专用参数，否则会破坏其严格的 M_ schema。
        return;
    }
    materialInstance.SetParameter(
        "u_eyeProfileId",
        static_cast<float>(*resolvedAssets.profileId));
    materialInstance.SetParameter(
        "u_eyeCorneaIor",
        resolvedAssets.corneaIor);
    materialInstance.SetParameter(
        "u_eyeCausticStrength",
        resolvedAssets.causticStrength);
    materialInstance.SetParameter(
        "u_eyeScleraProfileId",
        resolvedAssets.scleraProfileId
            ? static_cast<float>(*resolvedAssets.scleraProfileId)
            : 0.0f);
}

} // namespace VL
