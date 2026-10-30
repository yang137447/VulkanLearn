#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

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
            entry.key() != "heightmapPath" && entry.key() != "size" &&
            entry.key() != "heightScale" && entry.key() != "heightOffset" &&
            entry.key() != "gridResolution" && entry.key() != "chunkQuads" &&
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
    if (chunkQuads > gridResolution || gridResolution % chunkQuads != 0)
        throw std::invalid_argument(
            "Terrain gridResolution must be a positive multiple of chunkQuads: " + assetPath);

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

    if (!asset.contains("layers")) return;
    const auto& layers = asset.at("layers");
    if (!layers.is_array() || layers.empty() || layers.size() > 256)
        throw std::invalid_argument("Terrain layers requires 1..256 entries: " + assetPath);
    for (const auto& layer : layers)
    {
        if (!layer.is_object() || !layer.contains("name") || !layer.at("name").is_string() ||
            layer.at("name").get<std::string>().empty())
            throw std::invalid_argument("Terrain layer requires non-empty name: " + assetPath);
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
            if (entry.key() != "name" && entry.key() != "heightRange" &&
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
