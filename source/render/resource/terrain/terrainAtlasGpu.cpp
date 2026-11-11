#include "render/resource/terrain/terrainAtlasGpu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "commonFunction.h"
#include "texture.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"
#include "terrain/terrainLayerTable.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainPageManager.h"

namespace VL
{
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
    // 单一格式组（S5 之前的固定语义）：所有 sourceSet 必须共享三张图的
    // 格式，才能写进同一组 atlas。取第一个 set 作为 atlas 格式来源。
    if (streamingState.layerSources.empty())
    {
        throw std::runtime_error(
            "Terrain layer atlas requires bound albedo/normal/pbr layer arrays");
    }
    const TerrainSourceSetRecord& sources =
        streamingState.layerSources.begin()->second;
    if (!sources.HasTextures())
        throw std::runtime_error(
            "Terrain layer atlas requires bound albedo/normal/pbr layer arrays");

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
} // namespace VL
