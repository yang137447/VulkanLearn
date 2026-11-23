#pragma once

#include "composited_page_reference.h"
#include "composited_adjacency_cases.h"
#include "texture.h"
#include "render/backend/rendererBackendVulkan.h"
#include <functional>

namespace TerrainPageFixture
{
using namespace TerrainPageReference;

struct Fixture
{
    Baker::AdjacencySnapshot snapshot;
    std::map<std::string, Surface> reference;
    std::vector<std::shared_ptr<Texture>> owners;
};

inline Image Constant(uint32_t width, uint32_t height, uint32_t layers,
    uint32_t mips, const std::function<Colour(uint32_t, uint32_t, uint32_t, uint32_t)>& value,
    bool srgb = false, bool repeat = false)
{
    Image image;
    image.width = width;
    image.height = height;
    image.layers = layers;
    image.mips = mips;
    image.srgb = srgb;
    image.repeat = repeat;
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            std::vector<Colour> pixels;
            for (uint32_t row = 0; row < std::max(1u, height >> mip); ++row)
                for (uint32_t column = 0; column < std::max(1u, width >> mip); ++column)
                    pixels.push_back(value(layer, mip, column, row));
            image.planes.push_back(std::move(pixels));
        }
    return image;
}

inline Baker::SampledTexture Upload(VL::RendererBackendVulkan& backend, Fixture& fixture,
    const Image& image, bool floating, bool array)
{
    HostImage host;
    host.width = image.width;
    host.height = image.height;
    host.arrayLayers = image.layers;
    host.mipLevels = image.mips;
    host.format = floating ? HostImage::PixelFormat::RGBA32_FLOAT :
        image.srgb ? HostImage::PixelFormat::RGBA8_SRGB : HostImage::PixelFormat::RGBA8_UNORM;
    host.semantic = HostImage::TextureSemantic::Data;
    host.data.resize(host.GetTotalBytes());
    size_t offset = 0;
    for (const auto& plane : image.planes)
        for (const auto& colour : plane)
            for (const double component : colour)
            {
                if (floating)
                {
                    const float value = float(component);
                    std::memcpy(host.data.data() + offset, &value, 4);
                    offset += 4;
                }
                else
                    host.data[offset++] = uint8_t(std::lround(component * 255));
            }
    Texture::CreateDesc desc;
    desc.debugName = "BakerOracleInput";
    desc.generateMipmaps = false;
    desc.textureArray = array;
    desc.flipY = false;
    desc.wrapMode = image.repeat ? vk::SamplerAddressMode::eRepeat : vk::SamplerAddressMode::eClampToEdge;
    auto texture = std::make_shared<Texture>(backend, host, desc);
    fixture.owners.push_back(texture);
    Baker::SampledTexture result;
    result.descriptor = texture->GetDescriptorInfo();
    result.width = image.width;
    result.height = image.height;
    result.layers = image.layers;
    result.mipLevels = image.mips;
    result.encoding = image.srgb ? Baker::Encoding::SrgbView : Baker::Encoding::Linear;
    return result;
}

inline Fixture Build(VL::RendererBackendVulkan& backend,
    const VL::TerrainCompositedAdjacencyGraph& graph, uint32_t weightMode, uint32_t sourceExtent,
    bool spatial = false)
{
    Fixture fixture;
    fixture.snapshot.graph = graph;
    fixture.snapshot.sourceSurfaceId = graph.SurfaceIds().front();
    uint32_t index = 0;
    for (const auto& id : graph.SurfaceIds())
    {
        Surface surface;
        auto& input = surface.input;
        input.surfaceId = id;
        input.contentIdentity = "baker-oracle-" + id;
        input.retainFragmentPostprocess = true;
        input.uvScale = 2;
        input.retainedFragment.heightBlendStrength = 3;
        input.frozenHeightBlendFade = 1;
        input.weightSource = weightMode == 1 || (weightMode == 2 && index % 2)
            ? Baker::WeightSource::LocalLayerArray : Baker::WeightSource::CoverageAtlas;
        input.localLayerCount = 3;
        input.localToGlobalLayer[0] = 19;
        input.localToGlobalLayer[1] = 3;
        input.localToGlobalLayer[2] = 7;
        input.globalToSourceSlice[19] = index % 3;
        input.globalToSourceSlice[3] = (index + 1) % 3;
        input.globalToSourceSlice[7] = (index + 2) % 3;
        input.surfaceUvOffset = {.125f, .25f};
        input.surfaceUvScale = {.5f, .75f};
        if (spatial)
        {
            input.coverageUvOffset = {.125f, .25f};
            input.coverageUvScale = {.75f, .5f};
        }
        input.frameUvOffset = {.125f, .25f};
        input.frameUvScale = {.75f, .5f};
        const float scale = float(index % 3 + 1);
        if (index % 2)
            input.localToWorld = {0, 0, -scale, 0, 0, scale, 0, 0, scale, 0, 0, 0,
                float(index * 1000), 64, -128, 1};
        else
            input.localToWorld = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0,
                float(index * 1000), 64, -128, 1};
        surface.coverageIds = {3, 7, 19, 0};
        surface.coverageWeights = {64. / 255, 32. / 255, 159. / 255, 0};
        auto& images = surface.images;
        images["table"] = Constant(1, 1, 1, 1, [](auto, auto, auto, auto) { return Colour{0, 0, 0, 1}; });
        images["ids"] = Constant(128, 128, 1, 1, [=](auto, auto, uint32_t column, uint32_t row) {
            if (spatial && (column / 16 + row / 16) % 2)
                return Colour{19. / 255, 3. / 255, 7. / 255, 0};
            return Colour{3. / 255, 7. / 255, 19. / 255, 0}; });
        images["weights"] = Constant(128, 128, 1, 1, [&](auto, auto, auto, auto) {
            return Colour{surface.coverageWeights[0], surface.coverageWeights[1],
                surface.coverageWeights[2], 0}; });
        images["catalog"] = Constant(256, 4, 1, 1, [=](auto, auto, uint32_t column, uint32_t row) {
            return row == 1 ? Colour{0, 0, 1 + double(column % 3) * .25,
                spatial ? double(column % 3) * .25 : 0}
                : row == 3 ? Colour{0, 0, 0, .2} : Colour{}; });
        images["local"] = Constant(7, 7, 3, 1, [=](uint32_t layer, auto, uint32_t column, uint32_t row) {
            const double variation = spatial ? double(column + row) / 256 : 0;
            return Colour{layer == 0 ? .625 - variation : layer == 1 ? .25 + variation : .125, 0, 0, 1}; });
        images["frame-normal"] = Constant(4, 4, 1, 1, [=](auto, auto, uint32_t column, uint32_t row) {
            return Colour{double(index % 3) * .125 + (spatial ? double(column) / 128 : 0),
                1, .125 + (spatial ? double(row) / 128 : 0), 1}; });
        images["frame-tangent"] = Constant(4, 4, 1, 1, [=](auto, auto, uint32_t column, uint32_t row) {
            return Colour{1, .0625 + (spatial ? double(row) / 256 : 0),
                double(index % 2) * .125 + (spatial ? double(column) / 256 : 0), index % 2 ? -1. : 1.}; });
        for (const auto name : {"albedo", "normal", "orm", "height"})
        {
            const std::string role = name;
            const uint32_t extent = role == "height" ? 8 : sourceExtent;
            const uint32_t mips = uint32_t(std::log2(extent)) + 1;
            images[role] = Constant(extent, extent, 3, mips,
                [=](uint32_t layer, uint32_t mip, uint32_t column, uint32_t row) {
                    const double variation = spatial ? (double((column + row * 2) % 3) - 1) * 2 : 0;
                    if (role == "albedo")
                        return Colour{(double(45 + layer * 50 + index % 4 * 3 + mip * 2) + variation) / 255,
                            double(80 + layer * 25) / 255, double(150 - layer * 30) / 255, 1};
                    if (role == "normal")
                        return Colour{(double(96 + layer * 24) + variation) / 255, double(112 + layer * 8) / 255, 240. / 255, 1};
                    if (role == "orm")
                        return Colour{204. / 255, (double(51 + layer * 60 + mip) + variation) / 255,
                            double(layer * 40) / 255, 1};
                    return Colour{double(64 + layer * 64) / 255, 0, 0, 1};
                }, role == "albedo", true);
        }
        const auto upload = [&](const std::string& name, bool floating = false, bool array = false) {
            return Upload(backend, fixture, images.at(name), floating, array);
        };
        input.coveragePageTable = upload("table");
        input.coverageIds = upload("ids");
        input.coverageWeights = upload("weights");
        input.localLayerWeights = upload("local", true, true);
        input.layerCatalog = upload("catalog", true);
        input.layerAlbedo = upload("albedo", false, true);
        input.layerNormal = upload("normal", false, true);
        input.layerOrm = upload("orm", false, true);
        input.layerHeight = upload("height", false, true);
        input.surfaceLocalNormal = upload("frame-normal", true);
        input.surfaceLocalTangent = upload("frame-tangent", true);
        fixture.snapshot.surfaces.push_back({id, input});
        fixture.reference.emplace(id, std::move(surface));
        ++index;
    }
    return fixture;
}
}
