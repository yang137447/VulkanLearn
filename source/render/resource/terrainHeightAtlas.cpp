#include "render/resource/terrainHeightAtlas.h"

#include <cstring>
#include <map>
#include <stdexcept>

#include "render/backend/rendererBackendVulkan.h"
#include "terrain/terrainHeightAtlasStore.h"
#include "texture.h"

namespace VL
{
namespace
{
struct HeightReadback
{
    RendererBackendVulkan& backend;
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
    void* mapped = nullptr;
    ~HeightReadback()
    {
        if (mapped)
            backend.UnmapMemory(memory);
        if (buffer)
            backend.DestroyBuffer(buffer, memory);
    }
};

void RequireLittleEndian()
{
    const uint16_t value = 1;
    if (*reinterpret_cast<const uint8_t*>(&value) != 1)
        throw std::runtime_error("Terrain height GPU transfer requires a little-endian host");
}

void TransitionHeightImage(
    vk::CommandBuffer command, const Texture& texture,
    vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
    vk::AccessFlags sourceAccess, vk::AccessFlags destinationAccess,
    vk::PipelineStageFlags sourceStage, vk::PipelineStageFlags destinationStage)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setOldLayout(oldLayout).setNewLayout(newLayout)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(texture.getImage())
        .setSrcAccessMask(sourceAccess).setDstAccessMask(destinationAccess)
        .setSubresourceRange(vk::ImageSubresourceRange()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0).setLevelCount(texture.getMipLevels())
            .setBaseArrayLayer(0).setLayerCount(texture.GetArrayLayers()));
    command.pipelineBarrier(sourceStage, destinationStage, {}, {}, {}, barrier);
}
}

TerrainHeightAtlasResources CreateTerrainHeightAtlas(
    RendererBackendVulkan& rendererBackend,
    const std::vector<TerrainHeightAtlasSource>& sources,
    const std::vector<TerrainHeightAtlasLayer>& layers,
    uint64_t budgetBytes)
{
    using Layout = TerrainHeightAtlasLayout;
    const vk::PipelineStageFlags shaderStages =
        vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader;
    std::map<uint32_t, std::shared_ptr<Texture>> textures;
    std::map<uint32_t, uint32_t> canonicalSets;
    std::vector<TerrainHeightAtlasSourceLayout> sourceLayouts;
    for (const auto& source : sources)
    {
        if (!source.layerHeight || !source.layerHeight->getImage() ||
            source.layerHeight->GetFormat() != vk::Format::eR16Unorm ||
            source.layerHeight->getMipLevels() <= Layout::SourceMip ||
            !textures.emplace(source.sourceSet, source.layerHeight).second)
            throw std::invalid_argument(
                "Terrain height atlas requires unique source sets with R16_UNORM mip2 textures");
        uint32_t canonical = source.sourceSet;
        for (const auto& previous : sourceLayouts)
            if (textures.at(previous.sourceSet)->getImage() == source.layerHeight->getImage())
            {
                canonical = previous.sourceSet;
                break;
            }
        canonicalSets.emplace(source.sourceSet, canonical);
        if (canonical != source.sourceSet)
            continue;
        const auto extent = rendererBackend.GetImageExtent(source.layerHeight->getImage());
        if (extent.width == 0 || extent.height == 0)
            throw std::invalid_argument("Terrain height atlas source extent is unknown");
        sourceLayouts.push_back({source.sourceSet,
            std::max(1u, extent.width >> Layout::SourceMip),
            std::max(1u, extent.height >> Layout::SourceMip),
            source.layerHeight->GetArrayLayers()});
    }
    auto canonicalLayers = layers;
    for (auto& layer : canonicalLayers)
    {
        const auto found = canonicalSets.find(layer.sourceSet);
        if (found == canonicalSets.end())
            throw std::invalid_argument("Terrain height atlas layer references missing source set");
        layer.sourceSet = found->second;
    }
    const auto layout = BuildTerrainHeightAtlasLayout(sourceLayouts, canonicalLayers, budgetBytes);
    std::map<uint32_t, std::vector<vk::ImageCopy>> regions;
    uint64_t regionWorkspace = 0;
    for (uint32_t pageIndex = 0; pageIndex < layout.pages.size(); ++pageIndex)
    {
        const auto& page = layout.pages[pageIndex];
        const auto copies = BuildTerrainHeightAtlasCopies(layout, pageIndex);
        regionWorkspace += uint64_t(copies.size()) * sizeof(vk::ImageCopy) * 4;
        if (regionWorkspace + uint64_t(copies.capacity()) * sizeof(TerrainHeightAtlasCopy) >
            TerrainHeightAtlasStoreMaxPayloadBytes)
            throw std::invalid_argument("Terrain height atlas copy region CPU workspace exceeds 32 MiB");
        for (const auto& copy : copies)
        {
            vk::ImageCopy region;
            region.setSrcSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(Layout::SourceMip).setBaseArrayLayer(page.sourceSlice).setLayerCount(1))
                .setDstSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0).setBaseArrayLayer(0).setLayerCount(1))
                .setSrcOffset(vk::Offset3D{static_cast<int32_t>(copy.sourceX),
                    static_cast<int32_t>(copy.sourceY), 0})
                .setDstOffset(vk::Offset3D{static_cast<int32_t>(copy.destinationX),
                    static_cast<int32_t>(copy.destinationY), 0})
                .setExtent(vk::Extent3D{copy.width, copy.height, 1});
            regions[page.sourceSet].push_back(region);
        }
    }
    TerrainHeightAtlasResources result;
    Texture::EmptyArrayDesc atlasDesc;
    atlasDesc.width = layout.width;
    atlasDesc.height = layout.height;
    atlasDesc.format = vk::Format::eR16Unorm;
    atlasDesc.debugName = "TerrainHeightAtlas";
    result.heightAtlas = Texture::CreateEmptyArray(rendererBackend, atlasDesc);
    HostImage image;
    image.width = static_cast<uint32_t>(layout.layerMap.size());
    image.height = 1;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * 16;
    image.data.resize(image.rowStrideBytes);
    std::memcpy(image.data.data(), layout.layerMap.data(), image.data.size());
    Texture::CreateDesc mapDesc;
    mapDesc.semantic = HostImage::TextureSemantic::Data;
    mapDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    mapDesc.generateMipmaps = false;
    mapDesc.filter = vk::Filter::eNearest;
    mapDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    mapDesc.debugName = "TerrainHeightAtlasLayerMap";
    result.layerMap = std::make_shared<Texture>(rendererBackend, image, mapDesc);
    auto command = rendererBackend.BeginSingleTimeCommands();
    try
    {
        TransitionHeightImage(command, *result.heightAtlas,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferWrite,
            shaderStages, vk::PipelineStageFlagBits::eTransfer);
        vk::ClearColorValue clear;
        clear.setFloat32({0, 0, 0, 0});
        rendererBackend.RecordClearColorImage(command, result.heightAtlas->getImage(),
            vk::ImageLayout::eTransferDstOptimal, clear);
        TransitionHeightImage(command, *result.heightAtlas,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite,
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
        for (const auto& entry : regions)
        {
            const auto& source = *textures.at(entry.first);
            TransitionHeightImage(command, source,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferRead,
                shaderStages, vk::PipelineStageFlagBits::eTransfer);
            rendererBackend.RecordCopyImageRegions(command, source.getImage(),
                result.heightAtlas->getImage(), entry.second);
            TransitionHeightImage(command, source,
                vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer, shaderStages);
        }
        TransitionHeightImage(command, *result.heightAtlas,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
            vk::PipelineStageFlagBits::eTransfer, shaderStages);
        TransitionHeightImage(command, *result.layerMap,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
            vk::PipelineStageFlagBits::eTransfer, shaderStages);
        rendererBackend.EndSingleTimeCommands(command);
    }
    catch (...)
    {
        if (command)
            rendererBackend.AbortSingleTimeCommands(command);
        throw;
    }
    result.width = layout.width;
    result.height = layout.height;
    result.pageCount = static_cast<uint32_t>(layout.pages.size());
    result.layerCount = static_cast<uint32_t>(layout.layerMap.size());
    result.atlasBytes = layout.atlasBytes;
    result.layerMapBytes = layout.layerMapBytes;
    result.totalBytes = layout.totalBytes;
    return result;
}

void SaveTerrainHeightAtlas(
    RendererBackendVulkan& rendererBackend,
    const TerrainHeightAtlasResources& resources,
    const std::filesystem::path& manifestPath)
{
    RequireLittleEndian();
    const TerrainHeightAtlasStoreDescription desc{
        resources.width, resources.height, resources.pageCount, resources.layerCount};
    const auto payload = TerrainHeightAtlasStorePayloadBytes(desc, TerrainHeightAtlasStoreMaxPayloadBytes);
    const uint64_t atlasBytes = uint64_t(desc.width) * desc.height * 2;
    const uint64_t mapBytes = uint64_t(desc.layerCount) * 16;
    if (!resources.heightAtlas || !resources.layerMap ||
        resources.sourceMip != TerrainHeightAtlasLayout::SourceMip ||
        resources.atlasBytes != atlasBytes || resources.layerMapBytes != mapBytes ||
        resources.totalBytes != payload ||
        resources.heightAtlas->GetFormat() != vk::Format::eR16Unorm ||
        resources.layerMap->GetFormat() != vk::Format::eR32G32B32A32Sfloat ||
        resources.heightAtlas->GetArrayLayers() != 1 || resources.layerMap->GetArrayLayers() != 1 ||
        resources.heightAtlas->getMipLevels() != 1 || resources.layerMap->getMipLevels() != 1 ||
        rendererBackend.GetImageExtent(resources.heightAtlas->getImage()) != vk::Extent2D{desc.width, desc.height} ||
        rendererBackend.GetImageExtent(resources.layerMap->getImage()) != vk::Extent2D{desc.layerCount, 1})
        throw std::invalid_argument("Terrain height atlas resources disagree with Store metadata/format");
    TerrainHeightAtlasStoreData data;
    data.desc = desc;
    data.atlas.resize(static_cast<size_t>(atlasBytes));
    data.layerMap.resize(desc.layerCount);
    {
        HeightReadback readback{rendererBackend};
        const auto created = rendererBackend.CreateBuffer(payload, vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TerrainHeightAtlasCookReadback");
        readback.buffer = created.first;
        readback.memory = created.second;
        readback.mapped = rendererBackend.MapMemory(readback.memory, payload);
        const vk::PipelineStageFlags shaderStages =
            vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader;
        const std::array<const Texture*, 2> textures{resources.heightAtlas.get(), resources.layerMap.get()};
        auto command = rendererBackend.BeginSingleTimeCommands();
        try
        {
            for (uint32_t index = 0; index < textures.size(); ++index)
            {
                const auto& texture = *textures[index];
                TransitionHeightImage(command, texture, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::ImageLayout::eTransferSrcOptimal,
                    vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferRead,
                    shaderStages, vk::PipelineStageFlagBits::eTransfer);
                vk::BufferImageCopy region;
                region.setBufferOffset(index == 0 ? 0 : atlasBytes)
                    .setImageSubresource(vk::ImageSubresourceLayers()
                        .setAspectMask(vk::ImageAspectFlagBits::eColor)
                        .setMipLevel(0).setBaseArrayLayer(0).setLayerCount(1))
                    .setImageExtent(index == 0 ? vk::Extent3D{desc.width, desc.height, 1} :
                        vk::Extent3D{desc.layerCount, 1, 1});
                command.copyImageToBuffer(texture.getImage(), vk::ImageLayout::eTransferSrcOptimal,
                    readback.buffer, region);
                TransitionHeightImage(command, texture, vk::ImageLayout::eTransferSrcOptimal,
                    vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
                    vk::PipelineStageFlagBits::eTransfer, shaderStages);
            }
            vk::MemoryBarrier host;
            host.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eHostRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
            rendererBackend.EndSingleTimeCommands(command);
        }
        catch (...)
        {
            if (command)
                rendererBackend.AbortSingleTimeCommands(command);
            throw;
        }
        std::memcpy(data.atlas.data(), readback.mapped, static_cast<size_t>(atlasBytes));
        std::memcpy(data.layerMap.data(),
            static_cast<const uint8_t*>(readback.mapped) + atlasBytes, static_cast<size_t>(mapBytes));
    }
    WriteTerrainHeightAtlasStore(manifestPath, data);
}

TerrainHeightAtlasResources LoadTerrainHeightAtlas(
    RendererBackendVulkan& rendererBackend,
    const std::filesystem::path& manifestPath,
    uint64_t budgetBytes)
{
    RequireLittleEndian();
    auto data = ReadTerrainHeightAtlasStore(manifestPath, budgetBytes);
    TerrainHeightAtlasResources result;
    const auto& desc = data.desc;
    result.width = desc.width;
    result.height = desc.height;
    result.pageCount = desc.pageCount;
    result.layerCount = desc.layerCount;
    result.atlasBytes = data.atlas.size();
    result.layerMapBytes = uint64_t(desc.layerCount) * 16;
    result.totalBytes = result.atlasBytes + result.layerMapBytes;
    HostImage image;
    image.width = desc.width;
    image.height = desc.height;
    image.channels = 1;
    image.format = HostImage::PixelFormat::R16_UNORM;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = desc.width * 2;
    image.data = std::move(data.atlas);
    Texture::CreateDesc createDesc;
    createDesc.semantic = HostImage::TextureSemantic::Data;
    createDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    createDesc.generateMipmaps = false;
    createDesc.textureArray = true;
    createDesc.flipY = false;
    createDesc.filter = vk::Filter::eLinear;
    createDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    createDesc.debugName = "TerrainHeightAtlasCooked";
    result.heightAtlas = std::make_shared<Texture>(rendererBackend, image, createDesc);
    image.width = desc.layerCount;
    image.height = 1;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.rowStrideBytes = desc.layerCount * 16;
    image.data.resize(image.rowStrideBytes);
    std::memcpy(image.data.data(), data.layerMap.data(), image.data.size());
    createDesc.textureArray = false;
    createDesc.filter = vk::Filter::eNearest;
    createDesc.debugName = "TerrainHeightAtlasCookedLayerMap";
    result.layerMap = std::make_shared<Texture>(rendererBackend, image, createDesc);
    auto command = rendererBackend.BeginSingleTimeCommands();
    try
    {
        const vk::PipelineStageFlags shaderStages =
            vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader;
        for (const auto& texture : {result.heightAtlas, result.layerMap})
            TransitionHeightImage(command, *texture,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer, shaderStages);
        rendererBackend.EndSingleTimeCommands(command);
    }
    catch (...)
    {
        if (command)
            rendererBackend.AbortSingleTimeCommands(command);
        throw;
    }
    return result;
}
}
