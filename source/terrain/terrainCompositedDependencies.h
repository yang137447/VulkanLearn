#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "shader/build/contentHash.h"
#include "terrain/terrainAssetTypes.h"

namespace VL
{
struct TerrainCompositedDependencyRoots
{
    std::filesystem::path resourceRoot;
    std::filesystem::path projectRoot;
};

nlohmann::json BuildTerrainCompositedDependencies(
    const TerrainCompositedDependencyRoots& roots,
    const TerrainBuildPlan& plan,
    const std::array<float, 16>& placement,
    const std::string& materialInstancePath,
    const nlohmann::json& buildConfig);

void ValidateTerrainCompositedAuthors(
    const TerrainCompositedDependencyRoots& roots,
    const nlohmann::json& manifest);

void ValidateTerrainCompositedManifestConfig(
    const nlohmann::json& manifest,
    const TerrainBuildPlan& plan,
    const std::array<float, 16>& placement,
    const std::string& materialInstancePath,
    const nlohmann::json& buildConfig);

ContentDigest TerrainCompositedDependencyHash(const nlohmann::json& manifest);
ContentDigest TerrainCompositedDependencyEntryHash(
    std::string_view bytes, const std::string& mode, const std::string& logicalPath);
}
