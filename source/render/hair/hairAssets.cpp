#include "render/hair/hairAssets.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

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
                "Unknown Hair LUT metadata field \"" + field + "\": " +
                std::string(assetPath));
        }
    }
}

uint32_t RequireUnsigned(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string name(field);
    if (!json.contains(name) ||
        !(json.at(name).is_number_unsigned() ||
          json.at(name).is_number_integer()))
    {
        throw std::runtime_error(
            "Hair LUT metadata requires unsigned field \"" + name + "\": " +
            std::string(assetPath));
    }
    const int64_t value = json.at(name).get<int64_t>();
    if (value < 0 ||
        static_cast<uint64_t>(value) >
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    {
        throw std::runtime_error(
            "Hair LUT metadata field must not be negative \"" + name + "\": " +
            std::string(assetPath));
    }
    return static_cast<uint32_t>(value);
}

float RequireFloat(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string name(field);
    if (!json.contains(name) || !json.at(name).is_number())
    {
        throw std::runtime_error(
            "Hair LUT metadata requires numeric field \"" + name + "\": " +
            std::string(assetPath));
    }
    const float value = json.at(name).get<float>();
    if (!IsFinite(value))
    {
        throw std::runtime_error(
            "Hair LUT metadata field is not finite \"" + name + "\": " +
            std::string(assetPath));
    }
    return value;
}

std::string RequireString(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string name(field);
    if (!json.contains(name) || !json.at(name).is_string())
    {
        throw std::runtime_error(
            "Hair LUT metadata requires string field \"" + name + "\": " +
            std::string(assetPath));
    }
    const std::string value = json.at(name).get<std::string>();
    if (value.empty())
    {
        throw std::runtime_error(
            "Hair LUT metadata string field must not be empty \"" + name + "\": " +
            std::string(assetPath));
    }
    return value;
}

std::filesystem::path GetAuthoredMetadataPath(
    const std::filesystem::path& resourceRoot)
{
    return resourceRoot / "Common" / "Profiles" / "Hair" / "hairAzimuthalLut.json";
}

} // namespace

HairAzimuthalLutMetadata ParseHairAzimuthalLutMetadata(
    const nlohmann::json& json,
    std::string_view assetPath)
{
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Hair LUT metadata must be an object: " + std::string(assetPath));
    }

    RequireOnlyFields(
        json,
        {
            "schemaVersion",
            "lutVersion",
            "kernelVersion",
            "width",
            "height",
            "layers",
            "channels",
            "roughnessSlices",
            "thetaDSamples",
            "ior",
            "fiberRadius",
            "unit",
            "thetaDCoordinate",
            "deltaPhiCoordinate",
            "roughnessMapping",
            "wrap",
            "pathConvention",
            "sourceIdentity"},
        assetPath);

    HairAzimuthalLutMetadata metadata;
    metadata.schemaVersion = RequireUnsigned(json, "schemaVersion", assetPath);
    metadata.lutVersion = RequireUnsigned(json, "lutVersion", assetPath);
    metadata.kernelVersion = RequireUnsigned(json, "kernelVersion", assetPath);
    metadata.width = RequireUnsigned(json, "width", assetPath);
    metadata.height = RequireUnsigned(json, "height", assetPath);
    metadata.layers = RequireUnsigned(json, "layers", assetPath);
    metadata.channels = RequireUnsigned(json, "channels", assetPath);
    metadata.roughnessSlices = RequireUnsigned(json, "roughnessSlices", assetPath);
    metadata.thetaDSamples = RequireUnsigned(json, "thetaDSamples", assetPath);
    metadata.ior = RequireFloat(json, "ior", assetPath);
    metadata.fiberRadius = RequireFloat(json, "fiberRadius", assetPath);
    metadata.unit = RequireString(json, "unit", assetPath);
    metadata.thetaDCoordinate = RequireString(json, "thetaDCoordinate", assetPath);
    metadata.deltaPhiCoordinate = RequireString(json, "deltaPhiCoordinate", assetPath);
    metadata.roughnessMapping = RequireString(json, "roughnessMapping", assetPath);
    metadata.wrap = RequireString(json, "wrap", assetPath);
    metadata.pathConvention = RequireString(json, "pathConvention", assetPath);
    metadata.sourceIdentity = RequireString(json, "sourceIdentity", assetPath);
    ValidateHairAzimuthalLutMetadata(metadata, assetPath);
    return metadata;
}

void ValidateHairAzimuthalLutMetadata(
    const HairAzimuthalLutMetadata& metadata,
    std::string_view assetPath)
{
    const std::string context(assetPath);
    if (metadata.schemaVersion != HairAzimuthalLutSchemaVersion ||
        metadata.lutVersion != HairAzimuthalLutVersion ||
        metadata.kernelVersion != HairAzimuthalLutKernelVersion)
    {
        throw std::runtime_error(
            "Hair LUT metadata version is incompatible with the active shader contract: " +
            context);
    }
    if (metadata.width != HairAzimuthalLutWidth ||
        metadata.height != HairAzimuthalLutHeight ||
        metadata.layers != HairAzimuthalLutLayerCount ||
        metadata.channels != HairAzimuthalLutChannelCount ||
        metadata.roughnessSlices != HairAzimuthalLutRoughnessSliceCount ||
        metadata.thetaDSamples != HairAzimuthalLutThetaDSampleCount)
    {
        throw std::runtime_error(
            "Hair LUT metadata dimensions or channel contract are invalid: " +
            context);
    }
    if (!IsFinite(metadata.ior) ||
        !IsFinite(metadata.fiberRadius) ||
        metadata.ior <= 1.0f ||
        metadata.fiberRadius <= 0.0f)
    {
        throw std::runtime_error(
            "Hair LUT metadata requires IOR > 1 and fiberRadius > 0: " +
            context);
    }
    if (metadata.unit != "meter" ||
        metadata.thetaDCoordinate != "normalized[-pi/2,pi/2]" ||
        metadata.deltaPhiCoordinate != "normalized[-pi,pi]" ||
        metadata.roughnessMapping != "slice-linear-8" ||
        metadata.wrap != "deltaPhi-repeat-thetaD-clamp" ||
        metadata.pathConvention != "layer0=R,layer1=TT,layer2=TRT" ||
        metadata.sourceIdentity.empty())
    {
        throw std::runtime_error(
            "Hair LUT metadata coordinate or path convention is incompatible: " +
            context);
    }
}

HairAzimuthalLutAsset LoadHairAzimuthalLutAsset(
    const std::filesystem::path& resourceRoot)
{
    const std::filesystem::path metadataPath =
        GetAuthoredMetadataPath(resourceRoot);
    if (!std::filesystem::is_regular_file(metadataPath))
    {
        throw std::runtime_error(
            "Hair LUT authoring metadata is missing; expected " +
            metadataPath.generic_string() +
            ". Generated metadata is not a valid loader input: " +
            resourceRoot.generic_string());
    }

    std::ifstream file(metadataPath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open Hair LUT metadata: " + metadataPath.generic_string());
    }
    nlohmann::json json;
    file >> json;

    HairAzimuthalLutAsset asset;
    asset.metadataPath = std::filesystem::relative(
        metadataPath,
        resourceRoot).generic_string();
    asset.assetPath = asset.metadataPath;
    asset.metadata = ParseHairAzimuthalLutMetadata(json, metadataPath.generic_string());
    return asset;
}

nlohmann::json SerializeHairAzimuthalLutMetadata(
    const HairAzimuthalLutMetadata& metadata)
{
    ValidateHairAzimuthalLutMetadata(metadata, "Hair LUT metadata serialization");
    return nlohmann::json{
        {"schemaVersion", metadata.schemaVersion},
        {"lutVersion", metadata.lutVersion},
        {"kernelVersion", metadata.kernelVersion},
        {"width", metadata.width},
        {"height", metadata.height},
        {"layers", metadata.layers},
        {"channels", metadata.channels},
        {"roughnessSlices", metadata.roughnessSlices},
        {"thetaDSamples", metadata.thetaDSamples},
        {"ior", metadata.ior},
        {"fiberRadius", metadata.fiberRadius},
        {"unit", metadata.unit},
        {"thetaDCoordinate", metadata.thetaDCoordinate},
        {"deltaPhiCoordinate", metadata.deltaPhiCoordinate},
        {"roughnessMapping", metadata.roughnessMapping},
        {"wrap", metadata.wrap},
        {"pathConvention", metadata.pathConvention},
        {"sourceIdentity", metadata.sourceIdentity}};
}

} // namespace VL
