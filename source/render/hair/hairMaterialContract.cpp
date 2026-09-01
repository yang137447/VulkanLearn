#include "render/hair/hairMaterialContract.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include "render/hair/hairAssets.h"
#include "render/hair/hairConventions.h"
#include "render/hair/hairResourceSet.h"

namespace VL
{
namespace
{

std::array<float, 4> RequireVec4(
    const nlohmann::json& parameters,
    const char* name,
    std::string_view materialInstancePath)
{
    if (!parameters.contains(name) ||
        !parameters.at(name).is_array() ||
        parameters.at(name).size() != 4)
    {
        throw std::runtime_error(
            "Hair material requires vec4 parameter \"" + std::string(name) +
            "\": " + std::string(materialInstancePath));
    }

    std::array<float, 4> result{};
    for (size_t index = 0; index < result.size(); ++index)
    {
        const nlohmann::json& value = parameters.at(name).at(index);
        if (!value.is_number())
        {
            throw std::runtime_error(
                "Hair material parameter contains a non-numeric component \"" +
                std::string(name) + "\": " + std::string(materialInstancePath));
        }
        result[index] = value.get<float>();
        if (!std::isfinite(result[index]))
        {
            throw std::runtime_error(
                "Hair material parameter contains a non-finite component \"" +
                std::string(name) + "\": " + std::string(materialInstancePath));
        }
    }
    return result;
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
            "Hair material parameter is outside the frozen range \"" +
            std::string(field) + "\": " + std::string(materialInstancePath));
    }
}

void RequireNear(
    float value,
    float expected,
    const char* field,
    std::string_view materialInstancePath)
{
    if (std::abs(value - expected) > 1.0e-5f)
    {
        throw std::runtime_error(
            "Hair material parameter does not match the active LUT \"" +
            std::string(field) + "\": " + std::string(materialInstancePath));
    }
}

} // namespace

void ValidateHairMaterialAuthoringContract(
    const nlohmann::json& effectiveMaterialInstanceJson,
    const HairAzimuthalLutMetadata& lutMetadata,
    std::string_view materialInstancePath)
{
    if (!effectiveMaterialInstanceJson.contains("shadingModel") ||
        effectiveMaterialInstanceJson.at("shadingModel") != "Hair")
    {
        return;
    }

    ValidateHairAzimuthalLutMetadata(lutMetadata, materialInstancePath);

    if (!effectiveMaterialInstanceJson.contains("parameters") ||
        !effectiveMaterialInstanceJson.at("parameters").is_object())
    {
        throw std::runtime_error(
            "Hair material is missing its parameter object: " +
            std::string(materialInstancePath));
    }

    const nlohmann::json& parameters =
        effectiveMaterialInstanceJson.at("parameters");
    const std::array<float, 4> optical =
        RequireVec4(parameters, "u_hairOptical", materialInstancePath);
    const std::array<float, 4> scattering =
        RequireVec4(parameters, "u_hairScattering", materialInstancePath);
    const std::array<float, 4> coverage =
        RequireVec4(parameters, "u_hairCoverage", materialInstancePath);
    const std::array<float, 4> characterLighting =
        RequireVec4(parameters, "u_hairCharacterLighting", materialInstancePath);

    // u_hairOptical: absorption multiplier, IOR, fiber radius(m), cuticle tilt(rad)。
    // 上限同时保证最暗作者颜色转换出的 sigma_a 可安全写入 RGBA16F GBuffer。
    RequireRange(optical[0], 0.0001f, 2.0f, "u_hairOptical.x", materialInstancePath);
    RequireRange(optical[1], 1.0001f, 2.5f, "u_hairOptical.y", materialInstancePath);
    RequireNear(optical[1], lutMetadata.ior, "u_hairOptical.y", materialInstancePath);
    RequireRange(optical[2], 1.0e-7f, 0.01f, "u_hairOptical.z", materialInstancePath);
    RequireNear(optical[2], lutMetadata.fiberRadius, "u_hairOptical.z", materialInstancePath);
    RequireRange(optical[3], -Hair::HairHalfPi, Hair::HairHalfPi, "u_hairOptical.w", materialInstancePath);

    // u_hairScattering: scatter、backlit、longitudinal roughness、azimuthal roughness。
    RequireRange(scattering[0], 0.0f, 1.0f, "u_hairScattering.x", materialInstancePath);
    RequireRange(scattering[1], 0.0f, 1.0f, "u_hairScattering.y", materialInstancePath);
    RequireRange(scattering[2], 0.0001f, Hair::HairHalfPi, "u_hairScattering.z", materialInstancePath);
    RequireRange(scattering[3], 0.0001f, 1.0f, "u_hairScattering.w", materialInstancePath);

    // u_hairCoverage: surface coverage、MS budget、density、reserved。
    RequireRange(coverage[0], 0.0f, 1.0f, "u_hairCoverage.x", materialInstancePath);
    RequireRange(coverage[1], 0.0f, 1.0f, "u_hairCoverage.y", materialInstancePath);
    RequireRange(coverage[2], 0.0f, 1.0f, "u_hairCoverage.z", materialInstancePath);
    if (coverage[3] != 0.0f)
    {
        throw std::runtime_error(
            "u_hairCoverage.w is reserved and must be zero: " +
            std::string(materialInstancePath));
    }

    // u_hairCharacterLighting: ambient、directional、local、camera virtual light。
    // 数值在资产侧冻结；shader 不做逐像素防御性裁切。
    RequireRange(characterLighting[0], 0.0f, 4.0f, "u_hairCharacterLighting.x", materialInstancePath);
    RequireRange(characterLighting[1], 0.0f, 4.0f, "u_hairCharacterLighting.y", materialInstancePath);
    RequireRange(characterLighting[2], 0.0f, 4.0f, "u_hairCharacterLighting.z", materialInstancePath);
    RequireRange(characterLighting[3], 0.0f, 4.0f, "u_hairCharacterLighting.w", materialInstancePath);
}

void ValidateHairMaterialContract(
    const nlohmann::json& effectiveMaterialInstanceJson,
    const HairResourceSet* hairResources,
    std::string_view materialInstancePath)
{
    if (!effectiveMaterialInstanceJson.contains("shadingModel") ||
        effectiveMaterialInstanceJson.at("shadingModel") != "Hair")
    {
        return;
    }

    if (hairResources == nullptr || !hairResources->azimuthalLutTexture)
    {
        throw std::runtime_error(
            "Hair material requires an active GPU Hair LUT resource: " +
            std::string(materialInstancePath));
    }
    if (hairResources->sourceIdentity.empty() ||
        hairResources->sourceDigest.empty())
    {
        throw std::runtime_error(
            "Hair resource set is missing a stable LUT identity: " +
            std::string(materialInstancePath));
    }

    ValidateHairMaterialAuthoringContract(
        effectiveMaterialInstanceJson,
        hairResources->GetLutMetadata(),
        materialInstancePath);
}

} // namespace VL
