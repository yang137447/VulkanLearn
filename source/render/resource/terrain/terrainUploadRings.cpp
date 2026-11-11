#include "render/resource/terrain/terrainUploadRings.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "render/backend/rendererBackendVulkan.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainPageManager.h"

namespace VL
{

vk::DeviceSize AlignTerrainUploadOffset(vk::DeviceSize value)
{
    constexpr vk::DeviceSize Alignment = 16;
    return (value + Alignment - 1) / Alignment * Alignment;
}
TerrainUploadGpuRing::TerrainUploadGpuRing(
    RendererBackendVulkan& backend,
    uint32_t maxAtlasPageCount)
    : rendererBackend(&backend)
    , maxAtlasPages(maxAtlasPageCount)
{
    slotCount = backend.GetFrameFenceCount();
    if (slotCount == 0)
    {
        throw std::runtime_error(
            "TerrainUploadGpuRing requires at least one frame fence");
    }
    if (maxAtlasPages == 0)
    {
        throw std::invalid_argument(
            "TerrainUploadGpuRing requires a positive atlas page budget");
    }

    constexpr vk::DeviceSize PageBytes =
        static_cast<vk::DeviceSize>(TerrainPageManager::PageResolution) *
        TerrainPageManager::PageResolution * 4;
    // 页表项每项 4 字节；即使一帧把所有虚拟页都标脏也只需 4 KiB。
    const vk::DeviceSize pageTableBaseBytes = AlignTerrainUploadOffset(
        static_cast<vk::DeviceSize>(TerrainPageManager::PageSlotCount) * 4);
    const vk::DeviceSize atlasBytes = AlignTerrainUploadOffset(
        static_cast<vk::DeviceSize>(maxAtlasPages) * PageBytes);
    slotBytes = pageTableBaseBytes + atlasBytes * 2;

    buffers.reserve(slotCount);
    memories.reserve(slotCount);
    mapped.reserve(slotCount);
    try
    {
        for (uint32_t slot = 0; slot < slotCount; ++slot)
        {
            const std::pair<vk::Buffer, vk::DeviceMemory> created =
                backend.CreateBuffer(
                    slotBytes,
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "TerrainUploadRing" + std::to_string(slot));
            buffers.push_back(created.first);
            memories.push_back(created.second);
            // 常驻映射：每帧只做 memcpy，不 map/unmap。
            mapped.push_back(backend.MapMemory(created.second, slotBytes));
        }
    }
    catch (...)
    {
        for (size_t slot = 0; slot < buffers.size(); ++slot)
        {
            if (slot < memories.size() && slot < mapped.size() &&
                mapped[slot] != nullptr)
                backend.UnmapMemory(memories[slot]);
            backend.DestroyBuffer(
                buffers[slot],
                slot < memories.size() ? memories[slot] : vk::DeviceMemory{});
        }
        buffers.clear();
        memories.clear();
        mapped.clear();
        throw;
    }
}

TerrainUploadGpuRing::~TerrainUploadGpuRing()
{
    if (rendererBackend == nullptr)
        return;
    for (size_t slot = 0; slot < buffers.size(); ++slot)
    {
        if (mapped[slot] != nullptr)
            rendererBackend->UnmapMemory(memories[slot]);
        rendererBackend->DestroyBuffer(buffers[slot], memories[slot]);
    }
}

bool TerrainUploadGpuRing::Fits(
    const TerrainUploadBatch& batch) const noexcept
{
    if (batch.atlasUploads.size() > maxAtlasPages)
        return false;
    for (const TerrainPageManager::AtlasPageUpload& upload :
         batch.atlasUploads)
    {
        constexpr size_t PageBytes =
            static_cast<size_t>(TerrainPageManager::PageResolution) *
            TerrainPageManager::PageResolution * 4;
        if (upload.ids.size() != PageBytes ||
            upload.weights.size() != PageBytes)
            return false;
    }
    return true;
}

TerrainUploadStagingOffsets TerrainUploadGpuRing::Write(
    uint32_t frameIndex,
    const TerrainUploadBatch& batch)
{
    if (frameIndex >= buffers.size())
    {
        throw std::out_of_range(
            "Terrain upload frame index is out of range");
    }
    if (!Fits(batch))
    {
        throw std::invalid_argument(
            "Terrain upload batch exceeds the staging ring capacity");
    }

    constexpr vk::DeviceSize PageBytes =
        static_cast<vk::DeviceSize>(TerrainPageManager::PageResolution) *
        TerrainPageManager::PageResolution * 4;
    const vk::DeviceSize pageTableBytes = AlignTerrainUploadOffset(
        static_cast<vk::DeviceSize>(batch.pageTableUploads.size()) * 4);
    const vk::DeviceSize idsBase = pageTableBytes;
    const vk::DeviceSize idsBytes = AlignTerrainUploadOffset(
        static_cast<vk::DeviceSize>(batch.atlasUploads.size()) * PageBytes);
    const vk::DeviceSize weightsBase = idsBase + idsBytes;

    TerrainUploadStagingOffsets offsets;
    offsets.pageTable = 0;
    offsets.layerIds = idsBase;
    offsets.layerWeights = weightsBase;

    if (batch.Empty())
        return offsets;

    uint8_t* destination = static_cast<uint8_t*>(mapped[frameIndex]);
    for (size_t index = 0; index < batch.pageTableUploads.size(); ++index)
    {
        std::memcpy(
            destination + static_cast<vk::DeviceSize>(index) * 4,
            batch.pageTableUploads[index].entry,
            4);
    }
    for (size_t index = 0; index < batch.atlasUploads.size(); ++index)
    {
        const vk::DeviceSize pageOffset =
            static_cast<vk::DeviceSize>(index) * PageBytes;
        std::memcpy(
            destination + idsBase + pageOffset,
            batch.atlasUploads[index].ids.data(),
            PageBytes);
        std::memcpy(
            destination + weightsBase + pageOffset,
            batch.atlasUploads[index].weights.data(),
            PageBytes);
    }
    return offsets;
}

vk::Buffer TerrainUploadGpuRing::Buffer(uint32_t frameIndex) const
{
    return frameIndex < buffers.size() ? buffers[frameIndex] : vk::Buffer{};
}


TerrainLayerUploadGpuRing::TerrainLayerUploadGpuRing(
    RendererBackendVulkan& backend,
    const TerrainLayerPageManager& layerPageManager,
    uint32_t maxLayerPagesPerFrame)
    : rendererBackend(&backend)
    , maxLayerPages(maxLayerPagesPerFrame)
{
    slotCount = backend.GetFrameFenceCount();
    if (slotCount == 0)
    {
        throw std::runtime_error(
            "TerrainLayerUploadGpuRing requires at least one frame fence");
    }
    if (maxLayerPages == 0)
    {
        throw std::invalid_argument(
            "TerrainLayerUploadGpuRing requires a positive page budget");
    }

    // 一帧的页表项可能跨多个 mip；按所有 mip 页表项总数上界分配，
    // 保证任何 dirty 子集都能一次性打包。2K/256 层约 1.4 MiB/槽。
    vk::DeviceSize maxPageTableEntries = 0;
    for (uint32_t mip = 0; mip <= TerrainLayerPageManager::MaxMipLevel; ++mip)
    {
        maxPageTableEntries += layerPageManager.VirtualPageTableSize(mip);
    }
    slotBytes = AlignTerrainUploadOffset(maxPageTableEntries * 16);

    buffers.reserve(slotCount);
    memories.reserve(slotCount);
    mapped.reserve(slotCount);
    try
    {
        for (uint32_t slot = 0; slot < slotCount; ++slot)
        {
            const std::pair<vk::Buffer, vk::DeviceMemory> created =
                backend.CreateBuffer(
                    slotBytes,
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "TerrainLayerUploadRing" + std::to_string(slot));
            buffers.push_back(created.first);
            memories.push_back(created.second);
            mapped.push_back(backend.MapMemory(created.second, slotBytes));
        }
    }
    catch (...)
    {
        for (size_t slot = 0; slot < buffers.size(); ++slot)
        {
            if (slot < memories.size() && slot < mapped.size() &&
                mapped[slot] != nullptr)
                backend.UnmapMemory(memories[slot]);
            backend.DestroyBuffer(
                buffers[slot],
                slot < memories.size() ? memories[slot] : vk::DeviceMemory{});
        }
        buffers.clear();
        memories.clear();
        mapped.clear();
        throw;
    }
}

TerrainLayerUploadGpuRing::~TerrainLayerUploadGpuRing()
{
    if (rendererBackend == nullptr)
        return;
    for (size_t slot = 0; slot < buffers.size(); ++slot)
    {
        if (mapped[slot] != nullptr)
            rendererBackend->UnmapMemory(memories[slot]);
        rendererBackend->DestroyBuffer(buffers[slot], memories[slot]);
    }
}

bool TerrainLayerUploadGpuRing::Fits(
    const TerrainLayerUploadBatch& batch) const noexcept
{
    if (batch.uploads.size() > maxLayerPages)
        return false;
    return static_cast<vk::DeviceSize>(batch.pageTableUpdates.size()) * 16 <=
        slotBytes;
}

vk::DeviceSize TerrainLayerUploadGpuRing::Write(
    uint32_t frameIndex,
    const TerrainLayerUploadBatch& batch)
{
    if (frameIndex >= slotCount)
        throw std::out_of_range(
            "TerrainLayerUploadGpuRing frame index is out of range");
    if (!Fits(batch))
        throw std::runtime_error(
            "TerrainLayerUploadGpuRing batch exceeds the slot budget");
    uint8_t* base = static_cast<uint8_t*>(mapped[frameIndex]);
    for (size_t index = 0; index < batch.pageTableUpdates.size(); ++index)
    {
        std::memcpy(
            base + index * 16,
            batch.pageTableUpdates[index].entry.data(),
            16);
    }
    return 0;
}

vk::Buffer TerrainLayerUploadGpuRing::Buffer(uint32_t frameIndex) const
{
    if (frameIndex >= slotCount)
        throw std::out_of_range(
            "TerrainLayerUploadGpuRing frame index is out of range");
    return buffers[frameIndex];
}

} // namespace VL