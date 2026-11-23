#include "render/resource/terrain/terrainAtlasGpu.h"

#include <stdexcept>
#include "texture.h"
#include "terrain/terrainLayerTable.h"
#include "terrain/terrainPageManager.h"

namespace VL
{
namespace
{
Texture::CreateDesc DataTexture(const char* name, vk::Filter filter)
{
    Texture::CreateDesc desc;
    desc.semantic = HostImage::TextureSemantic::Data;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.filter = filter;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = name;
    return desc;
}

HostImage EmptyRgba(uint32_t size)
{
    HostImage image;
    image.width = image.height = size;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA8_UNORM;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = size * 4;
    image.data.assign(size_t(size) * size * 4, 0);
    return image;
}
}

bool IsBlockCompressedFormat(vk::Format format)
{
    return format >= vk::Format::eBc1RgbUnormBlock && format <= vk::Format::eBc7SrgbBlock;
}

uint64_t EstimateTextureBytes(vk::Format format, uint32_t width, uint32_t height, uint32_t arrayLayers)
{
    if (IsBlockCompressedFormat(format))
        return uint64_t((width + 3) / 4) * ((height + 3) / 4) * 16 * arrayLayers;
    uint64_t texelBytes = 0;
    switch (format)
    {
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Srgb: texelBytes = 1; break;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb: texelBytes = 4; break;
    case vk::Format::eR16G16B16A16Sfloat: texelBytes = 8; break;
    case vk::Format::eR32G32B32A32Sfloat: texelBytes = 16; break;
    default: return 0;
    }
    return uint64_t(width) * height * texelBytes * arrayLayers;
}

std::shared_ptr<Texture> CreateTerrainPageTableGpuResource(RendererBackendVulkan& backend)
{
    return std::make_shared<Texture>(backend, EmptyRgba(TerrainPageManager::PageTableSize),
        DataTexture("TerrainPageTable", vk::Filter::eNearest));
}

void CreateTerrainPageAtlasGpuResources(RendererBackendVulkan& backend, TerrainPageGpuResources& resources)
{
    const auto image = EmptyRgba(TerrainPageManager::AtlasResolution);
    resources.layerIds = std::make_shared<Texture>(backend, image,
        DataTexture("TerrainCoverageIdsAtlas", vk::Filter::eNearest));
    resources.layerWeights = std::make_shared<Texture>(backend, image,
        DataTexture("TerrainCoverageWeightsAtlas", vk::Filter::eLinear));
}

TerrainPageGpuResources CreateTerrainPageGpuResources(RendererBackendVulkan& backend)
{
    TerrainPageGpuResources resources;
    resources.pageTable = CreateTerrainPageTableGpuResource(backend);
    CreateTerrainPageAtlasGpuResources(backend, resources);
    return resources;
}

TerrainLayerGpuResources CreateTerrainLayerGpuResources(
    RendererBackendVulkan& backend, const std::vector<TerrainLayerDesc>& layers)
{
    TerrainLayerGpuResources resources;
    resources.layerTable = std::make_shared<Texture>(backend, TerrainLayerTable::BuildImage(layers),
        DataTexture("TerrainLayerTable", vk::Filter::eNearest));
    return resources;
}
}
