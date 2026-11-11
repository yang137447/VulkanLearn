#include "render/resource/terrain/terrainLayerPageUpload.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "commonFunction.h"
#include "texture.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"

namespace VL
{
std::vector<vk::BufferImageCopy> BuildTerrainLayerPageTableCopyRegions(
    const TerrainLayerUploadBatch& batch,
    const TerrainLayerPageManager& layerPageManager,
    uint32_t mip,
    vk::DeviceSize stagingBase)
{
    const TerrainLayerPageManager::PageTableLayout layout =
        layerPageManager.PageTableLayoutForMip(mip);
    std::vector<vk::BufferImageCopy> regions;
    for (size_t index = 0; index < batch.pageTableUpdates.size(); ++index)
    {
        const TerrainLayerPageManager::PageTableUpdate& update =
            batch.pageTableUpdates[index];
        if (update.key.mip != mip)
            continue;
        vk::BufferImageCopy region;
        region
            .setBufferOffset(
                stagingBase + static_cast<vk::DeviceSize>(index) * 16)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setImageOffset(vk::Offset3D{
                static_cast<int32_t>(update.virtualPageIndex % layout.width),
                static_cast<int32_t>(update.virtualPageIndex / layout.width),
                0 })
            .setImageExtent(vk::Extent3D{1, 1, 1});
        regions.push_back(region);
    }
    return regions;
}

// 源层页 -> atlas 物理页。源/目标页都是 128×128，块格式也满足 4×4 对齐；
// 源偏移由页坐标决定，目标偏移由物理页的 (slice,tileX,tileY) 决定。
std::vector<vk::ImageCopy> BuildTerrainLayerAtlasImageCopies(
    const TerrainLayerUploadBatch& batch,
    const TerrainLayerPageManager& layerPageManager,
    const TerrainSourceSetRecord& sources,
    uint32_t sourceSet,
    uint32_t sourceWidth,
    uint32_t sourceHeight)
{
    std::vector<vk::ImageCopy> regions;
    regions.reserve(batch.uploads.size());
    for (const TerrainLayerPageManager::PageUpload& upload : batch.uploads)
    {
        // 多 sourceSet：只有属于本组的层页才从这个源数组 copy。
        if (layerPageManager.LayerSourceSet(upload.key.layerId) != sourceSet)
            continue;
        // catalog 的 sourceResolution 可能与实际纹理尺寸不一致（现有验证资产
        // 就是 32² 图 + 默认 1K catalog）。L2 按源 mip 的真实尺寸裁剪 extent：
        // 源小于一页时只复制左上角，其余像素保持 atlas clear 的零值。
        const uint32_t mipWidth =
            std::max(1u, sourceWidth >> upload.key.mip);
        const uint32_t mipHeight =
            std::max(1u, sourceHeight >> upload.key.mip);
        const uint64_t sourceX =
            static_cast<uint64_t>(upload.key.pageX) *
            TerrainLayerPageManager::PageResolution;
        const uint64_t sourceY =
            static_cast<uint64_t>(upload.key.pageY) *
            TerrainLayerPageManager::PageResolution;
        if (sourceX >= mipWidth || sourceY >= mipHeight)
        {
            throw std::runtime_error(
                "Terrain layer page is outside the source mip extent");
        }
        const uint32_t copyWidth = static_cast<uint32_t>(std::min<uint64_t>(
            TerrainLayerPageManager::PageResolution,
            mipWidth - sourceX));
        const uint32_t copyHeight = static_cast<uint32_t>(std::min<uint64_t>(
            TerrainLayerPageManager::PageResolution,
            mipHeight - sourceY));
        const TerrainLayerPageManager::PhysicalPageAddress address =
            TerrainLayerPageManager::DecodePhysicalPage(upload.physicalPage);
        // 全局层号不等于源数组 slice：源集合内部仍按 MI 的局部层顺序。
        uint32_t sourceLayer = 0;
        if (!sources.TryGetLayerSlice(upload.key.layerId, sourceLayer))
        {
            throw std::runtime_error(
                "Terrain layer is not registered in its source set: layerId=" +
                std::to_string(upload.key.layerId));
        }
        vk::ImageCopy region;
        region
            .setSrcSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(upload.key.mip)
                .setBaseArrayLayer(sourceLayer)
                .setLayerCount(1))
            .setDstSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(address.slice)
                .setLayerCount(1))
            .setSrcOffset(vk::Offset3D{
                static_cast<int32_t>(sourceX),
                static_cast<int32_t>(sourceY),
                0 })
            .setDstOffset(vk::Offset3D{
                static_cast<int32_t>(
                    address.tileX *
                    TerrainLayerPageManager::PageResolution),
                static_cast<int32_t>(
                    address.tileY *
                    TerrainLayerPageManager::PageResolution),
                0 })
            .setExtent(vk::Extent3D{
                copyWidth,
                copyHeight,
                1 });
        regions.push_back(region);
    }
    return regions;
}

void RecordTerrainLayerAtlasCopies(
    vk::CommandBuffer commandBuffer,
    Texture& atlas,
    Texture& source,
    const std::vector<vk::ImageCopy>& regions)
{
    if (regions.empty())
        return;
    // 源纹理当前作为材质采样源处于 ShaderReadOnly；切页期间临时改为
    // TransferSrc，完成后必须恢复，否则后续 scene pass 读到非法 layout。
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        source.getImage(),
        source.getMipLevels(),
        source.GetFormat(),
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eTransferSrcOptimal,
        0,
        source.GetArrayLayers());
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        atlas.getImage(),
        atlas.getMipLevels(),
        atlas.GetFormat(),
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eTransferDstOptimal,
        0,
        atlas.GetArrayLayers());
    CommonFunction::RecordCopyImageRegions(
        commandBuffer,
        source.getImage(),
        atlas.getImage(),
        regions);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        atlas.getImage(),
        atlas.getMipLevels(),
        atlas.GetFormat(),
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        0,
        atlas.GetArrayLayers());
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        source.getImage(),
        source.getMipLevels(),
        source.GetFormat(),
        vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        0,
        source.GetArrayLayers());
}

// 空 atlas 的零初始化：非压缩格式用 clearColor；BC 禁止 clearColor，改用

void RecordTerrainLayerUploads(
    vk::CommandBuffer commandBuffer,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceSets& sources,
    const TerrainLayerUploadBatch& batch,
    vk::Buffer stagingBuffer,
    vk::DeviceSize pageTableStagingOffset)
{
    // 页表：按 mip 分组；每张表一次 layout 往返，所有 texel 合并成一次 copy。
    for (uint32_t mip = 0; mip <= TerrainLayerPageManager::MaxMipLevel; ++mip)
    {
        const std::vector<vk::BufferImageCopy> regions =
            BuildTerrainLayerPageTableCopyRegions(
                batch,
                layerPageManager,
                mip,
                pageTableStagingOffset);
        if (regions.empty())
            continue;
        Texture& pageTable = *layerResources.pageTables[mip];
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            pageTable.getImage(),
            pageTable.getMipLevels(),
            pageTable.GetFormat(),
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferDstOptimal);
        CommonFunction::RecordCopyBufferToImageRegions(
            commandBuffer,
            stagingBuffer,
            pageTable.getImage(),
            regions);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            pageTable.getImage(),
            pageTable.getMipLevels(),
            pageTable.GetFormat(),
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    if (batch.uploads.empty())
        return;
    if (sources.empty())
        throw std::runtime_error("Terrain layer source textures are not bound");
    // S2：本批可能来自多个 sourceSet；按 set 分组，各自从自己的源数组
    // copy 到同一组 atlas。三张源图允许尺寸/格式不同（BC7 albedo +
    // RGBA8 normal/pbr），因此每个 atlas 用自己的源尺寸生成 region。
    for (const auto& entry : sources)
    {
        const TerrainSourceSetRecord& record = entry.second;
        if (!record.HasTextures())
            throw std::runtime_error(
                "Terrain layer source set is incomplete: set=" +
                std::to_string(record.sourceSet));
        RecordTerrainLayerAtlasCopies(
            commandBuffer,
            *layerResources.albedoAtlas,
            *record.albedo,
            BuildTerrainLayerAtlasImageCopies(
                batch,
                layerPageManager,
                record,
                record.sourceSet,
                record.albedoWidth,
                record.albedoHeight));
        RecordTerrainLayerAtlasCopies(
            commandBuffer,
            *layerResources.normalAtlas,
            *record.normal,
            BuildTerrainLayerAtlasImageCopies(
                batch,
                layerPageManager,
                record,
                record.sourceSet,
                record.normalWidth,
                record.normalHeight));
        RecordTerrainLayerAtlasCopies(
            commandBuffer,
            *layerResources.pbrAtlas,
            *record.pbr,
            BuildTerrainLayerAtlasImageCopies(
                batch,
                layerPageManager,
                record,
                record.sourceSet,
                record.pbrWidth,
                record.pbrHeight));
    }
}

void RecordTerrainLayerPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceSets& sources,
    const TerrainLayerUploadBatch& batch,
    vk::Buffer stagingBuffer,
    vk::DeviceSize pageTableStagingOffset)
{
    if (batch.Empty())
        return;
    if (!layerResources.atlasesCreated)
        throw std::runtime_error("Terrain layer atlas is not initialized");
    // 逐帧路径与加载期共用同一套 region 生成/录制逻辑；区别只在于
    // staging 来自 ring 槽位、command buffer 属于主帧。
    RecordTerrainLayerUploads(
        commandBuffer,
        layerPageManager,
        layerResources,
        sources,
        batch,
        stagingBuffer,
        pageTableStagingOffset);
}

void TerrainLayerUploadTransaction::Begin(
    std::shared_ptr<TerrainStreamingState> streamingState,
    TerrainLayerUploadBatch uploadBatch)
{
    // 与覆盖页上传相同：生产路径同一时刻只允许一个 in-flight 层页批次。
    if (active)
        throw std::logic_error(
            "TerrainLayerUploadTransaction is already active");
    streaming = std::move(streamingState);
    batch = std::move(uploadBatch);
    uploadedPageCount = static_cast<uint32_t>(batch.uploads.size());
    active = !batch.Empty();
}

void TerrainLayerUploadTransaction::Commit()
{
    if (!active)
        return;
    streaming->layerPageManager.CommitUploadBatch(batch);
    // 统计只在提交成功后累计；失败/未提交路径不虚报带宽。
    streaming->layerUploadedPagesTotal += uploadedPageCount;
    const TerrainLayerGpuResources& resources = streaming->layerResources;
    const uint64_t pageBytes = EstimateTextureBytes(
        resources.albedoFormat,
        TerrainLayerPageManager::PageResolution,
        TerrainLayerPageManager::PageResolution,
        1) +
        EstimateTextureBytes(
            resources.normalFormat,
            TerrainLayerPageManager::PageResolution,
            TerrainLayerPageManager::PageResolution,
            1) +
        EstimateTextureBytes(
            resources.pbrFormat,
            TerrainLayerPageManager::PageResolution,
            TerrainLayerPageManager::PageResolution,
            1);
    streaming->layerUploadedBytesTotal += uploadedPageCount * pageBytes;
    streaming->lastLayerUploadedPages = uploadedPageCount;
    active = false;
    batch = TerrainLayerUploadBatch{};
}

TerrainLayerUploadTransaction::~TerrainLayerUploadTransaction()
{
    if (!active)
        return;
    // 录制没走到 SubmitFrame 或提交抛异常：不清 dirty，只解除本批 pin。
    streaming->layerPageManager.AbortUploadBatch(batch);
}

void RequestInitialTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight)
{
    TerrainLayerPageManager& pageManager = streamingState.layerPageManager;
    if (streamingState.layerSources.empty())
        return;
    // 请求前先建立 epoch 上下文；加载期没有提交过帧时 completedEpoch 为 0。
    pageManager.SetGpuEpochContext(completedEpoch, framesInFlight);
    const uint32_t layerCount =
        static_cast<uint32_t>(streamingState.layerDefinitions.size());
    for (uint32_t layerId = 0; layerId < layerCount; ++layerId)
    {
        // L2 只取每层尾页（page 0,0）作为映射验证集合；不请求 85/341 页全链。
        // 源纹理可能只有部分 mip（例如 BC7 4 mip），尾页必须夹到该层所在
        // sourceSet 的可用范围。
        const uint32_t sourceMipCount = TerrainSourceMipLevelsForLayer(
            streamingState.layerSources,
            pageManager.LayerSourceSet(layerId));
        if (sourceMipCount == 0)
            continue;
        const uint32_t mipCount =
            TerrainLayerMipCount(pageManager.LayerSourceResolution(layerId));
        const uint32_t mip = std::min(
            mipCount == 0 ? 0u : mipCount - 1,
            sourceMipCount - 1);
        TerrainLayerPageKey key;
        key.layerId = layerId;
        key.mip = mip;
        key.pageX = 0;
        key.pageY = 0;
        pageManager.RequestPageAtEpoch(key, 0, 0, completedEpoch);
    }
}

void RequestAllTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight)
{
    // L3 的 VT 验证路径没有 feedback 驱动，必须在加载期把每层每个 mip 的
    // 页都请求并上传，否则缺失页只能显示 catalog avgColor。L4 会用
    // feedback 取代这个全量预取。
    TerrainLayerPageManager& pageManager = streamingState.layerPageManager;
    if (streamingState.layerSources.empty())
        return;

    const uint32_t layerCount =
        static_cast<uint32_t>(streamingState.layerDefinitions.size());
    uint32_t totalPages = 0;
    for (uint32_t layerId = 0; layerId < layerCount; ++layerId)
    {
        const uint32_t sourceMipCount = TerrainSourceMipLevelsForLayer(
            streamingState.layerSources,
            pageManager.LayerSourceSet(layerId));
        const uint32_t mipCount = std::min(
            TerrainLayerMipCount(pageManager.LayerSourceResolution(layerId)),
            sourceMipCount);
        for (uint32_t mip = 0; mip < mipCount; ++mip)
            totalPages += pageManager.PagesPerMip(layerId, mip);
    }
    // 物理页池包含一个保留的 fallback 槽位；容量不能超过可用槽位。
    const uint32_t capacity = std::min(
        pageManager.UsablePhysicalPageCount(),
        std::max(totalPages, TerrainLayerPageManager::DefaultResidentCapacity));
    pageManager.SetResidentCapacity(capacity);
    pageManager.SetGpuEpochContext(completedEpoch, framesInFlight);

    uint32_t requestedPages = 0;
    for (uint32_t layerId = 0; layerId < layerCount; ++layerId)
    {
        const uint32_t sourceMipCount = TerrainSourceMipLevelsForLayer(
            streamingState.layerSources,
            pageManager.LayerSourceSet(layerId));
        const uint32_t mipCount = std::min(
            TerrainLayerMipCount(pageManager.LayerSourceResolution(layerId)),
            sourceMipCount);
        for (uint32_t mip = 0; mip < mipCount; ++mip)
        {
            const uint32_t side = pageManager.PagesPerSide(layerId, mip);
            for (uint32_t pageY = 0; pageY < side; ++pageY)
            {
                for (uint32_t pageX = 0; pageX < side; ++pageX)
                {
                    TerrainLayerPageKey key;
                    key.layerId = layerId;
                    key.mip = mip;
                    key.pageX = pageX;
                    key.pageY = pageY;
                    if (pageManager.RequestPageAtEpoch(
                            key,
                            0,
                            0,
                            completedEpoch))
                    {
                        ++requestedPages;
                    }
                }
            }
        }
    }
    if (requestedPages < totalPages)
    {
        std::cout
            << "Warning: terrain layer VT preload resident "
            << requestedPages << "/" << totalPages
            << " pages; missing pages use catalog avgColor" << std::endl;
    }
}

void RequestTerrainLayerPreload(TerrainStreamingState& streamingState)
{
    // 对照路径：一次性请求所有层页，用来和 feedback 驱动的驻留做画面对比。
    // 这里沿用加载期入口（不做 GPU 安全窗检查），因为命令在渲染线程帧首
    // 消费，此刻没有进行中的层页请求；像素仍由逐帧 ring 分批上传。
    RequestAllTerrainLayerPages(
        streamingState,
        streamingState.layerPageManager.LastSubmittedEpoch(),
        streamingState.layerPageManager.FramesInFlight());
}

size_t ApplyTerrainLayerPageUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceSets& sources)
{
    TerrainLayerUploadBatch batch = layerPageManager.BuildUploadBatch();
    if (batch.Empty())
        return 0;
    if (!layerResources.atlasesCreated ||
        !layerResources.albedoAtlas ||
        !layerResources.normalAtlas ||
        !layerResources.pbrAtlas)
    {
        layerPageManager.AbortUploadBatch(batch);
        throw std::runtime_error("Terrain layer atlas is not initialized");
    }
    if (sources.empty())
    {
        layerPageManager.AbortUploadBatch(batch);
        throw std::runtime_error(
            "Terrain layer source texture extent is unknown");
    }
    for (const auto& entry : sources)
    {
        const TerrainSourceSetRecord& record = entry.second;
        if (!record.HasTextures() ||
            record.albedoWidth == 0 || record.albedoHeight == 0 ||
            record.normalWidth == 0 || record.normalHeight == 0 ||
            record.pbrWidth == 0 || record.pbrHeight == 0)
        {
            layerPageManager.AbortUploadBatch(batch);
            throw std::runtime_error(
                "Terrain layer source set has no usable texture extent: set=" +
                std::to_string(record.sourceSet));
        }
    }
    for (uint32_t mip = 0; mip <= TerrainLayerPageManager::MaxMipLevel; ++mip)
    {
        if (!layerResources.pageTables[mip])
        {
            layerPageManager.AbortUploadBatch(batch);
            throw std::runtime_error(
                "Terrain layer page table is not initialized for mip " +
                std::to_string(mip));
        }
    }
    // 源 mip 不足时不能把该页像素写进 atlas；初始请求已经夹过，生产路径
    // 若出现越界应显式失败，而不是留下一个指向空内容的页表项。
    for (const TerrainLayerPageManager::PageUpload& upload : batch.uploads)
    {
        const TerrainSourceSetRecord* record = FindTerrainSourceSet(
            sources,
            layerPageManager.LayerSourceSet(upload.key.layerId));
        if (record == nullptr)
        {
            layerPageManager.AbortUploadBatch(batch);
            throw std::runtime_error(
                "Terrain layer has no bound source set: layerId=" +
                std::to_string(upload.key.layerId));
        }
        uint32_t sourceLayer = 0;
        if (!record->TryGetLayerSlice(upload.key.layerId, sourceLayer) ||
            sourceLayer >= record->arrayLayers)
        {
            layerPageManager.AbortUploadBatch(batch);
            throw std::runtime_error(
                "Terrain layer id is not present in the source texture array: "
                "layerId=" + std::to_string(upload.key.layerId) +
                " sourceSet=" + std::to_string(record->sourceSet));
        }
        // 三张源图的 mip 数可能不同，取最小值保证三张都能 copy。
        if (upload.key.mip >= record->MinMipLevels())
        {
            layerPageManager.AbortUploadBatch(batch);
            throw std::runtime_error(
                "Terrain layer page mip exceeds the source mip chain");
        }
    }

    const vk::DeviceSize stagingBytes =
        static_cast<vk::DeviceSize>(batch.pageTableUpdates.size()) * 16;
    vk::Buffer stagingBuffer = nullptr;
    vk::DeviceMemory stagingMemory = nullptr;
    try
    {
        if (stagingBytes != 0)
        {
            const std::pair<vk::Buffer, vk::DeviceMemory> created =
                rendererBackend.CreateBuffer(
                    stagingBytes,
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "TerrainLayerPageTableStaging");
            stagingBuffer = created.first;
            stagingMemory = created.second;
            void* mapped =
                rendererBackend.MapMemory(stagingMemory, stagingBytes);
            for (size_t index = 0; index < batch.pageTableUpdates.size();
                 ++index)
            {
                std::memcpy(
                    static_cast<uint8_t*>(mapped) + index * 16,
                    batch.pageTableUpdates[index].entry.data(),
                    16);
            }
            rendererBackend.UnmapMemory(stagingMemory);
        }

        vk::CommandBuffer commandBuffer =
            rendererBackend.BeginSingleTimeCommands();
        try
        {
            RecordTerrainLayerUploads(
                commandBuffer,
                layerPageManager,
                layerResources,
                sources,
                batch,
                stagingBuffer,
                0);
        }
        catch (...)
        {
            rendererBackend.AbortSingleTimeCommands(commandBuffer);
            throw;
        }
        rendererBackend.EndSingleTimeCommands(commandBuffer);
        layerPageManager.CommitUploadBatch(batch);
    }
    catch (...)
    {
        layerPageManager.AbortUploadBatch(batch);
        if (stagingBuffer)
            rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);
        throw;
    }
    if (stagingBuffer)
        rendererBackend.DestroyBuffer(stagingBuffer, stagingMemory);
    return batch.uploads.size();
}

void ApplyTerrainLayerStreamingUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState)
{
    if (!streamingState.HasPendingUpload())
        return;
    const uint64_t uploadingRevision = streamingState.pendingUploadRevision;
    const size_t uploadedPages = ApplyTerrainLayerPageUploads(
        rendererBackend,
        streamingState.layerPageManager,
        streamingState.layerResources,
        streamingState.layerSources);
    streamingState.layerUploadedPagesTotal += uploadedPages;
    // 按源格式估算本次写入的字节数：一页三张图（albedo/normal/pbr）。
    const TerrainLayerGpuResources& resources = streamingState.layerResources;
    const uint64_t pageBytes = EstimateTextureBytes(
        resources.albedoFormat,
        TerrainLayerPageManager::PageResolution,
        TerrainLayerPageManager::PageResolution,
        1) +
        EstimateTextureBytes(
            resources.normalFormat,
            TerrainLayerPageManager::PageResolution,
            TerrainLayerPageManager::PageResolution,
            1) +
        EstimateTextureBytes(
            resources.pbrFormat,
            TerrainLayerPageManager::PageResolution,
            TerrainLayerPageManager::PageResolution,
            1);
    streamingState.layerUploadedBytesTotal += uploadedPages * pageBytes;
    streamingState.MarkUploaded(uploadingRevision);
}
} // namespace VL