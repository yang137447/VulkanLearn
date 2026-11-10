#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainPageManager.h"

namespace VL
{
class RendererBackendVulkan;
// 一个 staging buffer 内三段数据的 base offset；RecordTerrainPageUploads 用它们
// 构造批量 vk::BufferImageCopy，三段都由调用方按 16 字节对齐打包。
struct TerrainUploadStagingOffsets
{
    vk::DeviceSize pageTable = 0;
    vk::DeviceSize layerIds = 0;
    vk::DeviceSize layerWeights = 0;
};

// 每帧一条 host-visible/coherent staging buffer，槽数等于 frame fence 数。
// 同一槽位只有在 BeginFrame 等过对应 fence 后才会被 CPU 重写，避免覆盖
// GPU 尚未读取的上传内容；不创建 per-frame 临时 buffer，也不 waitIdle。
class TerrainUploadGpuRing
{
public:
    TerrainUploadGpuRing(
        RendererBackendVulkan& rendererBackend,
        uint32_t maxAtlasPages);
    ~TerrainUploadGpuRing();

    TerrainUploadGpuRing(const TerrainUploadGpuRing&) = delete;
    TerrainUploadGpuRing& operator=(const TerrainUploadGpuRing&) = delete;

    uint32_t SlotCount() const noexcept { return slotCount; }
    uint32_t MaxAtlasPages() const noexcept { return maxAtlasPages; }
    bool Fits(const TerrainUploadBatch& batch) const noexcept;

    // 把一整批页表项/ID/权重打包进该 frameIndex 的槽位，返回三段 base offset。
    // 调用方保证该槽位 fence 已完成（BeginFrame 已等待）。
    TerrainUploadStagingOffsets Write(
        uint32_t frameIndex,
        const TerrainUploadBatch& batch);
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

// L4a：层页每帧上传 ring。层页上传只需要页表 texel 走 staging；atlas 像素
// 是 GPU→GPU copy，不占 staging。槽数等于 frame fence 数，常驻映射。
class TerrainLayerUploadGpuRing
{
public:
    TerrainLayerUploadGpuRing(
        RendererBackendVulkan& rendererBackend,
        const TerrainLayerPageManager& layerPageManager,
        uint32_t maxLayerPagesPerFrame);
    ~TerrainLayerUploadGpuRing();

    TerrainLayerUploadGpuRing(const TerrainLayerUploadGpuRing&) = delete;
    TerrainLayerUploadGpuRing& operator=(const TerrainLayerUploadGpuRing&) =
        delete;

    uint32_t SlotCount() const noexcept { return slotCount; }
    uint32_t MaxLayerPages() const noexcept { return maxLayerPages; }
    bool Fits(const TerrainLayerUploadBatch& batch) const noexcept;

    // 把本批全部页表项（16 B/项）打包进该 frameIndex 的槽位，返回 base offset。
    // 调用方保证该槽的 fence 已在 BeginFrame 等待过。
    vk::DeviceSize Write(
        uint32_t frameIndex,
        const TerrainLayerUploadBatch& batch);
    vk::Buffer Buffer(uint32_t frameIndex) const;

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    uint32_t slotCount = 0;
    uint32_t maxLayerPages = 0;
    vk::DeviceSize slotBytes = 0;
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceMemory> memories;
    std::vector<void*> mapped;
};
// staging buffer 内各段按 16 字节对齐：RGBA8 只要求 texel block size（4 字节），
// 但对齐到 16 可以让批量 copy 的 bufferOffset 在所有桌面实现上都保持高效路径。
vk::DeviceSize AlignTerrainUploadOffset(vk::DeviceSize value);

} // namespace VL