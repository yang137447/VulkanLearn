#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "terrain/terrainAssetTypes.h"

namespace VL
{
namespace TerrainAssetValidator
{

inline float RequireFiniteNumber(
    const nlohmann::json& object,
    const char* field,
    const std::string& context)
{
    if (!object.contains(field) || !object.at(field).is_number())
        throw std::invalid_argument(context + " requires number: " + field);
    const double value = object.at(field).get<double>();
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
        throw std::invalid_argument(context + " requires finite number: " + field);
    return static_cast<float>(value);
}

inline uint32_t RequirePositiveInteger(
    const nlohmann::json& object,
    const char* field,
    const std::string& context)
{
    if (!object.contains(field) || !object.at(field).is_number_integer())
        throw std::invalid_argument(context + " requires positive integer: " + field);
    const int64_t value = object.at(field).get<int64_t>();
    if (value <= 0 || value > 4096)
        throw std::invalid_argument(context + " requires positive integer <= 4096: " + field);
    return static_cast<uint32_t>(value);
}

// 可选整数字段：缺省返回 fallback，存在时必须是 [minValue, maxValue] 内的整数。
// 用于 sourceSet / flags 这类保留位；越界直接拒绝，避免作者数据悄悄被钳制。
inline uint32_t OptionalBoundedInteger(
    const nlohmann::json& object,
    const char* field,
    const std::string& context,
    uint32_t fallback,
    uint32_t minValue,
    uint32_t maxValue)
{
    if (!object.contains(field))
        return fallback;
    if (!object.at(field).is_number_integer())
        throw std::invalid_argument(context + " requires integer: " + field);
    const int64_t value = object.at(field).get<int64_t>();
    if (value < static_cast<int64_t>(minValue) ||
        value > static_cast<int64_t>(maxValue))
        throw std::invalid_argument(
            context + " requires integer in [" + std::to_string(minValue) +
            ", " + std::to_string(maxValue) + "]: " + field);
    return static_cast<uint32_t>(value);
}

// 与 Scene/Texture 共享同一条作者数据校验入口；不读高度图、不生成网格。
inline void Validate(const nlohmann::json& asset, const std::string& assetPath)
{
    if (!asset.is_object())
        throw std::invalid_argument("Terrain asset must be an object: " + assetPath);

    for (const auto& entry : asset.items())
    {
        if (entry.key() != "name" && entry.key() != "type" && entry.key() != "description" &&
            entry.key() != "heightmapPath" && entry.key() != "paintPath" &&
            entry.key() != "size" &&
            entry.key() != "heightScale" && entry.key() != "heightOffset" &&
            entry.key() != "gridResolution" && entry.key() != "chunkQuads" &&
            entry.key() != "heightmapMargin" &&
            entry.key() != "lodLevels" && entry.key() != "lodDistances" &&
            entry.key() != "chunkResidencyDistances" &&
            entry.key() != "pageOrigin" && entry.key() != "pageTilesPerSide" &&
            entry.key() != "materialInstancePath" && entry.key() != "layers")
        {
            throw std::invalid_argument(
                "Unknown terrain field: " + entry.key() + " in " + assetPath);
        }
    }

    if (!asset.contains("type") || !asset.at("type").is_string() ||
        asset.at("type").get<std::string>() != "terrain")
        throw std::invalid_argument("Terrain type must be terrain: " + assetPath);
    if (!asset.contains("name") || !asset.at("name").is_string() ||
        asset.at("name").get<std::string>().empty())
        throw std::invalid_argument("Terrain requires non-empty name: " + assetPath);
    if (asset.contains("description") && !asset.at("description").is_string())
        throw std::invalid_argument("Terrain description must be string: " + assetPath);
    for (const char* field : {"heightmapPath", "materialInstancePath"})
    {
        if (!asset.contains(field) || !asset.at(field).is_string() ||
            asset.at(field).get<std::string>().empty())
            throw std::invalid_argument(
                std::string("Terrain requires non-empty string: ") + field + " in " + assetPath);
    }

    if (!asset.contains("size") || !asset.at("size").is_array() || asset.at("size").size() != 2)
        throw std::invalid_argument("Terrain size must be a vec2 in cm: " + assetPath);
    for (const auto& component : asset.at("size"))
    {
        if (!component.is_number() || !std::isfinite(component.get<double>()) ||
            component.get<double>() <= 0.0)
            throw std::invalid_argument("Terrain size requires positive finite cm: " + assetPath);
    }
    // heightScale 只要求有限数：0 表示平地（权重图归一化高度取 0.5），负值表示
    // 高度翻转；两者都合法，不能在这里拒绝。归一化/钳制规则见
    // terrainWeightMap.cpp 的 SampleAt。
    RequireFiniteNumber(asset, "heightScale", "Terrain " + assetPath);
    RequireFiniteNumber(asset, "heightOffset", "Terrain " + assetPath);
    const uint32_t gridResolution = RequirePositiveInteger(asset, "gridResolution", "Terrain " + assetPath);
    const uint32_t chunkQuads = RequirePositiveInteger(asset, "chunkQuads", "Terrain " + assetPath);
    const uint32_t heightmapMargin = OptionalBoundedInteger(
        asset, "heightmapMargin", "Terrain " + assetPath, 0u, 0u, 16u);
    if (chunkQuads > gridResolution || gridResolution % chunkQuads != 0)
        throw std::invalid_argument(
            "Terrain gridResolution must be a positive multiple of chunkQuads: " + assetPath);
    if (heightmapMargin > gridResolution)
        throw std::invalid_argument(
            "Terrain heightmapMargin must be <= gridResolution: " + assetPath);

    // CPU 几何 LOD：只声明可生成的级数，运行时选择尚未接入。最粗一级的
    // 合并步长必须整除 chunkQuads，否则该级网格无法生成，必须在预检期拒绝，
    // 而不是等 World 加载时才抛错。
    const uint32_t lodLevels = asset.contains("lodLevels")
        ? OptionalBoundedInteger(
            asset, "lodLevels", "Terrain " + assetPath,
            1u, 1u, TerrainMaxLodLevels)
        : 1u;
    const uint32_t coarsestLodStep = TerrainLodStepQuads(lodLevels - 1);
    if (chunkQuads % coarsestLodStep != 0)
        throw std::invalid_argument(
            "Terrain chunkQuads must be divisible by the coarsest LOD step: " + assetPath);
    if (asset.contains("lodDistances"))
    {
        const auto& distances = asset.at("lodDistances");
        if (!distances.is_array())
            throw std::invalid_argument(
                "Terrain lodDistances must be a number array: " + assetPath);
        // 每个阈值都是“进入下一级”的距离，因此级数 N 只有 N-1 个阈值；
        // 只有 LOD0 时不允许写非空距离表。
        if (distances.size() != static_cast<size_t>(lodLevels - 1))
            throw std::invalid_argument(
                "Terrain lodDistances requires lodLevels-1 ascending distances: " + assetPath);
        double previous = 0.0;
        for (const auto& entry : distances)
        {
            if (!entry.is_number())
                throw std::invalid_argument(
                    "Terrain lodDistances must be numeric: " + assetPath);
            const double value = entry.get<double>();
            if (!std::isfinite(value) || value <= 0.0)
                throw std::invalid_argument(
                    "Terrain lodDistances must be positive finite cm: " + assetPath);
            if (value <= previous)
                throw std::invalid_argument(
                    "Terrain lodDistances must be strictly ascending: " + assetPath);
            previous = value;
        }
    }
    // 几何 chunk 驻留窗口：可选，长度必须是 lodLevels-1，严格升序正数。
    // 语义是“该级在距离内才创建”，最粗一级永远创建、不进入这张表。
    if (asset.contains("chunkResidencyDistances"))
    {
        const auto& distances = asset.at("chunkResidencyDistances");
        if (!distances.is_array())
            throw std::invalid_argument(
                "Terrain chunkResidencyDistances must be a number array: " +
                assetPath);
        if (distances.size() != static_cast<size_t>(lodLevels - 1))
            throw std::invalid_argument(
                "Terrain chunkResidencyDistances requires lodLevels-1 ascending distances: " +
                assetPath);
        double previous = 0.0;
        for (const auto& entry : distances)
        {
            if (!entry.is_number())
                throw std::invalid_argument(
                    "Terrain chunkResidencyDistances must be numeric: " +
                    assetPath);
            const double value = entry.get<double>();
            if (!std::isfinite(value) || value <= 0.0)
                throw std::invalid_argument(
                    "Terrain chunkResidencyDistances must be positive finite cm: " +
                    assetPath);
            if (value <= previous)
                throw std::invalid_argument(
                    "Terrain chunkResidencyDistances must be strictly ascending: " +
                    assetPath);
            previous = value;
        }
    }

    const uint32_t pageTilesPerSide = asset.contains("pageTilesPerSide")
        ? RequirePositiveInteger(asset, "pageTilesPerSide", "Terrain " + assetPath)
        : 8u;
    if (pageTilesPerSide > 32)
        throw std::invalid_argument("Terrain pageTilesPerSide must be <= 32: " + assetPath);
    Eigen::Vector2i pageOrigin = Eigen::Vector2i(0, 0);
    if (asset.contains("pageOrigin"))
    {
        const auto& origin = asset.at("pageOrigin");
        if (!origin.is_array() || origin.size() != 2 ||
            !origin[0].is_number_integer() || !origin[1].is_number_integer())
            throw std::invalid_argument("Terrain pageOrigin must be an integer vec2: " + assetPath);
        pageOrigin = Eigen::Vector2i(origin[0].get<int>(), origin[1].get<int>());
        if (pageOrigin.x() < 0 || pageOrigin.y() < 0)
            throw std::invalid_argument("Terrain pageOrigin must be non-negative: " + assetPath);
    }
    if (pageOrigin.x() + static_cast<int>(pageTilesPerSide) > 32 ||
        pageOrigin.y() + static_cast<int>(pageTilesPerSide) > 32)
        throw std::invalid_argument("Terrain pageOrigin + pageTilesPerSide exceeds global page table: " + assetPath);

    // 可选 paintPath：资源根相对的 .vlpaint 快照。文件可以尚不存在（首次
    // Save Paint 的写入目标），但路径本身必须是安全的相对路径，并绑定一层
    // 以上的 layer 表；否则保存出的快照无法与任何地形层身份对应。
    if (asset.contains("paintPath"))
    {
        if (!asset.at("paintPath").is_string())
            throw std::invalid_argument(
                "Terrain paintPath must be a string: " + assetPath);
        const std::string paintPath = asset.at("paintPath").get<std::string>();
        if (paintPath.empty() || paintPath.size() > 1024)
            throw std::invalid_argument(
                "Terrain paintPath requires 1..1024 characters: " + assetPath);
        const std::filesystem::path parsed(paintPath);
        if (parsed.is_absolute() || parsed.has_root_name() ||
            parsed.has_root_directory())
            throw std::invalid_argument(
                "Terrain paintPath must be resource-root-relative: " + assetPath);
        for (const auto& part : parsed)
            if (part == "..")
                throw std::invalid_argument(
                    "Terrain paintPath must not contain traversal: " + assetPath);
        std::string extension = parsed.extension().string();
        for (char& character : extension)
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        if (extension != ".vlpaint")
            throw std::invalid_argument(
                "Terrain paintPath must use the .vlpaint extension: " + assetPath);
        if (!asset.contains("layers") || !asset.at("layers").is_array() ||
            asset.at("layers").empty())
            throw std::invalid_argument(
                "Terrain paintPath requires at least one layer: " + assetPath);
    }

    if (!asset.contains("layers")) return;
    const auto& layers = asset.at("layers");
    if (!layers.is_array() || layers.empty() || layers.size() > 256)
        throw std::invalid_argument("Terrain layers requires 1..256 entries: " + assetPath);
    // 缺省 layerKey 是 "<assetPath>#<name>"，同 TR 内重名会让两层争同一个
    // globalId；显式 key 重复同理。这里在资产预检期就拒绝。
    std::vector<std::string> layerKeys;
    layerKeys.reserve(layers.size());
    for (const auto& layer : layers)
    {
        if (!layer.is_object() || !layer.contains("name") || !layer.at("name").is_string() ||
            layer.at("name").get<std::string>().empty())
            throw std::invalid_argument("Terrain layer requires non-empty name: " + assetPath);
        // 可选跨 TR 共享身份：非空、不超过 256 字节；sourceSet 不参与 key。
        std::string layerKey;
        if (layer.contains("key"))
        {
            if (!layer.at("key").is_string())
                throw std::invalid_argument(
                    "Terrain layer key must be a string: " + assetPath);
            layerKey = layer.at("key").get<std::string>();
            if (layerKey.empty() || layerKey.size() > 256)
                throw std::invalid_argument(
                    "Terrain layer key requires 1..256 characters: " + assetPath);
        }
        else
        {
            layerKey = assetPath + "#" + layer.at("name").get<std::string>();
        }
        for (const std::string& existing : layerKeys)
        {
            if (existing == layerKey)
                throw std::invalid_argument(
                    "Terrain layer key is duplicated: key=" + layerKey +
                    " in " + assetPath);
        }
        layerKeys.push_back(std::move(layerKey));
        for (const char* field : {"heightRange", "slopeRange"})
        {
            if (!layer.contains(field) || !layer.at(field).is_array() || layer.at(field).size() != 2)
                throw std::invalid_argument(
                    std::string("Terrain layer requires vec2: ") + field + " in " + assetPath);
            const auto& range = layer.at(field);
            if (!range[0].is_number() || !range[1].is_number())
                throw std::invalid_argument(
                    std::string("Invalid terrain layer range: ") + field + " in " + assetPath);
            const double min = range[0].get<double>();
            const double max = range[1].get<double>();
            if (
                !std::isfinite(min) || !std::isfinite(max) || min > max || min < 0.0)
                throw std::invalid_argument(
                    std::string("Invalid terrain layer range: ") + field + " in " + assetPath);
            if (std::string(field) == "heightRange" && max > 1.0)
                throw std::invalid_argument("Terrain layer heightRange must stay in 0..1: " + assetPath);
            if (std::string(field) == "slopeRange" && max > 90.0)
                throw std::invalid_argument("Terrain layer slopeRange must stay in 0..90 degrees: " + assetPath);
        }
        const float noise = layer.contains("noise")
            ? RequireFiniteNumber(layer, "noise", "Terrain layer " + assetPath)
            : 0.0f;
        if (noise < 0.0f)
            throw std::invalid_argument("Terrain layer noise must be non-negative: " + assetPath);
        if (layer.contains("uvScale"))
        {
            const float uvScale = RequireFiniteNumber(layer, "uvScale", "Terrain layer " + assetPath);
            if (uvScale <= 0.0f)
                throw std::invalid_argument("Terrain layer uvScale must be positive: " + assetPath);
        }
        if (layer.contains("uvRotation"))
            static_cast<void>(RequireFiniteNumber(layer, "uvRotation", "Terrain layer " + assetPath));
        // L1 catalog v2 可选字段。sourceResolution 只接受设计里列出的三档，
        // 因为它直接决定 pagesPerSide 与 mip 数；512/1024/2048 -> 4/8/16 页/边。
        if (layer.contains("sourceResolution"))
        {
            const uint32_t resolution = OptionalBoundedInteger(
                layer, "sourceResolution", "Terrain layer " + assetPath,
                1024u, 512u, 2048u);
            if (!IsSupportedTerrainLayerSourceResolution(resolution))
                throw std::invalid_argument(
                    "Terrain layer sourceResolution must be 512, 1024 or 2048: " + assetPath);
        }
        if (layer.contains("avgColor"))
        {
            const auto& color = layer.at("avgColor");
            if (!color.is_array() || color.size() != 3)
                throw std::invalid_argument(
                    "Terrain layer avgColor must be a vec3: " + assetPath);
            for (const auto& component : color)
            {
                if (!component.is_number())
                    throw std::invalid_argument(
                        "Terrain layer avgColor must be numeric: " + assetPath);
                const double value = component.get<double>();
                if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                    throw std::invalid_argument(
                        "Terrain layer avgColor must stay in 0..1: " + assetPath);
            }
        }
        static_cast<void>(OptionalBoundedInteger(
            layer, "sourceSet", "Terrain layer " + assetPath, 0u, 0u, 255u));
        static_cast<void>(OptionalBoundedInteger(
            layer, "flags", "Terrain layer " + assetPath, 0u, 0u, 65535u));
        for (const auto& entry : layer.items())
            if (entry.key() != "name" && entry.key() != "key" &&
                entry.key() != "heightRange" &&
                entry.key() != "slopeRange" && entry.key() != "noise" &&
                entry.key() != "uvScale" && entry.key() != "uvRotation" &&
                entry.key() != "sourceResolution" && entry.key() != "avgColor" &&
                entry.key() != "sourceSet" && entry.key() != "flags")
                throw std::invalid_argument(
                    "Unknown terrain layer field: " + entry.key() + " in " + assetPath);
    }
}

} // namespace TerrainAssetValidator
} // namespace VL
