#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "terrain/terrainHeightAtlasLayout.h"

namespace VL
{
inline constexpr uint64_t TerrainHeightAtlasStoreMaxPayloadBytes = 32ull * 1024 * 1024;
inline constexpr uint64_t TerrainHeightAtlasStoreMaxWorkingBytes = 128ull * 1024 * 1024;
inline constexpr uint64_t TerrainHeightAtlasStoreMaxManifestBytes = 64ull * 1024;
inline constexpr uint32_t TerrainHeightAtlasStoreHeaderBytes = 64;

struct TerrainHeightAtlasStoreDescription
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pageCount = 0;
    uint32_t layerCount = 0;
};

struct TerrainHeightAtlasStoreData
{
    TerrainHeightAtlasStoreDescription desc;
    std::vector<uint8_t> atlas;
    std::vector<std::array<float, 4>> layerMap;
    std::filesystem::path packagePath;
};

uint64_t TerrainHeightAtlasStorePayloadBytes(
    const TerrainHeightAtlasStoreDescription& desc, uint64_t budgetBytes);
void ValidateTerrainHeightAtlasStoreData(
    const TerrainHeightAtlasStoreData& data, uint64_t budgetBytes);
void WriteTerrainHeightAtlasStore(
    const std::filesystem::path& manifestPath, const TerrainHeightAtlasStoreData& data);
TerrainHeightAtlasStoreData ReadTerrainHeightAtlasStore(
    const std::filesystem::path& manifestPath, uint64_t budgetBytes);
std::filesystem::path TerrainHeightAtlasStorePackagePath(
    const std::filesystem::path& manifestPath);
}
