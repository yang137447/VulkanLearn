#include "render/resource/terrain/terrainSourceSets.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "texture.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"

namespace VL
{
// S2：把一个地形 MI 绑定的三张层数组登记到它用到的每个 sourceSet。
// 同一 set 必须始终指向同一组 shared_ptr（否则 atlas 内容来源不确定）；
// 不同 set 可以指向不同数组，但仍共享同一组 atlas，因此格式必须一致。
// layerSlices 记录 globalId -> 该 MI 数组的 slice，上传时按它取源层。
void RegisterTerrainSourceSets(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState,
    const TerrainBuildPlan& plan,
    const std::shared_ptr<Texture>& layerAlbedo,
    const std::shared_ptr<Texture>& layerNormal,
    const std::shared_ptr<Texture>& layerPbr,
    bool vtRequested)
{
    const std::string placementId = TerrainPlacementIdentity(plan);
    const std::vector<uint8_t>& layerIdMap =
        streamingState.pageManager.PlacementLayerIdMap(placementId);
    if (layerIdMap.size() != plan.desc.layers.size())
    {
        throw std::runtime_error(
            "Terrain layer id map is missing before source set registration: " +
            plan.desc.assetPath);
    }
    const std::vector<TerrainLayerDesc>& worldLayers =
        streamingState.layerDefinitions;

    const uint32_t arrayLayers = layerAlbedo->GetArrayLayers();
    if (layerNormal->GetArrayLayers() != arrayLayers ||
        layerPbr->GetArrayLayers() != arrayLayers)
    {
        throw std::runtime_error(
            "Terrain layer albedo/normal/pbr arrays must have identical "
            "layer counts: " + plan.desc.assetPath);
    }
    const vk::Extent2D albedoExtent =
        rendererBackend.GetImageExtent(layerAlbedo->getImage());
    const vk::Extent2D normalExtent =
        rendererBackend.GetImageExtent(layerNormal->getImage());
    const vk::Extent2D pbrExtent =
        rendererBackend.GetImageExtent(layerPbr->getImage());
    const bool sourceExtentConsistent =
        albedoExtent.width == albedoExtent.height &&
        albedoExtent.width != 0 &&
        normalExtent.width == albedoExtent.width &&
        normalExtent.height == albedoExtent.height &&
        pbrExtent.width == albedoExtent.width &&
        pbrExtent.height == albedoExtent.height;
    const vk::Format albedoFormat = layerAlbedo->GetFormat();
    const vk::Format normalFormat = layerNormal->GetFormat();
    const vk::Format pbrFormat = layerPbr->GetFormat();

    // 本 TR 用到的 set（按层顺序去重）。
    std::vector<uint32_t> usedSets;
    for (size_t local = 0; local < plan.desc.layers.size(); ++local)
    {
        const uint32_t sourceSet = worldLayers[layerIdMap[local]].sourceSet;
        if (std::find(usedSets.begin(), usedSets.end(), sourceSet) ==
            usedSets.end())
        {
            usedSets.push_back(sourceSet);
        }
    }

    for (uint32_t sourceSet : usedSets)
    {
        const auto found = streamingState.layerSources.find(sourceSet);
        if (found == streamingState.layerSources.end())
        {
            TerrainSourceSetRecord record;
            record.sourceSet = sourceSet;
            record.albedo = layerAlbedo;
            record.normal = layerNormal;
            record.pbr = layerPbr;
            record.arrayLayers = arrayLayers;
            record.albedoMipLevels = layerAlbedo->getMipLevels();
            record.normalMipLevels = layerNormal->getMipLevels();
            record.pbrMipLevels = layerPbr->getMipLevels();
            record.albedoWidth = albedoExtent.width;
            record.albedoHeight = albedoExtent.height;
            record.normalWidth = normalExtent.width;
            record.normalHeight = normalExtent.height;
            record.pbrWidth = pbrExtent.width;
            record.pbrHeight = pbrExtent.height;
            record.albedoFormat = static_cast<int32_t>(albedoFormat);
            record.normalFormat = static_cast<int32_t>(normalFormat);
            record.pbrFormat = static_cast<int32_t>(pbrFormat);
            streamingState.layerSources.emplace(sourceSet, std::move(record));
        }
        else
        {
            const TerrainSourceSetRecord& existing = found->second;
            if (existing.albedo != layerAlbedo ||
                existing.normal != layerNormal ||
                existing.pbr != layerPbr)
            {
                throw std::runtime_error(
                    "All layer-table terrains must share the same "
                    "albedo/normal/pbr arrays within one sourceSet: "
                    "sourceSet=" + std::to_string(sourceSet) +
                    " assetPath=" + plan.desc.assetPath);
            }
            if (existing.albedoFormat != static_cast<int32_t>(albedoFormat) ||
                existing.normalFormat != static_cast<int32_t>(normalFormat) ||
                existing.pbrFormat != static_cast<int32_t>(pbrFormat))
            {
                throw std::runtime_error(
                    "All sourceSets must share atlas formats before "
                    "multi-format groups exist: sourceSet=" +
                    std::to_string(sourceSet) +
                    " assetPath=" + plan.desc.assetPath);
            }
        }

        TerrainSourceSetRecord& record = streamingState.layerSources[sourceSet];
        // 单一 atlas 格式组：所有 set 的三张图格式必须一致（S5 才扩展）。
        for (const auto& other : streamingState.layerSources)
        {
            const TerrainSourceSetRecord& reference = other.second;
            if (reference.albedoFormat != record.albedoFormat ||
                reference.normalFormat != record.normalFormat ||
                reference.pbrFormat != record.pbrFormat)
            {
                throw std::runtime_error(
                    "Terrain sourceSets must share the same layer texture "
                    "formats (single atlas group): set=" +
                    std::to_string(reference.sourceSet) + " vs set=" +
                    std::to_string(sourceSet));
            }
        }

        bool resolutionConsistent = sourceExtentConsistent;
        for (size_t local = 0; local < plan.desc.layers.size(); ++local)
        {
            const uint32_t globalId = layerIdMap[local];
            const TerrainLayerDesc& layer = worldLayers[globalId];
            if (layer.sourceSet != sourceSet)
                continue;
            // 源数组按 MI 的局部层顺序排列；越界必须在加载期失败。
            if (local >= arrayLayers)
            {
                throw std::runtime_error(
                    "Terrain layer index exceeds the bound source texture "
                    "array layers: assetPath=" + plan.desc.assetPath +
                    " localLayer=" + std::to_string(local) +
                    " arrayLayers=" + std::to_string(arrayLayers));
            }
            if (!vtRequested && globalId >= arrayLayers)
            {
                throw std::runtime_error(
                    "Terrain mode 0 requires the layer array slice to match "
                    "the global layer id; use mode 1 or expand the array: "
                    "assetPath=" + plan.desc.assetPath +
                    " localLayer=" + std::to_string(local) +
                    " globalLayer=" + std::to_string(globalId) +
                    " arrayLayers=" + std::to_string(arrayLayers));
            }
            if (!sourceExtentConsistent ||
                layer.sourceResolution != albedoExtent.width)
            {
                resolutionConsistent = false;
            }
            const auto inserted =
                record.layerSlices.emplace(globalId, static_cast<uint32_t>(local));
            if (!inserted.second && inserted.first->second != local)
            {
                throw std::runtime_error(
                    "Global terrain layer maps to two different source "
                    "slices in one sourceSet: layerId=" +
                    std::to_string(globalId) +
                    " sourceSet=" + std::to_string(sourceSet));
            }
        }
        if (!resolutionConsistent)
        {
            record.catalogResolutionMatches = false;
            std::cout
                << "Warning: terrain layer sourceResolution does not match "
                << "the bound layer texture extent (sourceSet=" << sourceSet
                << ", source=" << albedoExtent.width << "x"
                << albedoExtent.height
                << "); VT layer mode 1 requires a matching source extent; "
                << "mode 0 remains available: "
                << plan.desc.assetPath << std::endl;
        }
    }
}
} // namespace VL