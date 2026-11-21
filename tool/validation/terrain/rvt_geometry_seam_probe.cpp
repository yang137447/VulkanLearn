#include "terrain/terrainMeshBuilder.h"
#include "terrain/terrainHeightmap.h"
#include <nlohmann/json.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <fstream>
#include <iostream>
#include <cstring>

using Json = nlohmann::json;

Json Read(const std::string& path)
{
    std::ifstream stream(path);
    return Json::parse(stream);
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const Json request = Read(argv[1]);
    Json result = Json::array();
    for (const auto& item : request)
    {
        const auto terrain = Read(item.at("tr"));
        VL::TerrainBuildPlan plan;
        auto& desc = plan.desc;
        desc.assetPath = item.at("tr");
        desc.heightmapPath = item.at("height");
        desc.gridResolution = terrain.at("gridResolution");
        desc.chunkQuads = terrain.at("chunkQuads");
        desc.heightmapMargin = terrain.at("heightmapMargin");
        desc.heightScale = terrain.at("heightScale");
        desc.heightOffset = terrain.at("heightOffset");
        desc.size = Eigen::Vector2f(terrain.at("size")[0], terrain.at("size")[1]);
        desc.pageOrigin = Eigen::Vector2i(terrain.at("pageOrigin")[0], terrain.at("pageOrigin")[1]);
        desc.pageTilesPerSide = terrain.at("pageTilesPerSide");
        int width, height, channels;
        auto* pixels = stbi_load_16(desc.heightmapPath.c_str(), &width, &height, &channels, 1);
        if (!pixels) return 3;
        auto image = std::make_shared<HostImage>();
        image->width = width;
        image->height = height;
        image->format = HostImage::PixelFormat::R32_FLOAT;
        image->data.resize(size_t(width) * height * 4);
        for (int index = 0; index < width * height; ++index)
        {
            const float value = float(pixels[index]) / 65535.0f;
            std::memcpy(image->data.data() + size_t(index) * 4, &value, 4);
        }
        stbi_image_free(pixels);
        plan.heightmap = image;
        Json tile;
        tile["id"] = item.at("id");
        tile["levels"] = Json::array();
        const int count = desc.gridResolution / desc.chunkQuads;
        VL::TerrainChunkBuildFilter filter;
        filter.chunkMask.resize(count * count, 0);
        const int chunkX = item.at("chunk")[0], chunkZ = item.at("chunk")[1];
        filter.chunkMask[chunkZ * count + chunkX] = 1;
        for (unsigned level = 0; level < terrain.at("lodLevels").get<unsigned>(); ++level)
        {
            const auto chunks = VL::TerrainMeshBuilder::BuildChunks(plan, level, &filter);
            const auto& chunk = chunks.at(0);
            Json edges = Json::array();
            for (const auto& vertex : chunk.vertices)
            {
                const float u = vertex.texCoord.x(), v = vertex.texCoord.y();
                if (u != 0 && u != 1 && v != 0 && v != 1) continue;
                edges.push_back({
                    {"uv", {u, v}},
                    {"position", {vertex.position.x(), vertex.position.y(), vertex.position.z()}},
                    {"normal", {vertex.normal.x(), vertex.normal.y(), vertex.normal.z()}},
                    {"uv1", {vertex.texCoord1.x(), vertex.texCoord1.y()}}
                });
            }
            tile["levels"].push_back({{"level", level}, {"edgeVertices", edges},
                {"vertices", chunk.vertices.size()}, {"indices", chunk.indices.size()},
                {"patchIndices", chunk.patchIndices.size()}});
        }
        result.push_back(tile);
    }
    std::cout << result.dump(2) << '\n';
}
