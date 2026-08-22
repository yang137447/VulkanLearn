#include "render/subsurface/subsurfaceMaterialContract.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include "materialInstance.h"
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
    const nlohmann::json& parameters = effectiveJson.at("parameters");
    if (!parameters.contains(name) ||
        !parameters[name].is_array() ||
        parameters[name].size() != 4)
    {
        throw std::runtime_error(
            "Subsurface material requires vec4 parameter \"" + name +
            "\": " + std::string(materialInstancePath));
    }

    std::array<float, 4> value{};
    for (size_t index = 0; index < value.size(); ++index)
    {
        if (!parameters[name][index].is_number())
        {
            throw std::runtime_error(
                "Subsurface vec4 parameter contains a non-number \"" +
                name + "\": " + std::string(materialInstancePath));
        }
        value[index] = parameters[name][index].get<float>();
        if (std::isfinite(value[index]) == 0)
        {
            throw std::runtime_error(
                "Subsurface vec4 parameter contains a non-finite value \"" +
                name + "\": " + std::string(materialInstancePath));
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
    const nlohmann::json& parameters = effectiveJson.at("parameters");
    if (!parameters.contains(name) || !parameters[name].is_number())
    {
        throw std::runtime_error(
            "Subsurface material requires float parameter \"" + name +
            "\": " + std::string(materialInstancePath));
    }
    const float value = parameters[name].get<float>();
    if (std::isfinite(value) == 0)
    {
        throw std::runtime_error(
            "Subsurface float parameter is non-finite \"" + name +
            "\": " + std::string(materialInstancePath));
    }
    return value;
}

void RequireUnitInterval(
    float value,
    std::string_view field,
    std::string_view materialInstancePath)
{
    if (value < 0.0f || value > 1.0f)
    {
        throw std::runtime_error(
            std::string(field) + " must be inside [0, 1]: " +
            std::string(materialInstancePath));
    }
}

std::string RequireAssetReference(
    const nlohmann::json& materialInstanceJson,
    std::string_view field,
    std::string_view materialInstancePath)
{
    const std::string fieldName(field);
    if (!materialInstanceJson.contains(fieldName) ||
        !materialInstanceJson[fieldName].is_string() ||
        materialInstanceJson[fieldName].get<std::string>().empty())
    {
        throw std::runtime_error(
            "Material instance requires non-empty string field \"" +
            fieldName + "\": " + std::string(materialInstancePath));
    }
    return materialInstanceJson[fieldName].get<std::string>();
}

void ValidateSubsurfaceMaterial(
    const nlohmann::json& effectiveJson,
    std::string_view materialInstancePath)
{
    const std::array<float, 4> colorWeight =
        RequireVec4Parameter(
            effectiveJson,
            "u_subsurfaceColorWeight",
            materialInstancePath);
    const std::array<float, 4> shape =
        RequireVec4Parameter(
            effectiveJson,
            "u_subsurfaceShape",
            materialInstancePath);
    const float transmissionWeight = RequireFloatParameter(
        effectiveJson,
        "u_subsurfaceTransmissionWeight",
        materialInstancePath);

    for (size_t channel = 0; channel < 3; ++channel)
    {
        if (colorWeight[channel] <= 0.0f ||
            colorWeight[channel] > 1.0f)
        {
            throw std::runtime_error(
                "Subsurface color must be inside (0, 1]: " +
                std::string(materialInstancePath));
        }
    }
    RequireUnitInterval(
        colorWeight[3],
        "Subsurface weight",
        materialInstancePath);
    RequireUnitInterval(
        shape[0],
        "Subsurface wrap width",
        materialInstancePath);
    if (shape[1] <= 0.0f)
    {
        throw std::runtime_error(
            "Subsurface backscatter power must be positive: " +
            std::string(materialInstancePath));
    }
    RequireUnitInterval(
        shape[2],
        "Subsurface backscatter weight",
        materialInstancePath);
    if (shape[3] <= 0.0f && transmissionWeight > 0.0f)
    {
        throw std::runtime_error(
            "Subsurface transmission requires positive thickness: " +
            std::string(materialInstancePath));
    }
    RequireUnitInterval(
        transmissionWeight,
        "Subsurface transmission weight",
        materialInstancePath);
}

void ValidatePreintegratedSkinMaterial(
    const nlohmann::json& effectiveJson,
    const PreintegratedSkinLutAsset& lut,
    std::string_view materialInstancePath)
{
    const std::array<float, 4> surface =
        RequireVec4Parameter(
            effectiveJson,
            "u_skinSurface",
            materialInstancePath);
    const float transmissionWeight = RequireFloatParameter(
        effectiveJson,
        "u_skinTransmissionWeight",
        materialInstancePath);
    if (surface[0] <= 0.0f || surface[1] <= 0.0f)
    {
        throw std::runtime_error(
            "Preintegrated skin thickness and thicknessScale must be positive: " +
            std::string(materialInstancePath));
    }
    if (surface[0] * surface[1] >
        lut.thicknessMax * lut.worldUnitScale)
    {
        throw std::runtime_error(
            "Preintegrated skin thickness exceeds the selected LUT domain: " +
            std::string(materialInstancePath));
    }
    RequireUnitInterval(
        surface[2],
        "Preintegrated skin weight",
        materialInstancePath);
    if (surface[3] != 0.0f)
    {
        throw std::runtime_error(
            "Preintegrated skin LUT v1 requires curvature == 0: " +
            std::string(materialInstancePath));
    }
    RequireUnitInterval(
        transmissionWeight,
        "Preintegrated skin transmission weight",
        materialInstancePath);
}

void ValidateSubsurfaceProfileMaterial(
    const nlohmann::json& effectiveJson,
    std::string_view materialInstancePath)
{
    const std::array<float, 4> surface =
        RequireVec4Parameter(
            effectiveJson,
            "u_subsurfaceProfileSurface",
            materialInstancePath);
    RequireUnitInterval(
        surface[0],
        "Subsurface profile weight",
        materialInstancePath);
    if (surface[1] <= 0.0f && surface[2] > 0.0f)
    {
        throw std::runtime_error(
            "Subsurface profile transmission requires positive thickness: " +
            std::string(materialInstancePath));
    }
    RequireUnitInterval(
        surface[2],
        "Subsurface profile transmission weight",
        materialInstancePath);
    if (surface[3] != 0.0f)
    {
        throw std::runtime_error(
            "u_subsurfaceProfileSurface.w is reserved and must be zero: " +
            std::string(materialInstancePath));
    }
}

} // namespace

ResolvedSubsurfaceMaterialAssets ResolveSubsurfaceMaterialContract(
    const nlohmann::json& materialInstanceJson,
    nlohmann::json& effectiveMaterialInstanceJson,
    const SubsurfaceResourceSet& resourceSet,
    std::string_view materialInstancePath)
{
    ResolvedSubsurfaceMaterialAssets resolvedAssets;
    const std::string shadingModel =
        effectiveMaterialInstanceJson.at("shadingModel")
            .get<std::string>();

    if (shadingModel == "Subsurface")
    {
        if (materialInstanceJson.contains("subsurfaceProfile") ||
            materialInstanceJson.contains("skinLut"))
        {
            throw std::runtime_error(
                "Subsurface local materials cannot bind profile/LUT assets: " +
                std::string(materialInstancePath));
        }
        ValidateSubsurfaceMaterial(
            effectiveMaterialInstanceJson,
            materialInstancePath);
    }
    else if (shadingModel == "PreintegratedSkin")
    {
        const std::string assetPath = RequireAssetReference(
            materialInstanceJson,
            "skinLut",
            materialInstancePath);
        resolvedAssets.skinLutId =
            resourceSet.ResolveSkinLutId(assetPath);
        // ID 是引擎从资产路径派生的数据，不能信任 MI 中可能残留的旧参数值。
        effectiveMaterialInstanceJson["parameters"]["u_skinLutId"] =
            static_cast<float>(*resolvedAssets.skinLutId);
        ValidatePreintegratedSkinMaterial(
            effectiveMaterialInstanceJson,
            resourceSet.GetSkinLut(*resolvedAssets.skinLutId),
            materialInstancePath);
    }
    else if (shadingModel == "SubsurfaceProfile")
    {
        const std::string assetPath = RequireAssetReference(
            materialInstanceJson,
            "subsurfaceProfile",
            materialInstancePath);
        resolvedAssets.profileId =
            resourceSet.ResolveProfileId(assetPath);
        // 先覆盖 effective JSON，再走统一 Material 参数校验与 snapshot 构建。
        effectiveMaterialInstanceJson["parameters"]
            ["u_subsurfaceProfileId"] =
                static_cast<float>(*resolvedAssets.profileId);
        ValidateSubsurfaceProfileMaterial(
            effectiveMaterialInstanceJson,
            materialInstancePath);
    }
    else if (materialInstanceJson.contains("subsurfaceProfile") ||
             materialInstanceJson.contains("skinLut"))
    {
        throw std::runtime_error(
            "Profile/LUT asset fields require an SSS shading model: " +
            std::string(materialInstancePath));
    }

    return resolvedAssets;
}

void ReapplyResolvedSubsurfaceMaterialIds(
    const ResolvedSubsurfaceMaterialAssets& resolvedAssets,
    ::MaterialInstance& materialInstance)
{
    // 热重载状态迁移可能恢复旧 snapshot，因此在最终验证前再次写回派生 ID。
    if (resolvedAssets.profileId)
    {
        materialInstance.SetParameter(
            "u_subsurfaceProfileId",
            static_cast<float>(*resolvedAssets.profileId));
    }
    if (resolvedAssets.skinLutId)
    {
        materialInstance.SetParameter(
            "u_skinLutId",
            static_cast<float>(*resolvedAssets.skinLutId));
    }
}

} // namespace VL
