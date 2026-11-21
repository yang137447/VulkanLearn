#pragma once

#include "terrain/terrainCompositedVT.h"
#include "terrain/terrainCompositedProduction.h"

#include <filesystem>
#include <functional>
#include <map>
#include <vector>

namespace VL
{

struct TerrainCompositedVTPageData
{
    TerrainCompositedVTPageKey key;
    std::vector<uint8_t> bytes;
};

struct TerrainCompositedVTPageRecord
{
    uint64_t offset = 0;
    uint64_t size = 0;
    ContentDigest hash;
};

struct TerrainCompositedVTStoreLimits
{
    uint64_t maxManifestBytes = 64u * 1024u * 1024u;
    uint64_t maxPackageBytes = uint64_t(1) << 40;
    uint64_t maxPages = 1000000;
    uint64_t maxInputObjectBytes = uint64_t(1) << 40;
    uint64_t maxInputBytes = uint64_t(1) << 40;
};

struct TerrainCompositedVTPublication
{
    TerrainCompositedProductionClosure production;
    std::vector<TerrainCompositedVTBuildDesc> surfaces;
    std::vector<TerrainCompositedVTPageKey> packagedPages;
};

class TerrainCompositedVTStore
{
public:
    static constexpr uint32_t Version = 2;
    using PageProducer = std::function<std::vector<uint8_t>(const TerrainCompositedVTPageKey&)>;

    static void Publish(
        const std::filesystem::path& manifestPath,
        const TerrainCompositedVTPublication& publication,
        const PageProducer& producePage,
        const TerrainCompositedVTStoreLimits& limits = {});
    static void Publish(
        const std::filesystem::path& manifestPath,
        const TerrainCompositedVTPublication& publication,
        const std::vector<TerrainCompositedVTPageData>& pages,
        const TerrainCompositedVTStoreLimits& limits = {});
    static TerrainCompositedVTStore Open(
        const std::filesystem::path& manifestPath,
        const TerrainCompositedVTStoreLimits& limits = {});

    const std::vector<TerrainCompositedVTBuildDesc>& Surfaces() const { return surfaces; }
    const std::map<TerrainCompositedVTPageKey, TerrainCompositedVTPageRecord>& Pages() const
    {
        return pages;
    }
    const std::filesystem::path& PackagePath() const { return packagePath; }
    const TerrainCompositedProductionClosure& Production() const { return production; }
    const TerrainCompositedVTBuildDesc& Surface(const std::string& surfaceId) const;
    bool HasPackagedPage(const TerrainCompositedVTPageKey& key) const;
    std::vector<uint8_t> ReadPage(const TerrainCompositedVTPageKey& key) const;
    std::vector<uint8_t> ReadInput(const std::string& objectId) const;
    void ValidateAll() const;

private:
    std::filesystem::path inputRoot;
    std::filesystem::path packagePath;
    uint64_t packageSize = 0;
    ContentDigest packageHash;
    TerrainCompositedProductionClosure production;
    TerrainCompositedVTStoreLimits limits;
    std::vector<TerrainCompositedVTBuildDesc> surfaces;
    std::map<TerrainCompositedVTPageKey, TerrainCompositedVTPageRecord> pages;
};

}
