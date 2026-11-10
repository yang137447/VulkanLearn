#include "terrain/terrainLayerRegistry.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace VL
{

std::string TerrainLayerRegistry::ResolveLayerKey(
    const TerrainLayerDesc& layer,
    const std::string& assetPath)
{
    if (!layer.key.empty())
        return layer.key;
    return assetPath + "#" + layer.name;
}

bool TerrainLayerRegistry::LayersEquivalent(
    const TerrainLayerDesc& lhs,
    const TerrainLayerDesc& rhs)
{
    return lhs.name == rhs.name &&
        std::abs(lhs.heightMin - rhs.heightMin) <= 1.0e-6f &&
        std::abs(lhs.heightMax - rhs.heightMax) <= 1.0e-6f &&
        std::abs(lhs.slopeMin - rhs.slopeMin) <= 1.0e-6f &&
        std::abs(lhs.slopeMax - rhs.slopeMax) <= 1.0e-6f &&
        std::abs(lhs.noise - rhs.noise) <= 1.0e-6f &&
        std::abs(lhs.uvScale - rhs.uvScale) <= 1.0e-6f &&
        std::abs(lhs.uvRotation - rhs.uvRotation) <= 1.0e-6f &&
        lhs.sourceResolution == rhs.sourceResolution &&
        lhs.sourceSet == rhs.sourceSet &&
        lhs.flags == rhs.flags &&
        (lhs.avgColor - rhs.avgColor).cwiseAbs().maxCoeff() <= 1.0e-6f;
}

const std::vector<uint8_t>& TerrainLayerRegistry::RegisterTerrain(
    const TerrainBuildPlan& plan)
{
    const std::string placementId = TerrainPlacementIdentity(plan);
    if (layerIdMapsByPlacement.find(placementId) !=
        layerIdMapsByPlacement.end())
    {
        throw std::runtime_error(
            "Terrain placement is already registered in the global layer "
            "registry: assetPath=" + plan.desc.assetPath +
            " placementId=" + placementId);
    }

    // 同一 TR 内 name 必须唯一，否则缺省 key 会撞车；显式 key 也不能重复，
    // 否则一个 TR 的两层会争同一个 globalId。
    std::unordered_set<std::string> localKeys;
    std::vector<uint8_t> mapping;
    mapping.reserve(plan.desc.layers.size());

    for (const TerrainLayerDesc& layer : plan.desc.layers)
    {
        const std::string key = ResolveLayerKey(layer, plan.desc.assetPath);
        if (!localKeys.insert(key).second)
        {
            throw std::runtime_error(
                "Terrain layer key is duplicated inside one terrain: key=" +
                key + " assetPath=" + plan.desc.assetPath);
        }

        const auto found = layerIdByKey.find(key);
        if (found != layerIdByKey.end())
        {
            const TerrainGlobalLayerRecord& existing = layers[found->second];
            if (!LayersEquivalent(existing.layer, layer))
            {
                // 同 key 不同参数是作者错误：不覆盖、不合并，直接失败，
                // 错误里同时给出两个资产路径。
                throw std::runtime_error(
                    "Terrain layer key conflict: key=" + key +
                    " firstAsset=" + existing.assetPaths.front() +
                    " secondAsset=" + plan.desc.assetPath +
                    " layer=" + layer.name);
            }
            if (std::find(
                    existing.assetPaths.begin(),
                    existing.assetPaths.end(),
                    plan.desc.assetPath) == existing.assetPaths.end())
            {
                layers[found->second].assetPaths.push_back(
                    plan.desc.assetPath);
            }
            mapping.push_back(static_cast<uint8_t>(found->second));
            continue;
        }

        if (layers.size() >= MaxGlobalLayers)
        {
            throw std::runtime_error(
                "Terrain global layer capacity exceeded (max " +
                std::to_string(MaxGlobalLayers) + "): assetPath=" +
                plan.desc.assetPath + " key=" + key);
        }
        TerrainGlobalLayerRecord record;
        record.key = key;
        record.layer = layer;
        record.layer.key = key;
        record.assetPaths.push_back(plan.desc.assetPath);
        record.placementCount = 0;
        layerIdByKey.emplace(key, static_cast<uint32_t>(layers.size()));
        mapping.push_back(static_cast<uint8_t>(layers.size()));
        layers.push_back(std::move(record));
    }

    const std::vector<uint8_t>& stored = layerIdMapsByPlacement.emplace(
        placementId, std::move(mapping)).first->second;
    // placementCount 按“引用该全局层的摆放数”统计，便于控制台区分去重收益。
    std::unordered_set<uint32_t> referenced;
    for (uint8_t globalId : stored)
        referenced.insert(globalId);
    for (uint32_t globalId : referenced)
        ++layers[globalId].placementCount;
    return stored;
}

std::vector<TerrainLayerDesc> TerrainLayerRegistry::LayerDescriptions() const
{
    std::vector<TerrainLayerDesc> descriptions;
    descriptions.reserve(layers.size());
    for (const TerrainGlobalLayerRecord& record : layers)
        descriptions.push_back(record.layer);
    return descriptions;
}

const std::vector<uint8_t>* TerrainLayerRegistry::FindLayerIdMap(
    const std::string& placementId) const
{
    const auto found = layerIdMapsByPlacement.find(placementId);
    return found == layerIdMapsByPlacement.end() ? nullptr : &found->second;
}

uint32_t TerrainLayerRegistry::DedupedLayerCount() const
{
    uint32_t localLayerTotal = 0;
    for (const auto& entry : layerIdMapsByPlacement)
        localLayerTotal += static_cast<uint32_t>(entry.second.size());
    return localLayerTotal > layers.size()
        ? localLayerTotal - static_cast<uint32_t>(layers.size())
        : 0;
}

void TerrainLayerRegistry::Clear()
{
    layers.clear();
    layerIdByKey.clear();
    layerIdMapsByPlacement.clear();
}

} // namespace VL
