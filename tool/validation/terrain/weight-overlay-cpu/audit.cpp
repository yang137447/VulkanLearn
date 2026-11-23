#include "terrain/terrainCompositedWeightOverlay.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainPaintFile.h"

#include <nlohmann/json.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using Json = nlohmann::json;
namespace fs = std::filesystem;

Json ReadJson(const fs::path& path)
{
    std::ifstream stream(path);
    if (!stream)
        throw std::runtime_error("Cannot open " + path.string());
    Json value;
    stream >> value;
    return value;
}

HostImage ReadImage(const fs::path& path)
{
    int width, height, channels;
    auto* pixels = stbi_load_16(path.string().c_str(), &width, &height, &channels, 1);
    if (!pixels)
        throw std::runtime_error("Cannot decode " + path.string());
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 1;
    image.format = HostImage::PixelFormat::R16_UNORM;
    image.data.resize(size_t(width) * height * 2);
    std::memcpy(image.data.data(), pixels, image.data.size());
    stbi_image_free(pixels);
    return image;
}

float TextureSample(const HostImage& image, uint32_t layer, float u, float v)
{
    const float positionX = u * image.width - 0.5f;
    const float positionY = v * image.height - 0.5f;
    const int startX = int(std::floor(positionX));
    const int startY = int(std::floor(positionY));
    const float fractionX = positionX - startX;
    const float fractionY = positionY - startY;
    auto read = [&](int pixelX, int pixelY)
    {
        const auto* pixel = static_cast<const uint8_t*>(image.GetMipData(0, layer)) +
            size_t(std::clamp(pixelY, 0, int(image.height) - 1)) * image.GetMipRowStride(0) +
            size_t(std::clamp(pixelX, 0, int(image.width) - 1)) * image.GetPixelSize();
        if (image.format == HostImage::PixelFormat::R16_UNORM)
        {
            uint16_t value;
            std::memcpy(&value, pixel, sizeof(value));
            return value / 65535.f;
        }
        float value;
        std::memcpy(&value, pixel, sizeof(value));
        return value;
    };
    const float top = read(startX, startY) * (1 - fractionX) +
        read(startX + 1, startY) * fractionX;
    const float bottom = read(startX, startY + 1) * (1 - fractionX) +
        read(startX + 1, startY + 1) * fractionX;
    return top * (1 - fractionY) + bottom * fractionY;
}

struct Tile
{
    VL::TerrainBuildPlan plan;
    HostImage imported;
    HostImage frozen;
    std::vector<uint8_t> mapping;
};

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
            throw std::runtime_error("Usage: weight_overlay_audit RESOURCE_ROOT OUTPUT.json");
        const fs::path root = argv[1];
        const fs::path family = "Generated/Validation/TerrainCompositedVT/Variants/"
            "RVTv2_Full4x4_R8192_Default_LocalLayerArray";
        std::map<int, Tile> tiles;
        Json report;
        report["scope"] = "Native C++ helper / float32 CPU; normalized texture taps emulate shader; no GPU";
        report["inputs"] = Json::array();
        auto input = [&](const fs::path& relative)
        {
            report["inputs"].push_back((root / relative).string());
        };
        std::map<std::string, uint8_t> registry;
        for (int index = 0; index < 16; ++index)
        {
            const std::string suffix = (index < 10 ? "0" : "") + std::to_string(index);
            const auto trPath = family / ("TR_composited_tile_" + suffix + ".json");
            const auto tr = ReadJson(root / trPath);
            std::vector<uint8_t> mapping;
            for (const auto& layer : tr.at("layers"))
            {
                const auto key = layer.at("key").get<std::string>();
                if (!registry.count(key))
                    registry[key] = uint8_t(registry.size());
                mapping.push_back(registry.at(key));
            }
            input(trPath);
            if (index != 5 && index != 6 && index != 9 && index != 10)
                continue;
            Tile tile;
            tile.mapping = mapping;
            tile.plan.desc.gridResolution = tr.at("gridResolution");
            tile.plan.desc.heightmapMargin = tr.at("heightmapMargin");
            tile.plan.desc.layers.resize(tr.at("layers").size());
            const auto heightPath = tr.at("heightmapPath").get<std::string>();
            tile.plan.heightmap = std::make_shared<HostImage>(ReadImage(root / heightPath));
            input(heightPath);
            auto paint = std::make_shared<VL::TerrainLayerPaintData>();
            std::string error;
            const auto paintPath = tr.at("paintPath").get<std::string>();
            if (!VL::LoadTerrainLayerPaintFile(root / paintPath, mapping.size(), *paint, error))
                throw std::runtime_error(error);
            input(paintPath);
            tile.plan.layerPaint = paint;
            const auto miPath = tr.at("materialInstancePath").get<std::string>();
            const auto mi = ReadJson(root / miPath);
            input(miPath);
            const auto weightsPath = mi.at("textures").at("layerWeightMaps").get<std::string>();
            const auto descriptor = ReadJson(root / weightsPath);
            input(weightsPath);
            for (const auto& source : descriptor.at("sources"))
            {
                const auto path = source.get<std::string>();
                const auto image = ReadImage(root / path);
                if (tile.imported.data.empty())
                {
                    tile.imported = image;
                    tile.imported.arrayLayers = mapping.size();
                    tile.imported.data.clear();
                }
                if (image.width != tile.imported.width || image.height != tile.imported.height)
                    throw std::runtime_error("Inconsistent array extents");
                tile.imported.data.insert(tile.imported.data.end(), image.data.begin(), image.data.end());
                input(path);
            }
            tile.frozen = VL::BuildTerrainCompositedWeightOverlay(tile.plan, tile.imported, mapping);
            report["mapping"][suffix] = mapping;
            tiles.emplace(index, std::move(tile));
        }
        auto sample = [&](int index, float u, float v)
        {
            const auto& tile = tiles.at(index);
            const auto transform = VL::TerrainHeightmap::MeshUvTransformFor(
                tile.plan.desc, *tile.plan.heightmap);
            const auto paintUv = transform.Apply(u, v);
            const float span = float(tile.frozen.width - 1);
            const float textureU = (1.f + u * (span - 2.f)) / span;
            const float textureV = (1.f + v * (span - 2.f)) / span;
            std::vector<float> author(tile.mapping.size()), overlay(tile.mapping.size());
            for (uint32_t layer = 0; layer < tile.mapping.size(); ++layer)
            {
                const float base = TextureSample(tile.imported, layer, textureU, textureV);
                author[layer] = tile.plan.layerPaint->Blend(layer, paintUv.x(), paintUv.y(), base);
                overlay[layer] = TextureSample(tile.frozen, layer, textureU, textureV);
            }
            return Json{{"surfaceUv", {u, v}}, {"paintUv", {paintUv.x(), paintUv.y()}},
                {"textureUv", {textureU, textureV}}, {"author", author}, {"overlay", overlay}};
        };
        for (const auto pair : {std::array<int, 3>{5, 6, 0}, {9, 10, 0}, {5, 9, 1}, {6, 10, 1}})
        {
            Json edge;
            edge["pair"] = {pair[0], pair[1]};
            float maxAuthor = 0, maxOverlay = 0, maxError = 0;
            int worst = 0;
            for (int step = 0; step <= 2048; ++step)
            {
                const float along = float(step) / 2048;
                const auto first = sample(pair[0], pair[2] ? along : 1, pair[2] ? 1 : along);
                const auto second = sample(pair[1], pair[2] ? along : 0, pair[2] ? 0 : along);
                for (size_t layer = 0; layer < tiles.at(pair[0]).mapping.size(); ++layer)
                {
                    const float firstAuthor = first["author"][layer];
                    const float secondAuthor = second["author"][layer];
                    const float firstOverlay = first["overlay"][layer];
                    const float secondOverlay = second["overlay"][layer];
                    maxAuthor = std::max(maxAuthor, std::abs(firstAuthor - secondAuthor));
                    const float delta = std::abs(firstOverlay - secondOverlay);
                    if (delta > maxOverlay)
                    {
                        maxOverlay = delta;
                        worst = step;
                        edge["worstSamples"] = {first, second};
                    }
                    maxError = std::max({maxError, std::abs(firstAuthor - firstOverlay),
                        std::abs(secondAuthor - secondOverlay)});
                }
            }
            edge["samples"] = 2049;
            edge["maxAuthorDelta"] = maxAuthor;
            edge["maxOverlayDelta"] = maxOverlay;
            edge["maxOverlayAuthorError"] = maxError;
            edge["worstT"] = float(worst) / 2048;
            report["edges"].push_back(edge);
        }
        for (const auto& entry : tiles)
        {
            const int index = entry.first;
            const float cornerU = index == 5 || index == 9 ? 1 : 0;
            const float cornerV = index == 5 || index == 6 ? 1 : 0;
            report["points"][std::to_string(index)] = {
                sample(index, cornerU, cornerV), sample(index, 0.5f, 0.5f),
                sample(index, 0.25f, 0.75f), sample(index, 0.371f, 0.629f)};
        }
        std::ofstream output(argv[2]);
        output << report.dump(2) << '\n';
        if (!output)
            throw std::runtime_error("Cannot write report");
        std::cout << report["edges"].dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
