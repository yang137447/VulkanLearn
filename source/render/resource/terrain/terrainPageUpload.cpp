#include "render/resource/terrain/terrainPageUpload.h"

#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "commonFunction.h"
#include "texture.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"

namespace VL
{
std::vector<vk::BufferImageCopy> BuildTerrainPageTableCopyRegions(
    const std::vector<TerrainPageManager::PageTableUpload>& uploads,
    vk::DeviceSize bufferBase)
{
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(uploads.size());
    for (size_t index = 0; index < uploads.size(); ++index)
    {
        const TerrainPageManager::PageTableUpload& upload = uploads[index];
        // 页表项每个 texel 4 字节，按收集顺序紧密排列。
        vk::BufferImageCopy region;
        region
            .setBufferOffset(bufferBase + static_cast<vk::DeviceSize>(index) * 4)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setImageOffset(vk::Offset3D{
                static_cast<int32_t>(upload.virtualPageX),
                static_cast<int32_t>(upload.virtualPageY),
                0 })
            .setImageExtent(vk::Extent3D{1, 1, 1});
        regions.push_back(region);
    }
    return regions;
}

std::vector<vk::BufferImageCopy> BuildTerrainAtlasCopyRegions(
    const std::vector<TerrainPageManager::AtlasPageUpload>& uploads,
    vk::DeviceSize bufferBase)
{
    constexpr vk::DeviceSize PageBytes =
        static_cast<vk::DeviceSize>(TerrainPageManager::PageResolution) *
        TerrainPageManager::PageResolution * 4;
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(uploads.size());
    for (size_t index = 0; index < uploads.size(); ++index)
    {
        const TerrainPageManager::AtlasPageUpload& upload = uploads[index];
        vk::BufferImageCopy region;
        region
            .setBufferOffset(bufferBase + static_cast<vk::DeviceSize>(index) * PageBytes)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setImageOffset(vk::Offset3D{
                static_cast<int32_t>(
                    upload.physicalPageX * TerrainPageManager::PageResolution),
                static_cast<int32_t>(
                    upload.physicalPageY * TerrainPageManager::PageResolution),
                0 })
            .setImageExtent(vk::Extent3D{
                TerrainPageManager::PageResolution,
                TerrainPageManager::PageResolution,
                1 });
        regions.push_back(region);
    }
    return regions;
}

// 把一张 image 的批量 copy 录制进同一个 command buffer：进入前 image 已发布为
// ShaderReadOnly，录制 ShaderReadOnly -> TransferDst、copy、再转回 ShaderReadOnly。
// 三段共用主帧/单次提交，GPU 看到的是完整的一批页内容，不会出现半更新状态。
void RecordTerrainImageUploads(
    vk::CommandBuffer commandBuffer,
    Texture& texture,
    vk::Buffer stagingBuffer,
    const std::vector<vk::BufferImageCopy>& regions)
{
    if (regions.empty()) return;
    const vk::Image image = texture.getImage();
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image,
        1,
        texture.GetFormat(),
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eTransferDstOptimal);
    CommonFunction::RecordCopyBufferToImageRegions(
        commandBuffer,
        stagingBuffer,
        image,
        regions);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        image,
        1,
        texture.GetFormat(),
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal);
}

// 页表 texel 的 CPU->GPU 描述：每个 update 占 16 字节，按虚拟页号映射到

void RecordTerrainPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainPageGpuResources& pageResources,
    const TerrainUploadBatch& batch,
    vk::Buffer stagingBuffer,
    const TerrainUploadStagingOffsets& stagingOffsets)
{
    if (batch.Empty())
        return;
    if (!pageResources.pageTable || !pageResources.layerIds || !pageResources.layerWeights)
        throw std::runtime_error("Terrain page resources are not initialized");

    // 三张 image 各自 transition + 批量 copy，但都录制在同一个 command buffer；
    // 调用方可以随主帧一次提交，也可以在加载期一次 submit + wait。
    RecordTerrainImageUploads(
        commandBuffer,
        *pageResources.pageTable,
        stagingBuffer,
        BuildTerrainPageTableCopyRegions(
            batch.pageTableUploads,
            stagingOffsets.pageTable));
    RecordTerrainImageUploads(
        commandBuffer,
        *pageResources.layerIds,
        stagingBuffer,
        BuildTerrainAtlasCopyRegions(
            batch.atlasUploads,
            stagingOffsets.layerIds));
    RecordTerrainImageUploads(
        commandBuffer,
        *pageResources.layerWeights,
        stagingBuffer,
        BuildTerrainAtlasCopyRegions(
            batch.atlasUploads,
            stagingOffsets.layerWeights));
}

void TerrainUploadTransaction::Begin(
    std::shared_ptr<TerrainStreamingState> streamingState,
    TerrainUploadBatch uploadBatch)
{
    // 生产路径同一时刻只允许一个 in-flight 上传批次；重叠批次会让
    // Commit/Abort 与 staging 槽位语义变得不可判定。
    if (active)
        throw std::logic_error(
            "TerrainUploadTransaction is already active");
    streaming = std::move(streamingState);
    batch = std::move(uploadBatch);
    active = !batch.Empty();
}

void TerrainUploadTransaction::Commit()
{
    if (!active)
        return;
    streaming->pageManager.CommitUploadBatch(batch);
    active = false;
    batch = TerrainUploadBatch{};
}

TerrainUploadTransaction::~TerrainUploadTransaction()
{
    if (!active)
        return;
    // 录制没走到 SubmitFrame 或提交抛异常：不清 dirty，只解除本批 pin。
    streaming->pageManager.AbortUploadBatch(batch);
}

size_t ApplyTerrainPageUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainPageManager& pageManager,
    TerrainPageGpuResources& pageResources)
{
    TerrainUploadBatch batch = pageManager.BuildUploadBatch();
    if (batch.Empty())
        return 0;
    if (!pageResources.pageTable || !pageResources.layerIds || !pageResources.layerWeights)
    {
        pageManager.AbortUploadBatch(batch);
        throw std::runtime_error("Terrain page resources are not initialized");
    }

    constexpr vk::DeviceSize PageBytes =
        static_cast<vk::DeviceSize>(TerrainPageManager::PageResolution) *
        TerrainPageManager::PageResolution * 4;
    // 一个 staging buffer 同时容纳本批页表项、ID atlas 页和权重 atlas 页；
    // 三段都按 16 字节对齐，批量 copy 只需要各自的 base offset。
    const vk::DeviceSize pageTableBytes = AlignTerrainUploadOffset(
        static_cast<vk::DeviceSize>(batch.pageTableUploads.size()) * 4);
    const vk::DeviceSize idsBase = pageTableBytes;
    const vk::DeviceSize idsBytes = AlignTerrainUploadOffset(
        static_cast<vk::DeviceSize>(batch.atlasUploads.size()) * PageBytes);
    const vk::DeviceSize weightsBase = idsBase + idsBytes;
    const vk::DeviceSize weightsBytes =
        static_cast<vk::DeviceSize>(batch.atlasUploads.size()) * PageBytes;
    const vk::DeviceSize totalBytes = weightsBase + weightsBytes;
    if (totalBytes == 0)
    {
        pageManager.AbortUploadBatch(batch);
        return 0;
    }

    // buffer 分配失败也要走 Abort：否则 BuildUploadBatch 的 pin 会永久留在页上。
    vk::Buffer stagingBuffer = nullptr;
    vk::DeviceMemory stagingMemory = nullptr;
    try
    {
        const std::pair<vk::Buffer, vk::DeviceMemory> created =
            rendererBackend.CreateBuffer(
            totalBytes,
            vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TerrainPageUploadStaging");
        stagingBuffer = created.first;
        stagingMemory = created.second;

        // 只 map 一次，把三类数据一次性打包进 staging，避免逐页 map/unmap。
        uint8_t* mapped = static_cast<uint8_t*>(
            rendererBackend.MapMemory(stagingMemory, totalBytes));
        for (size_t index = 0; index < batch.pageTableUploads.size(); ++index)
        {
            std::memcpy(
                mapped + static_cast<vk::DeviceSize>(index) * 4,
                batch.pageTableUploads[index].entry,
                4);
        }
        for (size_t index = 0; index < batch.atlasUploads.size(); ++index)
        {
            const vk::DeviceSize pageOffset =
                static_cast<vk::DeviceSize>(index) * PageBytes;
            std::memcpy(
                mapped + idsBase + pageOffset,
                batch.atlasUploads[index].ids.data(),
                PageBytes);
            std::memcpy(
                mapped + weightsBase + pageOffset,
                batch.atlasUploads[index].weights.data(),
                PageBytes);
        }
        rendererBackend.UnmapMemory(stagingMemory);

        TerrainUploadStagingOffsets stagingOffsets;
        stagingOffsets.pageTable = 0;
        stagingOffsets.layerIds = idsBase;
        stagingOffsets.layerWeights = weightsBase;

        // 三张 image 的 transition/copy 全部录制到同一个 command buffer，
        // 只做一次 submit + fence wait，而不是每张 image 各提交一次。
        vk::CommandBuffer commandBuffer = rendererBackend.BeginSingleTimeCommands();
        try
        {
            RecordTerrainPageUploads(
                commandBuffer,
                pageResources,
                batch,
                stagingBuffer,
                stagingOffsets);
        }
        catch (...)
        {
            // 尚未提交的 command buffer 必须释放；buffer 由外层 catch 统一销毁。
            rendererBackend.AbortSingleTimeCommands(commandBuffer);
            throw;
        }
        rendererBackend.EndSingleTimeCommands(commandBuffer);

        // GPU 读取完成后才清 dirty；中途失败保留 dirty 供下次整批重试。
        pageManager.CommitUploadBatch(batch);
    }
    catch (...)
    {
        pageManager.AbortUploadBatch(batch);
        if (stagingBuffer)
            rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);
        throw;
    }
    rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);
    return batch.atlasUploads.size();
}

void ApplyTerrainStreamingUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState)
{
    if (!streamingState.HasPendingUpload())
        return;
    // 先冻结本次版本；上传过程中若同帧又改脏，pendingUploadRevision 会前进，
    // 这里只确认已经写完的版本，剩余内容留给下一批。
    const uint64_t uploadingRevision = streamingState.pendingUploadRevision;
    const size_t uploadedPages = ApplyTerrainPageUploads(
        rendererBackend,
        streamingState.pageManager,
        streamingState.pageResources);
    streamingState.uploadedPagesTotal += uploadedPages;
    streamingState.MarkUploaded(uploadingRevision);
}
} // namespace VL