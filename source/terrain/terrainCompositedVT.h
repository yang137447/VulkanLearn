#pragma once

#include "shader/build/contentHash.h"
#include "terrain/terrainCompositedIdentity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace VL
{

struct TerrainCompositedVTPageKey
{
    std::string surfaceId;
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    bool operator==(const TerrainCompositedVTPageKey& other) const;
    bool operator<(const TerrainCompositedVTPageKey& other) const;
};

enum class TerrainCompositedVTAddressMode { Clamp, Repeat };
enum class TerrainCompositedVTFormat
{
    RGBA8UNorm, RGBA8Srgb, RG8UNorm, R16UNorm, BC5UNorm, BC7UNorm, BC7Srgb
};

struct TerrainCompositedVTMip
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pagesX = 0;
    uint32_t pagesY = 0;
};

struct TerrainCompositedVTUVRect
{
    double minU = 0;
    double minV = 0;
    double maxU = 0;
    double maxV = 0;
};

class TerrainCompositedVTLayout
{
public:
    static constexpr uint32_t InnerExtent = 128;
    static constexpr uint32_t Border = 4;
    static constexpr uint32_t StoredExtent = InnerExtent + 2 * Border;
    static constexpr uint32_t MaxDimension = 1u << 24;

    TerrainCompositedVTLayout(
        std::string surfaceId, uint32_t width, uint32_t height,
        TerrainCompositedVTAddressMode mode = TerrainCompositedVTAddressMode::Clamp);

    const std::string& SurfaceId() const { return surfaceId; }
    TerrainCompositedVTAddressMode AddressMode() const { return mode; }
    uint32_t MipCount() const { return static_cast<uint32_t>(mips.size()); }
    uint32_t TailMip() const;
    uint64_t PageCount() const;
    const TerrainCompositedVTMip& Mip(uint32_t mip) const;
    bool Contains(const TerrainCompositedVTPageKey& key) const;
    // 返回最近完整覆盖原页 UV 域的更粗祖先；NPOT 可跳 mip，不保证是 mip+1。
    TerrainCompositedVTPageKey Parent(const TerrainCompositedVTPageKey& key) const;
    TerrainCompositedVTMip ValidExtent(const TerrainCompositedVTPageKey& key) const;
    TerrainCompositedVTUVRect PageUVRect(const TerrainCompositedVTPageKey& key) const;
    TerrainCompositedVTUVRect TexelUV(
        const TerrainCompositedVTPageKey& key, uint32_t storedX, uint32_t storedY) const;
    std::vector<TerrainCompositedVTPageKey> DirtyPages(
        const TerrainCompositedVTUVRect& dirty, double filterRadiusTexels = 1.0) const;

private:
    std::string surfaceId;
    TerrainCompositedVTAddressMode mode;
    std::vector<TerrainCompositedVTMip> mips;
};

struct TerrainCompositedVTBuildDesc
{
    std::string surfaceId;
    uint32_t width = 0;
    uint32_t height = 0;
    TerrainCompositedVTAddressMode addressMode = TerrainCompositedVTAddressMode::Clamp;
    ContentDigest dependencyHash;
    uint32_t generatorVersion = TerrainCompositedGeneratorVersion;
    std::string encoding = TerrainCompositedEncoding;
    std::vector<TerrainCompositedVTFormat> channels;

    TerrainCompositedVTLayout Layout() const;
    uint64_t PageBytes() const;
    ContentDigest ContentHash() const;
};

struct TerrainCompositedVTTaskToken
{
    uint64_t worldGeneration = 0;
    uint64_t revision = 0;
    ContentDigest contentHash;
    TerrainCompositedVTPageKey key;

    bool Matches(const TerrainCompositedVTTaskToken& current) const;
};

uint64_t TerrainCompositedVTFormatBytes(TerrainCompositedVTFormat format);
void ValidateTerrainCompositedProductionOutput(const TerrainCompositedVTBuildDesc& surface);
std::string TerrainCompositedVTFormatName(TerrainCompositedVTFormat format);

}
