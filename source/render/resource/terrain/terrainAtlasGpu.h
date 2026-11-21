#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "terrain/terrainAssetTypes.h"

class Texture;
namespace VL
{
class RendererBackendVulkan;
struct TerrainPageGpuResources
{
    std::shared_ptr<Texture> pageTable;
    std::shared_ptr<Texture> layerIds;
    std::shared_ptr<Texture> layerWeights;
};

struct TerrainLayerGpuResources
{
    std::shared_ptr<Texture> layerTable;
};

std::shared_ptr<Texture> CreateTerrainPageTableGpuResource(RendererBackendVulkan& backend);
void CreateTerrainPageAtlasGpuResources(RendererBackendVulkan& backend, TerrainPageGpuResources& resources);
TerrainPageGpuResources CreateTerrainPageGpuResources(RendererBackendVulkan& backend);
TerrainLayerGpuResources CreateTerrainLayerGpuResources(
    RendererBackendVulkan& backend, const std::vector<TerrainLayerDesc>& layers);
bool IsBlockCompressedFormat(vk::Format format);
uint64_t EstimateTextureBytes(vk::Format format, uint32_t width, uint32_t height, uint32_t arrayLayers);
}
