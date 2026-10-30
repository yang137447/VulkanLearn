#include "render/resource/rendererTerrainLoader.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "renderableObject.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainLayerTable.h"
#include "terrain/terrainLayerVT.h"
#include "terrain/terrainMeshBuilder.h"
#include "terrain/terrainWeightMap.h"
#include "texture.h"

namespace VL
{
namespace
{

Eigen::Matrix4f BuildModelMatrix(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    return translationMatrix *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
}

HostImage MakeRGBA8Image(
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& pixels)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA8_UNORM;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = width * image.GetPixelSize();
    image.data = pixels;
    return image;
}

bool IsBlockCompressedFormat(vk::Format format)
{
    // L0 只引入 BC5/BC7；BC1..BC7 的枚举值在 Vulkan 里连续，统一按块格式
    // 处理 clear/copy 的行跨度与字节估算。
    return format >= vk::Format::eBc1RgbUnormBlock &&
        format <= vk::Format::eBc7SrgbBlock;
}

uint64_t EstimateTextureBytes(
    vk::Format format,
    uint32_t width,
    uint32_t height,
    uint32_t arrayLayers)
{
    if (width == 0 || height == 0 || arrayLayers == 0)
        return 0;
    if (IsBlockCompressedFormat(format))
    {
        // 16 字节 / 4×4 block = 1 字节每 texel（BC5/BC7）。
        return static_cast<uint64_t>((width + 3) / 4) *
            ((height + 3) / 4) * 16ull * arrayLayers;
    }
    uint64_t texelBytes = 0;
    switch (format)
    {
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Srgb:
        texelBytes = 1;
        break;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        texelBytes = 4;
        break;
    case vk::Format::eR16G16B16A16Sfloat:
        texelBytes = 8;
        break;
    case vk::Format::eR32G32B32A32Sfloat:
        texelBytes = 16;
        break;
    default:
        // 未知格式不阻塞 L2；统计按 0 计，避免把猜测值当预算依据。
        texelBytes = 0;
        break;
    }
    return static_cast<uint64_t>(width) * height * texelBytes * arrayLayers;
}

// staging buffer 内各段按 16 字节对齐：RGBA8 只要求 texel block size（4 字节），
// 但对齐到 16 可以让批量 copy 的 bufferOffset 在所有桌面实现上都保持高效路径。
vk::DeviceSize AlignTerrainUploadOffset(vk::DeviceSize value)
{
    constexpr vk::DeviceSize Alignment = 16;
    return (value + Alignment - 1) / Alignment * Alignment;
}

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
// 该 mip 表的 (index % width, index / width)。无效项同样写入，只是四个通道为 0。
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
    uint32_t sourceWidth,
    uint32_t sourceHeight)
{
    std::vector<vk::ImageCopy> regions;
    regions.reserve(batch.uploads.size());
    for (const TerrainLayerPageManager::PageUpload& upload : batch.uploads)
    {
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
        vk::ImageCopy region;
        region
            .setSrcSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(upload.key.mip)
                .setBaseArrayLayer(upload.key.layerId)
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
// 一个 16 KiB 的零页 staging 反复 copy 到每个物理页，避免 64 MiB CPU 零图。
void RecordTerrainLayerAtlasClear(
    vk::CommandBuffer commandBuffer,
    TerrainLayerGpuResources& layerResources,
    vk::Buffer zeroPageBuffer)
{
    constexpr uint32_t PageResolution = TerrainLayerPageManager::PageResolution;
    constexpr uint32_t TilesPerSide = TerrainLayerPageManager::TilesPerAtlasSide;
    std::vector<vk::BufferImageCopy> compressedRegions;

    Texture* atlases[3] = {
        layerResources.albedoAtlas.get(),
        layerResources.normalAtlas.get(),
        layerResources.pbrAtlas.get()};
    for (Texture* atlas : atlases)
    {
        if (atlas == nullptr)
            throw std::runtime_error(
                "Terrain layer atlas is not initialized");
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            atlas->getImage(),
            atlas->getMipLevels(),
            atlas->GetFormat(),
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferDstOptimal,
            0,
            atlas->GetArrayLayers());
        if (IsBlockCompressedFormat(atlas->GetFormat()))
        {
            if (!zeroPageBuffer)
                throw std::runtime_error(
                    "Terrain layer compressed atlas requires a zero page buffer");
            compressedRegions.clear();
            for (uint32_t slice = 0; slice < layerResources.atlasSlices;
                 ++slice)
            {
                for (uint32_t tile = 0;
                     tile < TerrainLayerPageManager::PagesPerAtlasSlice;
                     ++tile)
                {
                    vk::BufferImageCopy region;
                    region
                        .setBufferOffset(0)
                        .setBufferRowLength(0)
                        .setBufferImageHeight(0)
                        .setImageSubresource(vk::ImageSubresourceLayers()
                            .setAspectMask(vk::ImageAspectFlagBits::eColor)
                            .setMipLevel(0)
                            .setBaseArrayLayer(slice)
                            .setLayerCount(1))
                        .setImageOffset(vk::Offset3D{
                            static_cast<int32_t>(
                                (tile % TilesPerSide) * PageResolution),
                            static_cast<int32_t>(
                                (tile / TilesPerSide) * PageResolution),
                            0 })
                        .setImageExtent(vk::Extent3D{
                            PageResolution, PageResolution, 1 });
                    compressedRegions.push_back(region);
                }
            }
            CommonFunction::RecordCopyBufferToImageRegions(
                commandBuffer,
                zeroPageBuffer,
                atlas->getImage(),
                compressedRegions);
        }
        else
        {
            vk::ClearColorValue clearValue;
            clearValue.setFloat32({0.0f, 0.0f, 0.0f, 0.0f});
            CommonFunction::RecordClearColorImage(
                commandBuffer,
                atlas->getImage(),
                vk::ImageLayout::eTransferDstOptimal,
                clearValue,
                0,
                atlas->GetArrayLayers());
        }
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            atlas->getImage(),
            atlas->getMipLevels(),
            atlas->GetFormat(),
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            0,
            atlas->GetArrayLayers());
    }
}

void RecordTerrainLayerUploads(
    vk::CommandBuffer commandBuffer,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceTextures& sources,
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
    if (!sources.albedo || !sources.normal || !sources.pbr)
        throw std::runtime_error("Terrain layer source textures are not bound");
    // 三张源图允许尺寸/格式不同（BC7 albedo + RGBA8 normal/pbr），因此
    // 每个 atlas 必须用自己的源尺寸生成 region，不能共用一份。
    RecordTerrainLayerAtlasCopies(
        commandBuffer,
        *layerResources.albedoAtlas,
        *sources.albedo,
        BuildTerrainLayerAtlasImageCopies(
            batch,
            sources.albedoWidth,
            sources.albedoHeight));
    RecordTerrainLayerAtlasCopies(
        commandBuffer,
        *layerResources.normalAtlas,
        *sources.normal,
        BuildTerrainLayerAtlasImageCopies(
            batch,
            sources.normalWidth,
            sources.normalHeight));
    RecordTerrainLayerAtlasCopies(
        commandBuffer,
        *layerResources.pbrAtlas,
        *sources.pbr,
        BuildTerrainLayerAtlasImageCopies(
            batch,
            sources.pbrWidth,
            sources.pbrHeight));
}

} // namespace

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

void RecordTerrainLayerPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceTextures& sources,
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

namespace
{

Texture::CreateDesc MakeTerrainDataTextureDesc(
    const char* debugName,
    vk::Filter filter)
{
    Texture::CreateDesc desc;
    desc.semantic = HostImage::TextureSemantic::Data;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.filter = filter;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = debugName;
    return desc;
}

} // namespace

std::shared_ptr<Texture> CreateTerrainPageTableGpuResource(
    RendererBackendVulkan& rendererBackend)
{
    const Texture::CreateDesc pageTableDesc =
        MakeTerrainDataTextureDesc("TerrainPageTable", vk::Filter::eNearest);
    std::vector<uint8_t> emptyPageTable(
        static_cast<size_t>(TerrainPageManager::PageTableSize) *
            TerrainPageManager::PageTableSize * 4,
        0);
    return std::make_shared<Texture>(
        rendererBackend,
        MakeRGBA8Image(
            TerrainPageManager::PageTableSize,
            TerrainPageManager::PageTableSize,
            emptyPageTable),
        pageTableDesc);
}

void CreateTerrainPageAtlasGpuResources(
    RendererBackendVulkan& rendererBackend,
    TerrainPageGpuResources& resources)
{
    Texture::CreateDesc idsDesc =
        MakeTerrainDataTextureDesc(
            "TerrainLayerIdsAtlas",
            vk::Filter::eNearest);
    std::vector<uint8_t> emptyAtlas(
        static_cast<size_t>(TerrainPageManager::AtlasResolution) *
            TerrainPageManager::AtlasResolution * 4,
        0);
    resources.layerIds = std::make_shared<Texture>(
        rendererBackend,
        MakeRGBA8Image(
            TerrainPageManager::AtlasResolution,
            TerrainPageManager::AtlasResolution,
            emptyAtlas),
        idsDesc);

    Texture::CreateDesc weightsDesc =
        MakeTerrainDataTextureDesc(
            "TerrainLayerWeightsAtlas",
            vk::Filter::eLinear);
    resources.layerWeights = std::make_shared<Texture>(
        rendererBackend,
        MakeRGBA8Image(
            TerrainPageManager::AtlasResolution,
            TerrainPageManager::AtlasResolution,
            emptyAtlas),
        weightsDesc);
}

TerrainPageGpuResources CreateTerrainPageGpuResources(
    RendererBackendVulkan& rendererBackend)
{
    TerrainPageGpuResources resources;
    resources.pageTable =
        CreateTerrainPageTableGpuResource(rendererBackend);
    CreateTerrainPageAtlasGpuResources(rendererBackend, resources);
    return resources;
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

TerrainLayerGpuResources CreateTerrainLayerGpuResources(
    RendererBackendVulkan& rendererBackend,
    const std::vector<TerrainLayerDesc>& layers,
    const TerrainLayerPageManager& layerPageManager)
{
    TerrainLayerGpuResources resources;
    Texture::CreateDesc desc;
    desc.semantic = HostImage::TextureSemantic::Data;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.filter = vk::Filter::eNearest;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = "TerrainLayerTable";
    resources.layerTable = std::make_shared<Texture>(
        rendererBackend,
        TerrainLayerTable::BuildImage(layers),
        desc);

    // 每 mip 一张 RGBA32F 页表；宽度上限 4096，因此 1K 层的 mip0
    // （16384 项）是 4096×4。没有该 mip 的层时创建 1×1 占位表。
    for (uint32_t mip = 0; mip <= TerrainLayerPageManager::MaxMipLevel; ++mip)
    {
        const TerrainLayerPageManager::PageTableLayout layout =
            layerPageManager.PageTableLayoutForMip(mip);
        HostImage image;
        image.width = layout.width;
        image.height = layout.height;
        image.channels = 4;
        image.format = HostImage::PixelFormat::RGBA32_FLOAT;
        image.semantic = HostImage::TextureSemantic::Data;
        image.rowStrideBytes = image.width * image.GetPixelSize();
        // 未映射项必须是全零（B=0 -> fallback），初始页表因此可直接清零。
        image.data.assign(
            static_cast<size_t>(image.rowStrideBytes) * image.height,
            0);
        Texture::CreateDesc pageTableDesc = desc;
        pageTableDesc.debugName =
            "TerrainLayerPageTable" + std::to_string(mip);
        resources.pageTables[mip] = std::make_shared<Texture>(
            rendererBackend,
            image,
            pageTableDesc);
    }
    return resources;
}

void CreateTerrainLayerAtlasGpuResources(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState)
{
    const TerrainLayerSourceTextures& sources = streamingState.layerSources;
    if (!sources.albedo || !sources.normal || !sources.pbr)
    {
        throw std::runtime_error(
            "Terrain layer atlas requires bound albedo/normal/pbr layer arrays");
    }

    TerrainLayerGpuResources& resources = streamingState.layerResources;
    resources.physicalPageCount =
        streamingState.layerPageManager.PhysicalPageCount();
    // 4096² / 128² = 1024 页 / slice；物理页号按 slice 平铺。
    resources.atlasSlices =
        (resources.physicalPageCount +
            TerrainLayerPageManager::PagesPerAtlasSlice - 1) /
        TerrainLayerPageManager::PagesPerAtlasSlice;
    if (resources.atlasSlices == 0)
    {
        throw std::runtime_error(
            "Terrain layer atlas requires at least one physical page");
    }

    Texture::EmptyArrayDesc atlasDesc;
    atlasDesc.width = TerrainPageManager::AtlasResolution;
    atlasDesc.height = TerrainPageManager::AtlasResolution;
    atlasDesc.arrayLayers = resources.atlasSlices;
    // 页内采样需要双线性；atlas 本身无 mip，mip 由页表选择。
    atlasDesc.filter = vk::Filter::eLinear;
    atlasDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;

    atlasDesc.format = sources.albedo->GetFormat();
    atlasDesc.debugName = "TerrainLayerAlbedoAtlas";
    resources.albedoAtlas =
        Texture::CreateEmptyArray(rendererBackend, atlasDesc);

    atlasDesc.format = sources.normal->GetFormat();
    atlasDesc.debugName = "TerrainLayerNormalAtlas";
    resources.normalAtlas =
        Texture::CreateEmptyArray(rendererBackend, atlasDesc);

    atlasDesc.format = sources.pbr->GetFormat();
    atlasDesc.debugName = "TerrainLayerPbrAtlas";
    resources.pbrAtlas =
        Texture::CreateEmptyArray(rendererBackend, atlasDesc);

    resources.albedoFormat = resources.albedoAtlas->GetFormat();
    resources.normalFormat = resources.normalAtlas->GetFormat();
    resources.pbrFormat = resources.pbrAtlas->GetFormat();
    // 统计以 mip0 的整张 atlas 估算；无 mip 是 L2 的固定语义。
    resources.albedoBytes = EstimateTextureBytes(
        resources.albedoFormat,
        atlasDesc.width,
        atlasDesc.height,
        resources.atlasSlices);
    resources.normalBytes = EstimateTextureBytes(
        resources.normalFormat,
        atlasDesc.width,
        atlasDesc.height,
        resources.atlasSlices);
    resources.pbrBytes = EstimateTextureBytes(
        resources.pbrFormat,
        atlasDesc.width,
        atlasDesc.height,
        resources.atlasSlices);
    // “perMap”保留为 albedo 尺寸的同义值，便于控制台展示单张图规模。
    resources.atlasBytesPerMap = resources.albedoBytes;
    resources.atlasBytesTotal =
        resources.albedoBytes + resources.normalBytes + resources.pbrBytes;

    // BC atlas 不能 clearColor；用一个 16 KiB 零页反复 copy。buffer 必须
    // 活到 command buffer submit 完成，因此由本函数持有到 End 之后。
    constexpr size_t ZeroPageBytes =
        static_cast<size_t>(TerrainLayerPageManager::PageResolution / 4) *
        (TerrainLayerPageManager::PageResolution / 4) * 16;
    const bool needsZeroPage =
        IsBlockCompressedFormat(resources.albedoFormat) ||
        IsBlockCompressedFormat(resources.normalFormat) ||
        IsBlockCompressedFormat(resources.pbrFormat);
    vk::Buffer zeroPageBuffer = nullptr;
    vk::DeviceMemory zeroPageMemory = nullptr;
    if (needsZeroPage)
    {
        const std::pair<vk::Buffer, vk::DeviceMemory> created =
            rendererBackend.CreateBuffer(
                ZeroPageBytes,
                vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "TerrainLayerAtlasZeroPage");
        zeroPageBuffer = created.first;
        zeroPageMemory = created.second;
        void* mapped =
            rendererBackend.MapMemory(zeroPageMemory, ZeroPageBytes);
        std::memset(mapped, 0, ZeroPageBytes);
        rendererBackend.UnmapMemory(zeroPageMemory);
    }

    // 零初始化：三张 atlas 的 transition/copy 录制进同一个 command buffer，
    // 加载期只提交一次；不使用 device.waitIdle。
    vk::CommandBuffer commandBuffer =
        rendererBackend.BeginSingleTimeCommands();
    try
    {
        RecordTerrainLayerAtlasClear(
            commandBuffer,
            resources,
            zeroPageBuffer);
    }
    catch (...)
    {
        rendererBackend.AbortSingleTimeCommands(commandBuffer);
        if (zeroPageBuffer)
            rendererBackend.DestroyBuffer(zeroPageBuffer, zeroPageMemory);
        throw;
    }
    rendererBackend.EndSingleTimeCommands(commandBuffer);
    if (zeroPageBuffer)
        rendererBackend.DestroyBuffer(zeroPageBuffer, zeroPageMemory);
    resources.atlasesCreated = true;
}

void RequestInitialTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight)
{
    TerrainLayerPageManager& pageManager = streamingState.layerPageManager;
    const TerrainLayerSourceTextures& sources = streamingState.layerSources;
    if (!sources.albedo || !sources.normal || !sources.pbr)
        return;
    // L2 只取每层尾页（page 0,0）作为映射验证集合；不请求 85/341 页全链。
    // 源纹理可能只有部分 mip（例如 BC7 4 mip），尾页必须夹到可用范围。
    const uint32_t sourceMipCount = std::min(
        {sources.albedoMipLevels,
         sources.normalMipLevels,
         sources.pbrMipLevels});
    if (sourceMipCount == 0)
        return;
    // 请求前先建立 epoch 上下文；加载期没有提交过帧时 completedEpoch 为 0。
    pageManager.SetGpuEpochContext(completedEpoch, framesInFlight);
    const uint32_t layerCount =
        static_cast<uint32_t>(streamingState.layerDefinitions.size());
    for (uint32_t layerId = 0; layerId < layerCount; ++layerId)
    {
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
    const TerrainLayerSourceTextures& sources = streamingState.layerSources;
    if (!sources.albedo || !sources.normal || !sources.pbr)
        return;
    const uint32_t sourceMipCount = std::min(
        {sources.albedoMipLevels,
         sources.normalMipLevels,
         sources.pbrMipLevels});
    if (sourceMipCount == 0)
        return;

    const uint32_t layerCount =
        static_cast<uint32_t>(streamingState.layerDefinitions.size());
    uint32_t totalPages = 0;
    for (uint32_t layerId = 0; layerId < layerCount; ++layerId)
    {
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
    const TerrainLayerSourceTextures& sources)
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
    if (sources.albedoWidth == 0 || sources.albedoHeight == 0 ||
        sources.normalWidth == 0 || sources.normalHeight == 0 ||
        sources.pbrWidth == 0 || sources.pbrHeight == 0)
    {
        layerPageManager.AbortUploadBatch(batch);
        throw std::runtime_error(
            "Terrain layer source texture extent is unknown");
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
        uint32_t mipLevels = 0;
        if (upload.key.layerId >= sources.arrayLayers)
        {
            layerPageManager.AbortUploadBatch(batch);
            throw std::runtime_error(
                "Terrain layer id exceeds the source texture array layers");
        }
        // 三张源图的 mip 数可能不同，取最小值保证三张都能 copy。
        mipLevels = std::min(
            {sources.albedoMipLevels,
             sources.normalMipLevels,
             sources.pbrMipLevels});
        if (upload.key.mip >= mipLevels)
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

RendererTerrainLoader::RendererTerrainLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext,
    TerrainStreamingState& streamingState)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
    , streamingState(streamingState)
{
}

RendererTerrainLoadResult RendererTerrainLoader::LoadTerrainObject(
    const nlohmann::json& node,
    const TerrainBuildPlan& plan) const
{
    if (loadContext.offlineOnly)
        throw std::runtime_error("Terrain requires realtime rendering; offline scenes do not support it");

    const Eigen::Vector3f position =
        JsonParser::ParseValue<Eigen::Vector3f>(node.at("position"));
    const Eigen::Vector3f rotation =
        JsonParser::ParseValue<Eigen::Vector3f>(node.at("rotation"));
    const Eigen::Vector3f scale =
        JsonParser::ParseValue<Eigen::Vector3f>(node.at("scale"));
    const std::string objectName = node.at("name").get<std::string>();
    const Eigen::Matrix4f model = BuildModelMatrix(position, rotation, scale);

    RendererMaterialLoader materialLoader(
        pipelineFactory,
        rendererBackend,
        loadContext);
    const std::shared_ptr<MaterialInstance> materialInstance =
        materialLoader.LoadSceneMaterialInstance(plan.desc.materialInstancePath);
    if (!materialInstance)
        throw std::runtime_error(
            "Terrain material instance was not created: " + plan.desc.materialInstancePath);
    const std::string materialKey =
        materialInstance->GetBaseMaterial().lock()->GetMaterialKey();

    // Phase A：M_terrain 通过生成的 8x2 LUT 读取 TR.layers。
    // LUT 是 world-local 生成资源，由 MI 持有；后续 VT/页面化会替换它的上传方式。
    bool usesLayerTable = false;
    for (const MaterialTextureSchemaEntry& texture :
         materialInstance->GetBaseMaterial().lock()->GetMaterialDescriptorSchema().GetTextures())
    {
        if (texture.name == "layerTable")
        {
            usesLayerTable = true;
            break;
        }
    }
    if (usesLayerTable)
    {
        if (!streamingState.layerResources.layerTable)
            throw std::runtime_error("Terrain layer table resource is not initialized");
        // 层表允许 256 层，但 GPU 数组的层数由资产决定；越界层号必须在这里失败。
        const std::shared_ptr<Texture> layerAlbedo = materialInstance->GetTexture("layerAlbedo");
        const std::shared_ptr<Texture> layerNormal = materialInstance->GetTexture("layerNormal");
        const std::shared_ptr<Texture> layerPbr = materialInstance->GetTexture("layerPbr");
        if (!layerAlbedo || !layerNormal || !layerPbr)
            throw std::runtime_error("Terrain layer arrays are not bound");
        if (layerAlbedo->GetArrayLayers() < plan.desc.layers.size() ||
            layerNormal->GetArrayLayers() < plan.desc.layers.size() ||
            layerPbr->GetArrayLayers() < plan.desc.layers.size())
        {
            throw std::runtime_error(
                "Terrain layer table has more layers than the bound texture arrays: " +
                plan.desc.assetPath);
        }
        // L2：层 atlas 的源纹理按 World 共享一套。不同 MI 指向不同纹理时
        // 先拒绝，而不是让多个地形悄悄写入错误的 atlas 内容。
        if (!streamingState.layerSources.albedo)
        {
            streamingState.layerSources.albedo = layerAlbedo;
            streamingState.layerSources.normal = layerNormal;
            streamingState.layerSources.pbr = layerPbr;
            streamingState.layerSources.arrayLayers =
                layerAlbedo->GetArrayLayers();
            streamingState.layerSources.albedoMipLevels =
                layerAlbedo->getMipLevels();
            streamingState.layerSources.normalMipLevels =
                layerNormal->getMipLevels();
            streamingState.layerSources.pbrMipLevels =
                layerPbr->getMipLevels();
            const vk::Extent2D albedoExtent =
                rendererBackend.GetImageExtent(layerAlbedo->getImage());
            const vk::Extent2D normalExtent =
                rendererBackend.GetImageExtent(layerNormal->getImage());
            const vk::Extent2D pbrExtent =
                rendererBackend.GetImageExtent(layerPbr->getImage());
            streamingState.layerSources.albedoWidth = albedoExtent.width;
            streamingState.layerSources.albedoHeight = albedoExtent.height;
            streamingState.layerSources.normalWidth = normalExtent.width;
            streamingState.layerSources.normalHeight = normalExtent.height;
            streamingState.layerSources.pbrWidth = pbrExtent.width;
            streamingState.layerSources.pbrHeight = pbrExtent.height;
            if (layerNormal->GetArrayLayers() !=
                    streamingState.layerSources.arrayLayers ||
                layerPbr->GetArrayLayers() !=
                    streamingState.layerSources.arrayLayers)
            {
                throw std::runtime_error(
                    "Terrain layer albedo/normal/pbr arrays must have identical layer counts");
            }
            // L3：catalog 的 sourceResolution 是页表布局的唯一来源。真实源图
            // 尺寸不一致时页号会指向错误区域，因此禁止 VT；mode 0 继续可用。
            // 现有 B32/M3 验证资产是 32²、catalog 默认 1K，会走到这条警告。
            const bool sourceExtentConsistent =
                albedoExtent.width == albedoExtent.height &&
                albedoExtent.width != 0 &&
                normalExtent.width == albedoExtent.width &&
                normalExtent.height == albedoExtent.height &&
                pbrExtent.width == albedoExtent.width &&
                pbrExtent.height == albedoExtent.height;
            const bool sourceResolutionConsistent =
                sourceExtentConsistent &&
                TerrainLayerVT::CatalogMatchesSourceExtent(
                    plan.desc.layers,
                    albedoExtent.width,
                    albedoExtent.height);
            streamingState.layerSources.catalogResolutionMatches =
                sourceResolutionConsistent;
            if (!sourceResolutionConsistent)
            {
                std::cout
                    << "Warning: terrain layer sourceResolution does not match "
                    << "the bound layer texture extent (catalog="
                    << (plan.desc.layers.empty()
                            ? 0u
                            : plan.desc.layers.front().sourceResolution)
                    << ", source=" << albedoExtent.width << "x"
                    << albedoExtent.height
                    << "); VT layer mode 1 requires a matching source extent; "
                    << "mode 0 remains available: "
                    << plan.desc.assetPath << std::endl;
            }
        }
        else if (streamingState.layerSources.albedo != layerAlbedo ||
                 streamingState.layerSources.normal != layerNormal ||
                 streamingState.layerSources.pbr != layerPbr)
        {
            throw std::runtime_error(
                "All layer-table terrains in a World must share the same "
                "albedo/normal/pbr layer texture arrays: " +
                plan.desc.assetPath);
        }
        const bool vtRequested =
            materialInstance->HasParameter("u_terrainLayerMode") &&
            materialInstance->GetParameter<float>("u_terrainLayerMode") > 0.5f;
        if (vtRequested)
        {
            // 作者显式请求 VT 时不能静默退回 mode 0；源图与 catalog 不一致
            // 会让每个页号都采到错误内容，必须在加载期失败。
            if (!streamingState.layerSources.catalogResolutionMatches)
            {
                throw std::runtime_error(
                    "Terrain layer VT requires sourceResolution to match the "
                    "bound layer texture extent: " + plan.desc.assetPath);
            }
            streamingState.layerVTRequested = true;
        }
        // L2 的 atlas 依赖实际 MI 绑定到的层数组，因此只能在首个地形 MI
        // 载入后创建；创建后本函数立刻绑定，不能留到对象遍历结束再回填。
        if (!streamingState.layerResources.atlasesCreated)
        {
            CreateTerrainLayerAtlasGpuResources(
                rendererBackend,
                streamingState);
        }
        // page table/atlas 尺寸由 TerrainPageManager 固定；MI 不能覆盖成不匹配的值。
        if (materialInstance->HasParameter("u_pageTableSize"))
            materialInstance->SetParameter(
                "u_pageTableSize",
                static_cast<float>(TerrainPageManager::PageTableSize));
        if (materialInstance->HasParameter("u_pageResolution"))
            materialInstance->SetParameter(
                "u_pageResolution",
                static_cast<float>(TerrainPageManager::PageResolution));
        materialInstance->SetTexture("layerTable", streamingState.layerResources.layerTable);
        if (!streamingState.pageResources.pageTable ||
            !streamingState.pageResources.layerIds ||
            !streamingState.pageResources.layerWeights)
        {
            throw std::runtime_error("Terrain page resources are not initialized");
        }
        materialInstance->SetTexture("pageTable", streamingState.pageResources.pageTable);
        materialInstance->SetTexture("layerIds", streamingState.pageResources.layerIds);
        materialInstance->SetTexture("layerWeights", streamingState.pageResources.layerWeights);
        // L3：每 mip 一张页表 + 三张 atlas；mode 0 不使用，但 descriptor
        // 仍必须是合法纹理，因此统一绑定 L2 创建的共享资源。
        for (uint32_t mip = 0;
             mip <= TerrainLayerPageManager::MaxMipLevel;
             ++mip)
        {
            if (!streamingState.layerResources.pageTables[mip])
            {
                throw std::runtime_error(
                    "Terrain layer page table is not initialized for mip " +
                    std::to_string(mip));
            }
            materialInstance->SetTexture(
                "layerPageTable" + std::to_string(mip),
                streamingState.layerResources.pageTables[mip]);
        }
        if (!streamingState.layerResources.albedoAtlas ||
            !streamingState.layerResources.normalAtlas ||
            !streamingState.layerResources.pbrAtlas)
        {
            throw std::runtime_error(
                "Terrain layer atlas resources are not initialized");
        }
        materialInstance->SetTexture(
            "layerAlbedoAtlas",
            streamingState.layerResources.albedoAtlas);
        materialInstance->SetTexture(
            "layerNormalAtlas",
            streamingState.layerResources.normalAtlas);
        materialInstance->SetTexture(
            "layerPbrAtlas",
            streamingState.layerResources.pbrAtlas);
        if (materialInstance->HasParameter("u_layerPageResolution"))
        {
            materialInstance->SetParameter(
                "u_layerPageResolution",
                static_cast<float>(
                    TerrainLayerPageManager::PageResolution));
        }
        if (materialInstance->HasParameter("u_layerAtlasResolution"))
        {
            materialInstance->SetParameter(
                "u_layerAtlasResolution",
                static_cast<float>(
                    TerrainPageManager::AtlasResolution));
        }
        if (materialInstance->HasParameter("u_layerPagesPerAtlasSlice"))
        {
            materialInstance->SetParameter(
                "u_layerPagesPerAtlasSlice",
                static_cast<float>(
                    TerrainLayerPageManager::PagesPerAtlasSlice));
        }
        if (materialInstance->HasParameter("u_layerTilesPerAtlasSide"))
        {
            materialInstance->SetParameter(
                "u_layerTilesPerAtlasSide",
                static_cast<float>(
                    TerrainLayerPageManager::TilesPerAtlasSide));
        }
        if (materialInstance->HasParameter("u_layerMaxMip"))
        {
            materialInstance->SetParameter(
                "u_layerMaxMip",
                static_cast<float>(
                    TerrainLayerPageManager::MaxMipLevel));
        }
    }

    RendererResourceCache& resourceCache = loadContext.resourceCache;
    RendererTerrainLoadResult result;
    // 运行时 LOD 选择尚未接入：这里仍只生成/绑定 LOD0 网格。CPU 侧已能
    // 按 TerrainLodDistancesCm / TerrainSelectLodLevel 生成并选择各级网格，
    // 接入点应在本函数之上按 chunk 距离选择，而不是在 BuildChunks 内隐式决定。
    const std::vector<TerrainMeshChunk> chunks = TerrainMeshBuilder::BuildChunks(plan);
    result.objectPlans.reserve(chunks.size());
    for (const TerrainMeshChunk& chunk : chunks)
    {
        // key 包含 placementId：同一 TR 的多处摆放不能共享/覆盖 chunk 缓冲。
        const std::string chunkKey =
            TerrainChunkResourceKey(plan, chunk.chunkX, chunk.chunkZ);
        const std::shared_ptr<RenderableObject>* cached =
            resourceCache.GetRenderableObject(chunkKey);
        if (cached == nullptr || *cached == nullptr)
        {
            resourceCache.BindRenderableObject(
                chunkKey,
                std::make_shared<RenderableObject>(
                    chunk.vertices,
                    chunk.indices,
                    rendererBackend,
                    chunkKey));
        }

        MeshObjectBuildPlan objectPlan;
        objectPlan.objectName =
            objectName + "::chunk_" +
            std::to_string(chunk.chunkX) + "_" + std::to_string(chunk.chunkZ);
        objectPlan.sceneObjectIdentity = objectName;
        objectPlan.materialSlotIndex = 0;
        objectPlan.materialSlotName = "Terrain";
        objectPlan.debugName = objectPlan.objectName;
        objectPlan.model = model;
        objectPlan.scenePosition = position;
        objectPlan.sceneRotation = rotation;
        objectPlan.sceneScale = scale;
        objectPlan.localBoundsMin = chunk.boundsMin;
        objectPlan.localBoundsMax = chunk.boundsMax;
        objectPlan.meshKey = chunkKey;
        objectPlan.materialKey = materialKey;
        objectPlan.materialInstanceKey = materialInstance->GetName();
        // feedback 只回读共享 VT 页表：没有 layers 的普通地形既没有虚拟页，
        // 也不该把自己的页坐标写进其他分层地形的 feedback。
        objectPlan.terrainFeedbackEligible =
            TerrainParticipatesInFeedback(plan, usesLayerTable);
        result.objectPlans.push_back(std::move(objectPlan));
    }
    return result;
}

} // namespace VL
