#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VL
{
struct TerrainHeightAtlasSourceLayout
{
    uint32_t sourceSet = 0;
    uint32_t mipWidth = 0;
    uint32_t mipHeight = 0;
    uint32_t arrayLayers = 0;
};

struct TerrainHeightAtlasLayer
{
    uint32_t globalLayer = 0;
    uint32_t sourceSet = 0;
    uint32_t sourceSlice = 0;
};

struct TerrainHeightAtlasPage
{
    uint32_t sourceSet = 0;
    uint32_t sourceSlice = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t pageX = 0;
    uint32_t pageY = 0;
};

struct TerrainHeightAtlasCopy
{
    uint32_t sourceX = 0;
    uint32_t sourceY = 0;
    uint32_t destinationX = 0;
    uint32_t destinationY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct TerrainHeightAtlasLayout
{
    static constexpr uint32_t SourceMip = 2;
    static constexpr uint32_t Interior = 128;
    static constexpr uint32_t Border = 4;
    static constexpr uint32_t PageExtent = Interior + 2 * Border;
    static constexpr uint32_t MaxExtent = 4096;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t columns = 0;
    uint64_t atlasBytes = 0;
    uint64_t layerMapBytes = 0;
    uint64_t totalBytes = 0;
    std::vector<TerrainHeightAtlasPage> pages;
    std::vector<std::array<float, 4>> layerMap;
};

inline TerrainHeightAtlasLayout BuildTerrainHeightAtlasLayout(
    const std::vector<TerrainHeightAtlasSourceLayout>& sources,
    const std::vector<TerrainHeightAtlasLayer>& layers,
    uint64_t budgetBytes)
{
    using Layout = TerrainHeightAtlasLayout;
    if (layers.empty())
        throw std::invalid_argument("Terrain height atlas requires mapped layers");
    std::map<uint32_t, TerrainHeightAtlasSourceLayout> sourceTable;
    for (const auto& source : sources)
    {
        if (source.mipWidth == 0 || source.mipHeight == 0 ||
            source.arrayLayers == 0 ||
            source.mipWidth > Layout::MaxExtent ||
            source.mipHeight > Layout::MaxExtent ||
            !sourceTable.emplace(source.sourceSet, source).second)
            throw std::invalid_argument("Invalid terrain height atlas source");
    }
    uint32_t layerCount = 0;
    for (const auto& layer : layers)
    {
        if (layer.globalLayer >= Layout::MaxExtent)
            throw std::invalid_argument("Terrain height atlas layer LUT exceeds 4096");
        layerCount = std::max(layerCount, layer.globalLayer + 1);
    }
    Layout result;
    result.layerMap.resize(layerCount, {0, 0, 0, 0});
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> firstPages;
    constexpr uint32_t MaxColumns = Layout::MaxExtent / Layout::PageExtent;
    for (const auto& layer : layers)
    {
        const auto sourceIt = sourceTable.find(layer.sourceSet);
        if (sourceIt == sourceTable.end() ||
            layer.sourceSlice >= sourceIt->second.arrayLayers)
            throw std::invalid_argument("Terrain height atlas references missing source/slice");
        if (result.layerMap[layer.globalLayer][2] != 0)
            throw std::invalid_argument("Terrain height atlas has duplicate global layer");
        const auto& source = sourceIt->second;
        const uint32_t pagesX = (source.mipWidth + Layout::Interior - 1) / Layout::Interior;
        const uint32_t pagesY = (source.mipHeight + Layout::Interior - 1) / Layout::Interior;
        const auto identity = std::make_pair(layer.sourceSet, layer.sourceSlice);
        auto first = firstPages.find(identity);
        if (first == firstPages.end())
        {
            if (result.pages.size() + uint64_t(pagesX) * pagesY >
                uint64_t(MaxColumns) * MaxColumns)
                throw std::invalid_argument("Terrain height atlas exceeds fixed 4096 extent");
            first = firstPages.emplace(identity, static_cast<uint32_t>(result.pages.size())).first;
            for (uint32_t pageY = 0; pageY < pagesY; ++pageY)
                for (uint32_t pageX = 0; pageX < pagesX; ++pageX)
                    result.pages.push_back({layer.sourceSet, layer.sourceSlice,
                        source.mipWidth, source.mipHeight, pageX, pageY});
        }
        result.layerMap[layer.globalLayer] = {
            static_cast<float>(first->second), static_cast<float>(pagesX),
            static_cast<float>(source.mipWidth), static_cast<float>(source.mipHeight)};
    }
    const uint32_t pageCount = static_cast<uint32_t>(result.pages.size());
    result.columns = 1;
    while (result.columns * result.columns < pageCount)
        ++result.columns;
    result.width = result.columns * Layout::PageExtent;
    result.height = ((pageCount + result.columns - 1) / result.columns) * Layout::PageExtent;
    result.atlasBytes = uint64_t(result.width) * result.height * 2;
    result.layerMapBytes = uint64_t(layerCount) * 16;
    result.totalBytes = result.atlasBytes + result.layerMapBytes;
    if (result.totalBytes > budgetBytes)
        throw std::invalid_argument("Terrain height atlas and LUT exceed resident payload budget");
    return result;
}

inline std::vector<TerrainHeightAtlasCopy> BuildTerrainHeightAtlasCopies(
    const TerrainHeightAtlasLayout& layout, uint32_t pageIndex)
{
    using Layout = TerrainHeightAtlasLayout;
    const auto& page = layout.pages.at(pageIndex);
    const uint32_t destinationBaseX = (pageIndex % layout.columns) * Layout::PageExtent;
    const uint32_t destinationBaseY = (pageIndex / layout.columns) * Layout::PageExtent;
    const uint32_t startX = (page.pageX * Layout::Interior + page.sourceWidth -
        Layout::Border % page.sourceWidth) % page.sourceWidth;
    const uint32_t startY = (page.pageY * Layout::Interior + page.sourceHeight -
        Layout::Border % page.sourceHeight) % page.sourceHeight;
    std::vector<TerrainHeightAtlasCopy> copies;
    for (uint32_t offsetY = 0; offsetY < Layout::PageExtent;)
    {
        const uint32_t sourceY = (startY + offsetY) % page.sourceHeight;
        const uint32_t height = std::min(
            Layout::PageExtent - offsetY, page.sourceHeight - sourceY);
        for (uint32_t offsetX = 0; offsetX < Layout::PageExtent;)
        {
            const uint32_t sourceX = (startX + offsetX) % page.sourceWidth;
            const uint32_t width = std::min(
                Layout::PageExtent - offsetX, page.sourceWidth - sourceX);
            copies.push_back({sourceX, sourceY, destinationBaseX + offsetX,
                destinationBaseY + offsetY, width, height});
            offsetX += width;
        }
        offsetY += height;
    }
    return copies;
}
}
