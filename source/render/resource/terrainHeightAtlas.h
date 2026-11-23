#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "terrain/terrainHeightAtlasLayout.h"

class Texture;

namespace VL
{
class RendererBackendVulkan;

struct TerrainHeightAtlasSource
{
    uint32_t sourceSet = 0;
    std::shared_ptr<Texture> layerHeight;
};

struct TerrainHeightAtlasResources
{
    std::shared_ptr<Texture> heightAtlas;
    std::shared_ptr<Texture> layerMap;
    uint32_t sourceMip = TerrainHeightAtlasLayout::SourceMip;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pageCount = 0;
    uint32_t layerCount = 0;
    uint64_t atlasBytes = 0;
    uint64_t layerMapBytes = 0;
    uint64_t totalBytes = 0;
};

TerrainHeightAtlasResources CreateTerrainHeightAtlas(
    RendererBackendVulkan& rendererBackend,
    const std::vector<TerrainHeightAtlasSource>& sources,
    const std::vector<TerrainHeightAtlasLayer>& layers,
    uint64_t budgetBytes);

void SaveTerrainHeightAtlas(
    RendererBackendVulkan& rendererBackend,
    const TerrainHeightAtlasResources& resources,
    const std::filesystem::path& manifestPath);

TerrainHeightAtlasResources LoadTerrainHeightAtlas(
    RendererBackendVulkan& rendererBackend,
    const std::filesystem::path& manifestPath,
    uint64_t budgetBytes);
}
