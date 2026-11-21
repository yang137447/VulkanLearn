#include "terrain/terrainCompositedVT.h"
#include "terrain/terrainCompositedIdentity.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace VL
{
void ValidateTerrainCompositedProductionOutput(const TerrainCompositedVTBuildDesc& surface)
{
    if (surface.generatorVersion != TerrainCompositedGeneratorVersion ||
        surface.encoding != TerrainCompositedEncoding ||
        surface.channels != std::vector<TerrainCompositedVTFormat>{
            TerrainCompositedVTFormat::RGBA8Srgb, TerrainCompositedVTFormat::RGBA8UNorm,
            TerrainCompositedVTFormat::RGBA8UNorm})
        throw std::invalid_argument("RVT generation 3 requires fixed RGBA8 sRGB/local-normal/ORM output; BC production is unsupported");
}

namespace
{

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::vector<std::pair<double, double>> Intervals(
    double low, double high, TerrainCompositedVTAddressMode mode)
{
    if (mode == TerrainCompositedVTAddressMode::Clamp)
    {
        low = std::max(0.0, low);
        high = std::min(1.0, high);
        return low < high ? std::vector<std::pair<double, double>>{{low, high}}
                          : std::vector<std::pair<double, double>>{};
    }
    if (high - low >= 1.0)
    {
        return {{0.0, 1.0}};
    }
    const double begin = low - std::floor(low);
    const double end = begin + high - low;
    if (end <= 1.0)
    {
        return {{begin, end}};
    }
    return {{begin, 1.0}, {0.0, end - 1.0}};
}

double Address(double coordinate, uint32_t extent, TerrainCompositedVTAddressMode mode)
{
    if (mode == TerrainCompositedVTAddressMode::Repeat)
    {
        return coordinate - std::floor(coordinate);
    }
    return std::clamp(coordinate, 0.5 / extent, 1.0 - 0.5 / extent);
}

}

bool TerrainCompositedVTPageKey::operator==(const TerrainCompositedVTPageKey& other) const
{
    return std::tie(surfaceId, mip, x, y) == std::tie(other.surfaceId, other.mip, other.x, other.y);
}

bool TerrainCompositedVTPageKey::operator<(const TerrainCompositedVTPageKey& other) const
{
    return std::tie(surfaceId, mip, x, y) < std::tie(other.surfaceId, other.mip, other.x, other.y);
}

TerrainCompositedVTLayout::TerrainCompositedVTLayout(
    std::string surfaceIdentity, uint32_t width, uint32_t height,
    TerrainCompositedVTAddressMode addressMode)
    : surfaceId(std::move(surfaceIdentity)), mode(addressMode)
{
    Require(!surfaceId.empty() && surfaceId.size() <= 1024 &&
        surfaceId.find('\0') == std::string::npos, "Invalid VT surface identity");
    Require(width > 0 && height > 0 && width <= MaxDimension && height <= MaxDimension,
        "Invalid VT dimensions");
    Require(mode == TerrainCompositedVTAddressMode::Clamp ||
        mode == TerrainCompositedVTAddressMode::Repeat, "Invalid VT address mode");
    for (;;)
    {
        mips.push_back({width, height, (width + InnerExtent - 1) / InnerExtent,
            (height + InnerExtent - 1) / InnerExtent});
        if (width == 1 && height == 1)
        {
            break;
        }
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
}

const TerrainCompositedVTMip& TerrainCompositedVTLayout::Mip(uint32_t mip) const
{
    return mips.at(mip);
}

uint32_t TerrainCompositedVTLayout::TailMip() const
{
    for (uint32_t mip = 0; mip < MipCount(); ++mip)
    {
        if (mips[mip].pagesX == 1 && mips[mip].pagesY == 1)
        {
            return mip;
        }
    }
    throw std::runtime_error("Missing VT tail mip");
}

uint64_t TerrainCompositedVTLayout::PageCount() const
{
    uint64_t count = 0;
    for (const auto& mip : mips)
    {
        count += uint64_t(mip.pagesX) * mip.pagesY;
    }
    return count;
}

bool TerrainCompositedVTLayout::Contains(const TerrainCompositedVTPageKey& key) const
{
    return key.surfaceId == surfaceId && key.mip < MipCount() &&
        key.x < mips[key.mip].pagesX && key.y < mips[key.mip].pagesY;
}

TerrainCompositedVTPageKey TerrainCompositedVTLayout::Parent(
    const TerrainCompositedVTPageKey& key) const
{
    Require(Contains(key), "Invalid VT page key");
    Require(key.mip + 1 < MipCount(), "VT root page has no parent");
    const auto& child = Mip(key.mip);
    const uint64_t beginX = uint64_t(key.x) * InnerExtent;
    const uint64_t beginY = uint64_t(key.y) * InnerExtent;
    const uint64_t endX = std::min(beginX + InnerExtent, uint64_t(child.width));
    const uint64_t endY = std::min(beginY + InnerExtent, uint64_t(child.height));
    for (uint32_t mip = key.mip + 1; mip < MipCount(); ++mip)
    {
        const auto& parent = Mip(mip);
        const uint64_t pageX = beginX * parent.width / (uint64_t(child.width) * InnerExtent);
        const uint64_t pageY = beginY * parent.height / (uint64_t(child.height) * InnerExtent);
        const uint64_t parentEndX = std::min((pageX + 1) * InnerExtent, uint64_t(parent.width));
        const uint64_t parentEndY = std::min((pageY + 1) * InnerExtent, uint64_t(parent.height));
        if (endX * parent.width <= parentEndX * child.width &&
            endY * parent.height <= parentEndY * child.height)
            return {surfaceId, mip, static_cast<uint32_t>(pageX), static_cast<uint32_t>(pageY)};
    }
    throw std::runtime_error("Missing VT covering ancestor");
}

TerrainCompositedVTMip TerrainCompositedVTLayout::ValidExtent(
    const TerrainCompositedVTPageKey& key) const
{
    Require(Contains(key), "Invalid VT page key");
    const auto& mip = Mip(key.mip);
    return {std::min(InnerExtent, mip.width - key.x * InnerExtent),
        std::min(InnerExtent, mip.height - key.y * InnerExtent), 1, 1};
}

TerrainCompositedVTUVRect TerrainCompositedVTLayout::PageUVRect(
    const TerrainCompositedVTPageKey& key) const
{
    const auto valid = ValidExtent(key);
    const auto& mip = Mip(key.mip);
    return {double(key.x * InnerExtent) / mip.width, double(key.y * InnerExtent) / mip.height,
        double(key.x * InnerExtent + valid.width) / mip.width,
        double(key.y * InnerExtent + valid.height) / mip.height};
}

TerrainCompositedVTUVRect TerrainCompositedVTLayout::TexelUV(
    const TerrainCompositedVTPageKey& key, uint32_t storedX, uint32_t storedY) const
{
    Require(Contains(key) && storedX < StoredExtent && storedY < StoredExtent,
        "Invalid VT stored texel");
    const auto& mip = Mip(key.mip);
    const double texelU = (double(key.x * InnerExtent) + storedX - Border + 0.5) / mip.width;
    const double texelV = (double(key.y * InnerExtent) + storedY - Border + 0.5) / mip.height;
    const double sampleU = Address(texelU, mip.width, mode);
    const double sampleV = Address(texelV, mip.height, mode);
    return {sampleU, sampleV, sampleU, sampleV};
}

std::vector<TerrainCompositedVTPageKey> TerrainCompositedVTLayout::DirtyPages(
    const TerrainCompositedVTUVRect& dirty, double filterRadiusTexels) const
{
    Require(std::isfinite(dirty.minU) && std::isfinite(dirty.minV) &&
        std::isfinite(dirty.maxU) && std::isfinite(dirty.maxV) &&
        dirty.minU <= dirty.maxU && dirty.minV <= dirty.maxV &&
        std::isfinite(filterRadiusTexels) && filterRadiusTexels >= 0 &&
        filterRadiusTexels <= MaxDimension, "Invalid VT dirty rectangle/filter radius");
    std::vector<TerrainCompositedVTPageKey> pages;
    auto region = dirty;
    if (mode == TerrainCompositedVTAddressMode::Clamp)
    {
        region.minU = std::max(0.0, region.minU);
        region.minV = std::max(0.0, region.minV);
        region.maxU = std::min(1.0, region.maxU);
        region.maxV = std::min(1.0, region.maxV);
    }
    if (region.minU >= region.maxU || region.minV >= region.maxV)
    {
        return pages;
    }
    for (uint32_t mipIndex = 0; mipIndex < MipCount(); ++mipIndex)
    {
        const auto& mip = Mip(mipIndex);
        const double paddingU = (Border + filterRadiusTexels) / mip.width;
        const double paddingV = (Border + filterRadiusTexels) / mip.height;
        const auto horizontal = Intervals(region.minU - paddingU, region.maxU + paddingU, mode);
        const auto vertical = Intervals(region.minV - paddingV, region.maxV + paddingV, mode);
        for (const auto& spanU : horizontal)
        {
            for (const auto& spanV : vertical)
            {
                const auto beginX = static_cast<uint32_t>(std::floor(spanU.first * mip.width / InnerExtent));
                const auto beginY = static_cast<uint32_t>(std::floor(spanV.first * mip.height / InnerExtent));
                const auto endX = std::min(mip.pagesX,
                    static_cast<uint32_t>(std::ceil(spanU.second * mip.width / InnerExtent)));
                const auto endY = std::min(mip.pagesY,
                    static_cast<uint32_t>(std::ceil(spanV.second * mip.height / InnerExtent)));
                for (uint32_t pageY = beginY; pageY < endY; ++pageY)
                {
                    for (uint32_t pageX = beginX; pageX < endX; ++pageX)
                    {
                        pages.push_back({surfaceId, mipIndex, pageX, pageY});
                    }
                }
            }
        }
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

std::string TerrainCompositedVTFormatName(TerrainCompositedVTFormat format)
{
    switch (format)
    {
    case TerrainCompositedVTFormat::RGBA8UNorm: return "rgba8-unorm";
    case TerrainCompositedVTFormat::RGBA8Srgb: return "rgba8-srgb";
    case TerrainCompositedVTFormat::RG8UNorm: return "rg8-unorm";
    case TerrainCompositedVTFormat::R16UNorm: return "r16-unorm";
    case TerrainCompositedVTFormat::BC5UNorm: return "bc5-unorm";
    case TerrainCompositedVTFormat::BC7UNorm: return "bc7-unorm";
    case TerrainCompositedVTFormat::BC7Srgb: return "bc7-srgb";
    }
    throw std::runtime_error("Unsupported VT format");
}

uint64_t TerrainCompositedVTFormatBytes(TerrainCompositedVTFormat format)
{
    constexpr uint64_t extent = TerrainCompositedVTLayout::StoredExtent;
    switch (format)
    {
    case TerrainCompositedVTFormat::RGBA8UNorm:
    case TerrainCompositedVTFormat::RGBA8Srgb: return extent * extent * 4;
    case TerrainCompositedVTFormat::RG8UNorm:
    case TerrainCompositedVTFormat::R16UNorm: return extent * extent * 2;
    case TerrainCompositedVTFormat::BC5UNorm:
    case TerrainCompositedVTFormat::BC7UNorm:
    case TerrainCompositedVTFormat::BC7Srgb: return (extent / 4) * (extent / 4) * 16;
    }
    throw std::runtime_error("Unsupported VT format");
}

TerrainCompositedVTLayout TerrainCompositedVTBuildDesc::Layout() const
{
    return {surfaceId, width, height, addressMode};
}

uint64_t TerrainCompositedVTBuildDesc::PageBytes() const
{
    Require(!channels.empty() && channels.size() <= 8, "Invalid VT channel count");
    uint64_t size = 0;
    for (auto format : channels)
    {
        size += TerrainCompositedVTFormatBytes(format);
    }
    return size;
}

ContentDigest TerrainCompositedVTBuildDesc::ContentHash() const
{
    Layout();
    PageBytes();
    Require(generatorVersion > 0 && !encoding.empty() && encoding.size() <= 1024 &&
        encoding.find('\0') == std::string::npos, "Invalid VT generator/encoding");
    CanonicalFieldHasher hasher("terrain-composited-vt-v2");
    hasher.AddString("surfaceId", surfaceId);
    hasher.AddUInt32("width", width);
    hasher.AddUInt32("height", height);
    hasher.AddUInt32("innerExtent", TerrainCompositedVTLayout::InnerExtent);
    hasher.AddUInt32("border", TerrainCompositedVTLayout::Border);
    hasher.AddString("mipRule", "floor-half-to-one");
    hasher.AddString("filterRule", TerrainCompositedFilterRule);
    hasher.AddUInt32("addressMode", static_cast<uint32_t>(addressMode));
    hasher.AddDigest("dependencies", dependencyHash);
    hasher.AddUInt32("generatorVersion", generatorVersion);
    hasher.AddString("encoding", encoding);
    hasher.AddUInt32("channelCount", static_cast<uint32_t>(channels.size()));
    for (auto format : channels)
    {
        hasher.AddString("channelFormat", TerrainCompositedVTFormatName(format));
    }
    return hasher.Finalize();
}

bool TerrainCompositedVTTaskToken::Matches(const TerrainCompositedVTTaskToken& current) const
{
    return worldGeneration == current.worldGeneration && revision == current.revision &&
        contentHash == current.contentHash && key == current.key;
}

}
