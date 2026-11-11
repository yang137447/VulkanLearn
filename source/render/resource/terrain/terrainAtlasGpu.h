#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainLayerPageManager.h"

class Texture;

namespace VL
{
class RendererBackendVulkan;
struct TerrainStreamingState;
struct TerrainPageGpuResources
{
    std::shared_ptr<Texture> pageTable;
    std::shared_ptr<Texture> layerIds;
    std::shared_ptr<Texture> layerWeights;
};

struct TerrainLayerGpuResources
{
    std::shared_ptr<Texture> layerTable;
    // L2：每 mip 一张 RGBA32F 页表（nearest / clamp / 无 mip）。
    std::array<
        std::shared_ptr<Texture>,
        TerrainLayerPageManager::MaxMipLevel + 1> pageTables;
    // 三张 layer atlas 都是 4096² × slice 的 sampler2DArray，无 mip。
    std::shared_ptr<Texture> albedoAtlas;
    std::shared_ptr<Texture> normalAtlas;
    std::shared_ptr<Texture> pbrAtlas;
    uint32_t atlasSlices = 0;
    uint32_t physicalPageCount = 0;
    vk::Format albedoFormat = vk::Format::eUndefined;
    vk::Format normalFormat = vk::Format::eUndefined;
    vk::Format pbrFormat = vk::Format::eUndefined;
    // 单张 atlas（全部 slice）的估算字节数；统计按源格式计算。
    uint64_t atlasBytesPerMap = 0;
    uint64_t albedoBytes = 0;
    uint64_t normalBytes = 0;
    uint64_t pbrBytes = 0;
    uint64_t atlasBytesTotal = 0;
    bool atlasesCreated = false;
};
// page table 始终创建（即使 World 没有地形），feedback pass 的
// worldTexture 输入需要一张稳定的 32×32 零页表。
std::shared_ptr<Texture> CreateTerrainPageTableGpuResource(
    RendererBackendVulkan& rendererBackend);

// page table 之外的 ID/weight atlas；只有真正注册了地形页时才创建。
void CreateTerrainPageAtlasGpuResources(
    RendererBackendVulkan& rendererBackend,
    TerrainPageGpuResources& resources);

TerrainPageGpuResources CreateTerrainPageGpuResources(
    RendererBackendVulkan& rendererBackend);

TerrainLayerGpuResources CreateTerrainLayerGpuResources(
    RendererBackendVulkan& rendererBackend,
    const std::vector<TerrainLayerDesc>& layers,
    const TerrainLayerPageManager& layerPageManager);

// L2：用源层数组的格式/层数创建三张空 atlas（4096² × slice，无 mip），
// 并用 record-only clear 把整张 atlas 置零；不做 CPU 零图上传。
void CreateTerrainLayerAtlasGpuResources(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState);

// Shared format/bytes helpers used by atlas creation and upload statistics.
bool IsBlockCompressedFormat(vk::Format format);
uint64_t EstimateTextureBytes(
    vk::Format format,
    uint32_t width,
    uint32_t height,
    uint32_t arrayLayers);

} // namespace VL