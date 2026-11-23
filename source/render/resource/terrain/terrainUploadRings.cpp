#include "render/resource/terrain/terrainUploadRings.h"
#include <cstring>
#include <stdexcept>
#include "render/backend/rendererBackendVulkan.h"

namespace VL
{
vk::DeviceSize AlignTerrainUploadOffset(vk::DeviceSize value)
{
    return (value + 15) / 16 * 16;
}

TerrainUploadGpuRing::TerrainUploadGpuRing(RendererBackendVulkan& backend, uint32_t maxAtlasPageCount)
    : rendererBackend(&backend), maxAtlasPages(maxAtlasPageCount)
{
    slotCount = backend.GetFrameFenceCount();
    if (!slotCount || !maxAtlasPages)
        throw std::invalid_argument("Coverage upload ring requires positive frame/page counts");
    constexpr vk::DeviceSize PageBytes =
        vk::DeviceSize(TerrainPageManager::PageResolution) * TerrainPageManager::PageResolution * 4;
    slotBytes = AlignTerrainUploadOffset(TerrainPageManager::PageSlotCount * 4) +
        AlignTerrainUploadOffset(maxAtlasPages * PageBytes) * 2;
    try
    {
        for (uint32_t slot = 0; slot < slotCount; ++slot)
        {
            const auto buffer = backend.CreateBuffer(slotBytes, vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "TerrainCoverageUploadRing" + std::to_string(slot));
            buffers.push_back(buffer.first);
            memories.push_back(buffer.second);
            mapped.push_back(backend.MapMemory(buffer.second, slotBytes));
        }
    }
    catch (...)
    {
        for (size_t slot = 0; slot < buffers.size(); ++slot)
        {
            if (slot < mapped.size() && mapped[slot])
                backend.UnmapMemory(memories[slot]);
            backend.DestroyBuffer(buffers[slot], memories[slot]);
        }
        throw;
    }
}

TerrainUploadGpuRing::~TerrainUploadGpuRing()
{
    for (size_t slot = 0; slot < buffers.size(); ++slot)
    {
        if (mapped[slot])
            rendererBackend->UnmapMemory(memories[slot]);
        rendererBackend->DestroyBuffer(buffers[slot], memories[slot]);
    }
}

bool TerrainUploadGpuRing::Fits(const TerrainUploadBatch& batch) const noexcept
{
    if (batch.atlasUploads.size() > maxAtlasPages ||
        batch.pageTableUploads.size() > TerrainPageManager::PageSlotCount)
        return false;
    constexpr size_t PageBytes =
        size_t(TerrainPageManager::PageResolution) * TerrainPageManager::PageResolution * 4;
    for (const auto& page : batch.atlasUploads)
        if (page.ids.size() != PageBytes || page.weights.size() != PageBytes)
            return false;
    return true;
}

TerrainUploadStagingOffsets TerrainUploadGpuRing::Write(
    uint32_t frameIndex, const TerrainUploadBatch& batch)
{
    if (frameIndex >= buffers.size() || !Fits(batch))
        throw std::invalid_argument("Coverage upload batch/frame exceeds ring bounds");
    constexpr vk::DeviceSize PageBytes =
        vk::DeviceSize(TerrainPageManager::PageResolution) * TerrainPageManager::PageResolution * 4;
    TerrainUploadStagingOffsets offsets;
    offsets.layerIds = AlignTerrainUploadOffset(batch.pageTableUploads.size() * 4);
    offsets.layerWeights = offsets.layerIds + AlignTerrainUploadOffset(batch.atlasUploads.size() * PageBytes);
    auto* destination = static_cast<uint8_t*>(mapped[frameIndex]);
    for (size_t index = 0; index < batch.pageTableUploads.size(); ++index)
        std::memcpy(destination + index * 4, batch.pageTableUploads[index].entry, 4);
    for (size_t index = 0; index < batch.atlasUploads.size(); ++index)
    {
        std::memcpy(destination + offsets.layerIds + index * PageBytes,
            batch.atlasUploads[index].ids.data(), PageBytes);
        std::memcpy(destination + offsets.layerWeights + index * PageBytes,
            batch.atlasUploads[index].weights.data(), PageBytes);
    }
    return offsets;
}

vk::Buffer TerrainUploadGpuRing::Buffer(uint32_t frameIndex) const
{
    return frameIndex < buffers.size() ? buffers[frameIndex] : vk::Buffer{};
}
}
