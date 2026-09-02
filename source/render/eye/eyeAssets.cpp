#include "render/eye/eyeAssets.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{

bool IsFinite(float value) noexcept
{
    return std::isfinite(value) != 0;
}

void RequireOnlyFields(
    const nlohmann::json& json,
    const std::set<std::string>& fields,
    std::string_view assetPath)
{
    for (const auto& [field, value] : json.items())
    {
        if (fields.find(field) == fields.end())
        {
            throw std::runtime_error(
                "Unknown Eye profile field \"" + field + "\": " +
                std::string(assetPath));
        }
    }
}

uint32_t RequireUnsigned(
    const nlohmann::json& json,
    const char* field,
    std::string_view assetPath,
    uint32_t defaultValue,
    bool required)
{
    if (!json.contains(field))
    {
        if (required)
        {
            throw std::runtime_error(
                "Eye profile requires integer field \"" + std::string(field) +
                "\": " + std::string(assetPath));
        }
        return defaultValue;
    }
    if (!json.at(field).is_number_integer())
    {
        throw std::runtime_error(
            "Eye profile integer field has an invalid type \"" +
            std::string(field) + "\": " + std::string(assetPath));
    }
    const int64_t value = json.at(field).get<int64_t>();
    if (value < 0 || value > static_cast<int64_t>(UINT32_MAX))
    {
        throw std::runtime_error(
            "Eye profile integer field is outside the unsigned range \"" +
            std::string(field) + "\": " + std::string(assetPath));
    }
    return static_cast<uint32_t>(value);
}

float RequireFloat(
    const nlohmann::json& json,
    const char* field,
    std::string_view assetPath,
    float defaultValue,
    bool required)
{
    if (!json.contains(field))
    {
        if (required)
        {
            throw std::runtime_error(
                "Eye profile requires numeric field \"" + std::string(field) +
                "\": " + std::string(assetPath));
        }
        return defaultValue;
    }
    if (!json.at(field).is_number())
    {
        throw std::runtime_error(
            "Eye profile numeric field has an invalid type \"" +
            std::string(field) + "\": " + std::string(assetPath));
    }
    const float value = json.at(field).get<float>();
    if (!IsFinite(value))
    {
        throw std::runtime_error(
            "Eye profile field is non-finite \"" + std::string(field) +
            "\": " + std::string(assetPath));
    }
    return value;
}

float UnitScale(
    std::string_view unit,
    const nlohmann::json& json,
    std::string_view assetPath)
{
    const float inferred =
        unit == "meter" ? 1.0f :
        unit == "centimeter" ? 0.01f :
        unit == "millimeter" ? 0.001f :
        unit == "micrometer" ? 0.000001f :
        0.0f;
    if (inferred == 0.0f)
    {
        throw std::runtime_error(
            "Unsupported Eye profile distanceUnit: " + std::string(assetPath));
    }
    if (json.contains("worldUnitScale"))
    {
        const float explicitScale = json.at("worldUnitScale").get<float>();
        if (!IsFinite(explicitScale) || explicitScale <= 0.0f ||
            std::abs(explicitScale - inferred) > inferred * 1.0e-4f)
        {
            throw std::runtime_error(
                "Eye profile worldUnitScale does not match distanceUnit: " +
                std::string(assetPath));
        }
    }
    return inferred;
}

void RequirePositive(float value, const char* field, std::string_view assetPath)
{
    if (!(value > 0.0f) || !IsFinite(value))
    {
        throw std::runtime_error(
            "Eye profile field must be positive \"" + std::string(field) +
            "\": " + std::string(assetPath));
    }
}

} // namespace

EyeProfileAsset ParseEyeProfileAsset(
    const nlohmann::json& json,
    std::string_view assetPath)
{
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Eye profile must be a JSON object: " + std::string(assetPath));
    }
    RequireOnlyFields(
        json,
        {
            "name", "type", "schemaVersion", "profileVersion", "profileId",
            "ior", "eyeRadius", "corneaRadius", "irisDistance", "irisRadius",
            "pupilRadius", "pupilRadiusRange", "limbusWidth", "causticStrength",
            "distanceUnit", "worldUnitScale", "unit", "causticLutVersion",
            "kernelVersion", "sourceIdentity", "lodContract"
        },
        assetPath);
    if (json.value("type", std::string()) != "eyeProfile")
    {
        throw std::runtime_error(
            "Eye profile type must be eyeProfile: " + std::string(assetPath));
    }

    EyeProfileAsset asset;
    asset.name = json.value("name", std::string(assetPath));
    asset.assetPath = std::filesystem::path(assetPath)
        .lexically_normal()
        .generic_string();
    asset.schemaVersion = RequireUnsigned(
        json, "schemaVersion", assetPath, EyeProfileSchemaVersion, true);
    asset.profileVersion = RequireUnsigned(
        json, "profileVersion", assetPath, EyeProfileVersion, false);
    asset.profileId = RequireUnsigned(json, "profileId", assetPath, 0, true);
    asset.kernelVersion = RequireUnsigned(
        json, "kernelVersion", assetPath, EyeCausticLutKernelVersion, false);
    asset.causticLutVersion = RequireUnsigned(
        json, "causticLutVersion", assetPath, EyeCausticLutVersion, false);
    asset.ior = RequireFloat(json, "ior", assetPath, asset.ior, true);

    const std::string authoringUnit = json.contains("distanceUnit")
        ? json.at("distanceUnit").get<std::string>()
        : json.value("unit", std::string("meter"));
    asset.distanceUnit = authoringUnit;
    asset.worldUnitScale = UnitScale(authoringUnit, json, assetPath);
    asset.unit = "meter";

    const float scale = asset.worldUnitScale;
    asset.eyeRadius = RequireFloat(
        json, "eyeRadius", assetPath, asset.eyeRadius / scale, false) * scale;
    asset.corneaRadius = RequireFloat(
        json, "corneaRadius", assetPath, asset.corneaRadius / scale, true) * scale;
    asset.irisDistance = RequireFloat(
        json, "irisDistance", assetPath, asset.irisDistance / scale, true) * scale;
    asset.irisRadius = RequireFloat(
        json, "irisRadius", assetPath, asset.irisRadius / scale, true) * scale;
    asset.limbusWidth = RequireFloat(
        json, "limbusWidth", assetPath, asset.limbusWidth / scale, false) * scale;
    asset.causticStrength = RequireFloat(
        json, "causticStrength", assetPath, asset.causticStrength, false);

    if (json.contains("pupilRadiusRange"))
    {
        const nlohmann::json& range = json.at("pupilRadiusRange");
        if (!range.is_array() || range.size() != 2 ||
            !range[0].is_number() || !range[1].is_number())
        {
            throw std::runtime_error(
                "Eye profile pupilRadiusRange must contain two numbers: " +
                std::string(assetPath));
        }
        asset.pupilRadiusMin = range[0].get<float>() * scale;
        asset.pupilRadiusMax = range[1].get<float>() * scale;
        asset.pupilRadius =
            json.contains("pupilRadius")
                ? json.at("pupilRadius").get<float>() * scale
                : (asset.pupilRadiusMin + asset.pupilRadiusMax) * 0.5f;
    }
    else
    {
        asset.pupilRadius = RequireFloat(
            json, "pupilRadius", assetPath, asset.pupilRadius / scale, true) * scale;
        asset.pupilRadiusMin = asset.pupilRadius;
        asset.pupilRadiusMax = asset.pupilRadius;
    }
    asset.sourceIdentity = json.value(
        "sourceIdentity", std::string("vulkanlearn.eye.compute.v1"));
    asset.lodContract = ParseEyeLodContract(
        json,
        assetPath,
        asset.profileId,
        asset.profileVersion,
        asset.causticLutVersion);

    if (asset.schemaVersion != EyeProfileSchemaVersion ||
        asset.profileVersion != EyeProfileVersion ||
        asset.kernelVersion != EyeCausticLutKernelVersion ||
        asset.causticLutVersion != EyeCausticLutVersion)
    {
        throw std::runtime_error(
            "Unsupported Eye profile schema/kernel/LUT version: " +
            std::string(assetPath));
    }
    ValidateEyeProfileAsset(asset, assetPath);
    return asset;
}

void ValidateEyeProfileAsset(
    const EyeProfileAsset& asset,
    std::string_view assetPath)
{
    if (asset.profileId == 0 || asset.profileId > EyeCausticLutMaximumProfileId)
    {
        throw std::runtime_error(
            "Eye profileId 0 is reserved and the maximum is " +
            std::to_string(EyeCausticLutMaximumProfileId) + ": " +
            std::string(assetPath));
    }
    if (asset.unit != "meter" || asset.distanceUnit.empty() ||
        !IsFinite(asset.worldUnitScale) || asset.worldUnitScale <= 0.0f)
    {
        throw std::runtime_error(
            "Eye profile runtime unit contract is invalid: " +
            std::string(assetPath));
    }
    if (!(asset.ior > 1.0f && asset.ior < 2.0f) || !IsFinite(asset.ior))
    {
        throw std::runtime_error(
            "Eye profile IOR is outside the supported range: " +
            std::string(assetPath));
    }
    RequirePositive(asset.eyeRadius, "eyeRadius", assetPath);
    RequirePositive(asset.corneaRadius, "corneaRadius", assetPath);
    RequirePositive(asset.irisDistance, "irisDistance", assetPath);
    RequirePositive(asset.irisRadius, "irisRadius", assetPath);
    RequirePositive(asset.pupilRadiusMin, "pupilRadiusMin", assetPath);
    RequirePositive(asset.pupilRadiusMax, "pupilRadiusMax", assetPath);
    RequirePositive(asset.pupilRadius, "pupilRadius", assetPath);
    RequirePositive(asset.limbusWidth, "limbusWidth", assetPath);
    if (asset.corneaRadius > asset.eyeRadius ||
        asset.irisDistance >= asset.corneaRadius ||
        asset.irisRadius >= asset.corneaRadius ||
        asset.pupilRadiusMin > asset.pupilRadiusMax ||
        asset.pupilRadiusMax >= asset.irisRadius ||
        asset.pupilRadius < asset.pupilRadiusMin ||
        asset.pupilRadius > asset.pupilRadiusMax ||
        asset.limbusWidth >= asset.irisRadius)
    {
        throw std::runtime_error(
            "Eye profile radius/depth relationship is invalid: " +
            std::string(assetPath));
    }
    if (!(asset.causticStrength >= 0.0f && asset.causticStrength <= 0.45f))
    {
        throw std::runtime_error(
            "Eye profile causticStrength must be in [0,0.45]: " +
            std::string(assetPath));
    }
    if (asset.sourceIdentity.empty())
    {
        throw std::runtime_error(
            "Eye profile sourceIdentity must not be empty: " +
            std::string(assetPath));
    }
    ValidateEyeLodContract(asset.lodContract, assetPath);
    if (asset.lodContract.profileId != asset.profileId ||
        asset.lodContract.profileVersion != asset.profileVersion ||
        asset.lodContract.lutVersion != asset.causticLutVersion)
    {
        throw std::runtime_error(
            "Eye LOD contract changes profile identity: " +
            std::string(assetPath));
    }
}

std::vector<EyeProfileAsset> LoadEyeProfileAssets(
    const std::filesystem::path& resourceRoot)
{
    const std::filesystem::path profileDirectory =
        resourceRoot / "Common" / "Profiles" / "Eye";
    if (!std::filesystem::is_directory(profileDirectory))
    {
        return {};
    }

    std::vector<std::filesystem::path> paths;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(profileDirectory))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    std::vector<EyeProfileAsset> assets;
    assets.reserve(paths.size());
    std::set<uint32_t> profileIds;
    for (const std::filesystem::path& path : paths)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            throw std::runtime_error(
                "Failed to open Eye profile: " + path.generic_string());
        }
        nlohmann::json json;
        file >> json;
        const std::filesystem::path relativePath =
            std::filesystem::relative(path, resourceRoot);
        EyeProfileAsset asset = ParseEyeProfileAsset(
            json,
            relativePath.generic_string());
        if (!profileIds.insert(asset.profileId).second)
        {
            throw std::runtime_error(
                "Duplicate Eye profileId: " +
                std::to_string(asset.profileId) + ": " +
                relativePath.generic_string());
        }
        assets.push_back(std::move(asset));
    }
    return assets;
}

nlohmann::json SerializeEyeCausticLutMetadata(
    const EyeCausticLutMetadata& metadata)
{
    return {
        {"schemaVersion", metadata.schemaVersion},
        {"lutVersion", metadata.lutVersion},
        {"kernelVersion", metadata.kernelVersion},
        {"width", metadata.width},
        {"height", metadata.height},
        {"elevationSlices", metadata.elevationSlices},
        {"maximumProfileId", metadata.maximumProfileId},
        {"layers", metadata.layers},
        {"channels", metadata.channels},
        {"unit", metadata.unit},
        {"radialCoordinate", metadata.radialCoordinate},
        {"elevationCoordinate", metadata.elevationCoordinate},
        {"channelConvention", metadata.channelConvention},
        {"computeArtifactGenerationKey", metadata.computeArtifactGenerationKey},
        {"sourceDigest", metadata.sourceDigest}
    };
}

float EvaluateEyeCausticGain(
    float radialSquared,
    float elevation,
    float strength) noexcept
{
    const float frontLightWeight = 0.35f + 0.65f * elevation;
    return 1.0f + strength * (1.0f - 2.0f * radialSquared) * frontLightWeight;
}

} // namespace VL
