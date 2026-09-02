#include "render/subsurface/subsurfaceAssets.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{

constexpr float ValidationTolerance = 1.0e-6f;

bool IsFinite(float value)
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
                "Unknown subsurface asset field \"" + field +
                "\": " + std::string(assetPath));
        }
    }
}

uint32_t RequireUnsigned(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string fieldName(field);
    if (!json.contains(fieldName) ||
        !(json[fieldName].is_number_unsigned() ||
          json[fieldName].is_number_integer()))
    {
        throw std::runtime_error(
            "Subsurface asset requires integer field \"" + fieldName +
            "\": " + std::string(assetPath));
    }

    const int64_t value = json[fieldName].get<int64_t>();
    if (value < 0 ||
        static_cast<uint64_t>(value) >
            std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error(
            "Subsurface asset integer field is out of range \"" +
            fieldName + "\": " + std::string(assetPath));
    }
    return static_cast<uint32_t>(value);
}

float RequirePositiveFloat(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string fieldName(field);
    if (!json.contains(fieldName) || !json[fieldName].is_number())
    {
        throw std::runtime_error(
            "Subsurface asset requires numeric field \"" + fieldName +
            "\": " + std::string(assetPath));
    }
    const float value = json[fieldName].get<float>();
    if (!IsFinite(value) || value <= 0.0f)
    {
        throw std::runtime_error(
            "Subsurface asset field must be finite and positive \"" +
            fieldName + "\": " + std::string(assetPath));
    }
    return value;
}

std::string RequireString(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string fieldName(field);
    if (!json.contains(fieldName) || !json[fieldName].is_string())
    {
        throw std::runtime_error(
            "Subsurface asset requires string field \"" + fieldName +
            "\": " + std::string(assetPath));
    }
    const std::string value = json[fieldName].get<std::string>();
    if (value.empty())
    {
        throw std::runtime_error(
            "Subsurface asset string field must not be empty \"" +
            fieldName + "\": " + std::string(assetPath));
    }
    return value;
}

SubsurfaceRgb RequireRgb(
    const nlohmann::json& json,
    std::string_view field,
    std::string_view assetPath)
{
    const std::string fieldName(field);
    if (!json.contains(fieldName) ||
        !json[fieldName].is_array() ||
        json[fieldName].size() != 3)
    {
        throw std::runtime_error(
            "Subsurface asset requires RGB array field \"" + fieldName +
            "\": " + std::string(assetPath));
    }

    SubsurfaceRgb value{};
    for (size_t channel = 0; channel < value.size(); ++channel)
    {
        if (!json[fieldName][channel].is_number())
        {
            throw std::runtime_error(
                "Subsurface RGB field must contain numbers \"" +
                fieldName + "\": " + std::string(assetPath));
        }
        value[channel] = json[fieldName][channel].get<float>();
        if (!IsFinite(value[channel]) ||
            value[channel] <= 0.0f ||
            value[channel] > 1.0f)
        {
            throw std::runtime_error(
                "Subsurface RGB field must be inside (0, 1] \"" +
                fieldName + "\": " + std::string(assetPath));
        }
    }
    return value;
}

nlohmann::json LoadJsonFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open subsurface asset: " + path.string());
    }

    nlohmann::json json;
    input >> json;
    return json;
}

std::vector<std::filesystem::path> CollectJsonAssets(
    const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> paths;
    if (!std::filesystem::exists(directory))
    {
        return paths;
    }

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory))
    {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".json")
        {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string MakeAssetPath(
    const std::filesystem::path& resourceRoot,
    const std::filesystem::path& absolutePath)
{
    return std::filesystem::relative(absolutePath, resourceRoot)
        .lexically_normal()
        .generic_string();
}

void ValidateWorldUnitScale(
    SubsurfaceDistanceUnit unit,
    float worldUnitScale,
    std::string_view assetPath)
{
    const float expectedScale = GetDistanceUnitWorldScale(unit);
    if (std::abs(worldUnitScale - expectedScale) >
        ValidationTolerance)
    {
        throw std::runtime_error(
            "worldUnitScale does not match distanceUnit in subsurface asset: " +
            std::string(assetPath));
    }
}

} // namespace

// 资产边界统一把作者单位转换到 VulkanLearn 世界单位；后续 GPU 代码只消费已校验的米制值。
SubsurfaceDistanceUnit ParseSubsurfaceDistanceUnit(
    std::string_view value)
{
    if (value == "millimeter")
    {
        return SubsurfaceDistanceUnit::Millimeter;
    }
    if (value == "centimeter")
    {
        return SubsurfaceDistanceUnit::Centimeter;
    }
    if (value == "meter")
    {
        return SubsurfaceDistanceUnit::Meter;
    }
    throw std::runtime_error(
        "Unsupported subsurface distance unit: " +
        std::string(value));
}

std::string_view ToString(SubsurfaceDistanceUnit value)
{
    switch (value)
    {
    case SubsurfaceDistanceUnit::Millimeter:
        return "millimeter";
    case SubsurfaceDistanceUnit::Centimeter:
        return "centimeter";
    case SubsurfaceDistanceUnit::Meter:
        return "meter";
    }
    throw std::runtime_error("Invalid subsurface distance unit");
}

float GetDistanceUnitWorldScale(SubsurfaceDistanceUnit value)
{
    switch (value)
    {
    case SubsurfaceDistanceUnit::Millimeter:
        return 0.001f;
    case SubsurfaceDistanceUnit::Centimeter:
        return 0.01f;
    case SubsurfaceDistanceUnit::Meter:
        return 1.0f;
    }
    throw std::runtime_error("Invalid subsurface distance unit");
}

PreintegratedSkinOutputMode ParsePreintegratedSkinOutputMode(
    std::string_view value)
{
    if (value == "scatteringMultiplier")
    {
        return PreintegratedSkinOutputMode::ScatteringMultiplier;
    }
    if (value == "finalDiffuseResponse")
    {
        return PreintegratedSkinOutputMode::FinalDiffuseResponse;
    }
    throw std::runtime_error(
        "Unsupported preintegrated skin output mode: " +
        std::string(value));
}

std::string_view ToString(PreintegratedSkinOutputMode value)
{
    switch (value)
    {
    case PreintegratedSkinOutputMode::ScatteringMultiplier:
        return "scatteringMultiplier";
    case PreintegratedSkinOutputMode::FinalDiffuseResponse:
        return "finalDiffuseResponse";
    }
    throw std::runtime_error(
        "Invalid preintegrated skin output mode");
}

// profile JSON 在这里完成 schema、版本、stable ID、单位和范围校验；通过后才允许进入 World candidate。
SubsurfaceProfileAsset ParseSubsurfaceProfileAsset(
    const nlohmann::json& json,
    std::string_view assetPath)
{
    static const std::set<std::string> Fields = {
        "name",
        "type",
        "schemaVersion",
        "profileId",
        "meanFreePathColor",
        "meanFreePathDistance",
        "distanceUnit",
        "worldUnitScale",
        "kernelVersion",
    };
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Subsurface profile must be a JSON object: " +
            std::string(assetPath));
    }
    RequireOnlyFields(json, Fields, assetPath);
    if (RequireString(json, "type", assetPath) !=
        "subsurfaceProfile")
    {
        throw std::runtime_error(
            "Subsurface profile type must be \"subsurfaceProfile\": " +
            std::string(assetPath));
    }
    if (RequireUnsigned(json, "schemaVersion", assetPath) !=
        SubsurfaceProfileSchemaVersion)
    {
        throw std::runtime_error(
            "Unsupported subsurface profile schemaVersion: " +
            std::string(assetPath));
    }
    if (RequireUnsigned(json, "kernelVersion", assetPath) !=
        SubsurfaceProfileKernelVersion)
    {
        throw std::runtime_error(
            "Unsupported subsurface profile kernelVersion: " +
            std::string(assetPath));
    }

    SubsurfaceProfileAsset asset;
    asset.name = RequireString(json, "name", assetPath);
    asset.assetPath = std::string(assetPath);
    asset.profileId = RequireUnsigned(json, "profileId", assetPath);
    if (asset.profileId == 0 ||
        asset.profileId > SubsurfaceProfileMaximumId)
    {
        throw std::runtime_error(
            "Subsurface profileId must be inside [1, 255]: " +
            std::string(assetPath));
    }
    asset.meanFreePathColor =
        RequireRgb(json, "meanFreePathColor", assetPath);
    asset.meanFreePathDistance = RequirePositiveFloat(
        json,
        "meanFreePathDistance",
        assetPath);
    asset.distanceUnit = ParseSubsurfaceDistanceUnit(
        RequireString(json, "distanceUnit", assetPath));
    asset.worldUnitScale =
        RequirePositiveFloat(json, "worldUnitScale", assetPath);
    ValidateWorldUnitScale(
        asset.distanceUnit,
        asset.worldUnitScale,
        assetPath);
    return asset;
}

// skin LUT JSON 同样只产生作者参数和稳定身份，不在 CPU 端求值 lookup 数值。
PreintegratedSkinLutAsset ParsePreintegratedSkinLutAsset(
    const nlohmann::json& json,
    std::string_view assetPath)
{
    static const std::set<std::string> Fields = {
        "name",
        "type",
        "schemaVersion",
        "skinLutId",
        "lutVersion",
        "width",
        "height",
        "thicknessMax",
        "thicknessUnit",
        "worldUnitScale",
        "outputMode",
        "scatterColor",
        "transmissionColor",
        "sourceIdentity",
    };
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Preintegrated skin LUT must be a JSON object: " +
            std::string(assetPath));
    }
    RequireOnlyFields(json, Fields, assetPath);
    if (RequireString(json, "type", assetPath) !=
        "preintegratedSkinLut")
    {
        throw std::runtime_error(
            "Preintegrated skin type must be \"preintegratedSkinLut\": " +
            std::string(assetPath));
    }
    if (RequireUnsigned(json, "schemaVersion", assetPath) !=
        PreintegratedSkinSchemaVersion)
    {
        throw std::runtime_error(
            "Unsupported preintegrated skin schemaVersion: " +
            std::string(assetPath));
    }
    if (RequireUnsigned(json, "lutVersion", assetPath) !=
        PreintegratedSkinLutVersion)
    {
        throw std::runtime_error(
            "Unsupported preintegrated skin lutVersion: " +
            std::string(assetPath));
    }

    PreintegratedSkinLutAsset asset;
    asset.name = RequireString(json, "name", assetPath);
    asset.assetPath = std::string(assetPath);
    asset.skinLutId = RequireUnsigned(json, "skinLutId", assetPath);
    if (asset.skinLutId == 0 ||
        asset.skinLutId > PreintegratedSkinMaximumId)
    {
        throw std::runtime_error(
            "skinLutId must be inside [1, 15]: " +
            std::string(assetPath));
    }
    asset.width = RequireUnsigned(json, "width", assetPath);
    asset.height = RequireUnsigned(json, "height", assetPath);
    if (asset.width != PreintegratedSkinLutWidth ||
        asset.height != PreintegratedSkinLutTileHeight)
    {
        throw std::runtime_error(
            "Preintegrated skin LUT v1 requires 128x64 resolution: " +
            std::string(assetPath));
    }
    asset.thicknessMax =
        RequirePositiveFloat(json, "thicknessMax", assetPath);
    asset.thicknessUnit = ParseSubsurfaceDistanceUnit(
        RequireString(json, "thicknessUnit", assetPath));
    asset.worldUnitScale =
        RequirePositiveFloat(json, "worldUnitScale", assetPath);
    ValidateWorldUnitScale(
        asset.thicknessUnit,
        asset.worldUnitScale,
        assetPath);
    asset.outputMode = ParsePreintegratedSkinOutputMode(
        RequireString(json, "outputMode", assetPath));
    asset.scatterColor =
        RequireRgb(json, "scatterColor", assetPath);
    asset.transmissionColor =
        RequireRgb(json, "transmissionColor", assetPath);
    asset.sourceIdentity =
        RequireString(json, "sourceIdentity", assetPath);
    return asset;
}

// 文件枚举和 JSON 读取属于 CPU 资产边界；profile response 仍由 Compute generator 生成。
std::vector<SubsurfaceProfileAsset> LoadSubsurfaceProfileAssets(
    const std::filesystem::path& resourceRoot)
{
    std::vector<SubsurfaceProfileAsset> assets;
    const std::filesystem::path directory =
        resourceRoot / "Common" / "Profiles" / "Subsurface";
    for (const std::filesystem::path& path :
         CollectJsonAssets(directory))
    {
        const std::string assetPath =
            MakeAssetPath(resourceRoot, path);
        assets.push_back(ParseSubsurfaceProfileAsset(
            LoadJsonFile(path),
            assetPath));
    }
    return assets;
}

// 返回的 metadata 会被序列化到 candidate resource set，不创建 HostImage lookup fallback。
std::vector<PreintegratedSkinLutAsset>
LoadPreintegratedSkinLutAssets(
    const std::filesystem::path& resourceRoot)
{
    std::vector<PreintegratedSkinLutAsset> assets;
    const std::filesystem::path directory =
        resourceRoot / "Common" / "Profiles" / "SkinLuts";
    for (const std::filesystem::path& path :
         CollectJsonAssets(directory))
    {
        const std::string assetPath =
            MakeAssetPath(resourceRoot, path);
        assets.push_back(ParsePreintegratedSkinLutAsset(
            LoadJsonFile(path),
            assetPath));
    }
    return assets;
}

} // namespace VL
