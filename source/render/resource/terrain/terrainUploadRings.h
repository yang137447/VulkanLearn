#pragma once

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "terrain/terrainPageManager.h"

namespace VL
{
class RendererBackendVulkan;
struct TerrainUploadStagingOffsets
{
    vk::DeviceSize pageTable = 0;
    vk::DeviceSize layerIds = 0;
    vk::DeviceSize layerWeights = 0;
};

class TerrainUploadGpuRing
{
public:
    TerrainUploadGpuRing(RendererBackendVulkan& backend, uint32_t maxAtlasPages);
    ~TerrainUploadGpuRing();
    TerrainUploadGpuRing(const TerrainUploadGpuRing&) = delete;
    TerrainUploadGpuRing& operator=(const TerrainUploadGpuRing&) = delete;
    uint32_t SlotCount() const noexcept { return slotCount; }
    uint32_t MaxAtlasPages() const noexcept { return maxAtlasPages; }
    bool Fits(const TerrainUploadBatch& batch) const noexcept;
    TerrainUploadStagingOffsets Write(uint32_t frameIndex, const TerrainUploadBatch& batch);
    vk::Buffer Buffer(uint32_t frameIndex) const;
private:
    RendererBackendVulkan* rendererBackend = nullptr;
    uint32_t slotCount = 0;
    uint32_t maxAtlasPages = 0;
    vk::DeviceSize slotBytes = 0;
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceMemory> memories;
    std::vector<void*> mapped;
};
vk::DeviceSize AlignTerrainUploadOffset(vk::DeviceSize value);
}
