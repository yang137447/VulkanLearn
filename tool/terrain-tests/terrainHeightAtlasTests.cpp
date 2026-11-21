#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "terrain/terrainHeightAtlasLayout.h"

namespace
{
using Layout = VL::TerrainHeightAtlasLayout;
constexpr uint64_t Unlimited = std::numeric_limits<uint64_t>::max();

uint16_t SourceValue(uint32_t sourceSet, uint32_t slice, uint32_t pixelX, uint32_t pixelY)
{
    return static_cast<uint16_t>(
        (sourceSet * 5279 + slice * 1103 + pixelX * 251 + pixelY * 937) % 65536);
}

std::vector<uint16_t> PopulateAtlas(const Layout& layout)
{
    std::vector<uint16_t> pixels(static_cast<size_t>(layout.width) * layout.height);
    for (uint32_t pageIndex = 0; pageIndex < layout.pages.size(); ++pageIndex)
    {
        const auto& page = layout.pages[pageIndex];
        for (const auto& copy : VL::BuildTerrainHeightAtlasCopies(layout, pageIndex))
            for (uint32_t offsetY = 0; offsetY < copy.height; ++offsetY)
                for (uint32_t offsetX = 0; offsetX < copy.width; ++offsetX)
                    pixels[(copy.destinationY + offsetY) * layout.width +
                        copy.destinationX + offsetX] = SourceValue(page.sourceSet,
                            page.sourceSlice, copy.sourceX + offsetX, copy.sourceY + offsetY);
    }
    return pixels;
}

template<class Fetch>
double Bilinear(double positionX, double positionY, Fetch fetch)
{
    const int baseX = static_cast<int>(std::floor(positionX - 0.5));
    const int baseY = static_cast<int>(std::floor(positionY - 0.5));
    const double weightX = positionX - 0.5 - baseX;
    const double weightY = positionY - 0.5 - baseY;
    return (1 - weightY) * ((1 - weightX) * fetch(baseX, baseY) +
        weightX * fetch(baseX + 1, baseY)) +
        weightY * ((1 - weightX) * fetch(baseX, baseY + 1) +
            weightX * fetch(baseX + 1, baseY + 1));
}
}

TEST(TerrainHeightAtlas, CountsR16BordersPaddingAndSparseLutInBudget)
{
    const auto layout = VL::BuildTerrainHeightAtlasLayout(
        {{7, 512, 512, 4}}, {{9, 7, 3}}, Unlimited);
    EXPECT_EQ(layout.pages.size(), 16u);
    EXPECT_EQ(layout.width, 544u);
    EXPECT_EQ(layout.height, 544u);
    EXPECT_EQ(layout.atlasBytes, 544ull * 544 * 2);
    EXPECT_EQ(layout.layerMapBytes, 10u * 16);
    EXPECT_EQ(layout.totalBytes, layout.atlasBytes + layout.layerMapBytes);
    EXPECT_EQ(layout.layerMap[0], (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(layout.layerMap[9], (std::array<float, 4>{0, 4, 512, 512}));
    EXPECT_NO_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 512, 512, 4}}, {{9, 7, 3}}, layout.totalBytes));
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 512, 512, 4}}, {{9, 7, 3}}, layout.totalBytes - 1), std::invalid_argument);
}

TEST(TerrainHeightAtlas, DeduplicatesSourceSlicesWithoutConfusingGlobalLayersOrSourceSets)
{
    const auto layout = VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 4}, {8, 128, 128, 4}},
        {{0, 7, 3}, {5, 7, 3}, {6, 7, 1}, {7, 8, 3}}, Unlimited);
    EXPECT_EQ(layout.pages.size(), 3u);
    EXPECT_EQ(layout.layerMap[0], layout.layerMap[5]);
    EXPECT_EQ(layout.layerMap[6][0], 1);
    EXPECT_EQ(layout.layerMap[7][0], 2);
    EXPECT_EQ(layout.pages[0].sourceSlice, 3u);
    EXPECT_EQ(layout.pages[2].sourceSet, 8u);
    EXPECT_EQ(layout.atlasBytes, 272ull * 272 * 2);
}

TEST(TerrainHeightAtlas, RejectsMissingSourceAndOutOfRangeSlice)
{
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {}, {{0, 7, 0}}, Unlimited), std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 1}}, {{0, 7, 1}}, Unlimited), std::invalid_argument);
}

TEST(TerrainHeightAtlas, RejectsAmbiguousAndInvalidLayoutInputs)
{
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 1}}, {}, Unlimited), std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 1}, {7, 128, 128, 1}}, {{0, 7, 0}}, Unlimited),
        std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 0, 128, 1}}, {{0, 7, 0}}, Unlimited), std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 0}}, {{0, 7, 0}}, Unlimited), std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 1}}, {{0, 7, 0}, {0, 7, 0}}, Unlimited),
        std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 128, 128, 1}}, {{4096, 7, 0}}, Unlimited), std::invalid_argument);
}

TEST(TerrainHeightAtlas, RejectsFixedAtlasOverflowBeforeAllocation)
{
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 4096, 4096, 1}}, {{0, 7, 0}}, Unlimited), std::invalid_argument);
    EXPECT_THROW(VL::BuildTerrainHeightAtlasLayout(
        {{7, 4097, 128, 1}}, {{0, 7, 0}}, Unlimited), std::invalid_argument);
}

TEST(TerrainHeightAtlas, WrapsEveryBorderAndCornerForFullPartialAndTinySources)
{
    for (const auto size : {std::pair<uint32_t, uint32_t>{256, 128},
        {259, 137}, {3, 2}, {1, 1}})
    {
        const auto layout = VL::BuildTerrainHeightAtlasLayout(
            {{7, size.first, size.second, 2}}, {{0, 7, 1}}, Unlimited);
        const auto pixels = PopulateAtlas(layout);
        for (uint32_t pageIndex = 0; pageIndex < layout.pages.size(); ++pageIndex)
        {
            const auto& page = layout.pages[pageIndex];
            std::vector<uint32_t> writes(Layout::PageExtent * Layout::PageExtent, 0);
            const uint32_t baseX = (pageIndex % layout.columns) * Layout::PageExtent;
            const uint32_t baseY = (pageIndex / layout.columns) * Layout::PageExtent;
            for (const auto& copy : VL::BuildTerrainHeightAtlasCopies(layout, pageIndex))
            {
                ASSERT_LE(copy.sourceX + copy.width, size.first);
                ASSERT_LE(copy.sourceY + copy.height, size.second);
                for (uint32_t offsetY = 0; offsetY < copy.height; ++offsetY)
                    for (uint32_t offsetX = 0; offsetX < copy.width; ++offsetX)
                        ++writes[(copy.destinationY + offsetY - baseY) * Layout::PageExtent +
                            copy.destinationX + offsetX - baseX];
            }
            for (uint32_t localY = 0; localY < Layout::PageExtent; ++localY)
                for (uint32_t localX = 0; localX < Layout::PageExtent; ++localX)
                {
                    const uint32_t sourceX = (page.pageX * 128 + localX +
                        4 * size.first - 4) % size.first;
                    const uint32_t sourceY = (page.pageY * 128 + localY +
                        4 * size.second - 4) % size.second;
                    ASSERT_EQ(writes[localY * Layout::PageExtent + localX], 1u);
                    ASSERT_EQ(pixels[(baseY + localY) * layout.width + baseX + localX],
                        SourceValue(7, 1, sourceX, sourceY));
                }
        }
    }
}

TEST(TerrainHeightAtlas, LodZeroBilinearMatchesSourceMipRepeatAcrossSeamsAndNegativeUV)
{
    const auto layout = VL::BuildTerrainHeightAtlasLayout(
        {{7, 259, 137, 4}, {8, 128, 256, 2}},
        {{3, 7, 2}, {4, 8, 1}}, Unlimited);
    const auto pixels = PopulateAtlas(layout);
    const std::vector<double> coordinates = {
        -2.001, -1, -0.0001, 0, 0.0001, 0.25, 0.5, 0.9999, 1, 2.001,
        (128.0 - 0.25) / 259, 128.0 / 259, (128.0 + 0.25) / 259,
        (256.0 - 0.25) / 259, 256.0 / 259};
    for (const auto layer : {3u, 4u})
        for (const double tilingU : coordinates)
            for (const double tilingV : coordinates)
            {
                const auto& mapping = layout.layerMap[layer];
                const int sourceWidth = static_cast<int>(mapping[2]);
                const int sourceHeight = static_cast<int>(mapping[3]);
                const double sourceX = (tilingU - std::floor(tilingU)) * sourceWidth;
                const double sourceY = (tilingV - std::floor(tilingV)) * sourceHeight;
                const uint32_t pageX = static_cast<uint32_t>(sourceX / 128);
                const uint32_t pageY = static_cast<uint32_t>(sourceY / 128);
                const uint32_t physical = static_cast<uint32_t>(mapping[0]) +
                    pageY * static_cast<uint32_t>(mapping[1]) + pageX;
                const double atlasX = (physical % layout.columns) * 136 +
                    4 + sourceX - pageX * 128;
                const double atlasY = (physical / layout.columns) * 136 +
                    4 + sourceY - pageY * 128;
                const double actual = Bilinear(atlasX, atlasY, [&](int pixelX, int pixelY)
                {
                    return pixels[pixelY * layout.width + pixelX];
                });
                const double expected = Bilinear(sourceX, sourceY, [&](int pixelX, int pixelY)
                {
                    return SourceValue(layer == 3 ? 7 : 8, layer == 3 ? 2 : 1,
                        (pixelX % sourceWidth + sourceWidth) % sourceWidth,
                        (pixelY % sourceHeight + sourceHeight) % sourceHeight);
                });
                EXPECT_NEAR(actual / 65535, expected / 65535, 1e-12);
            }
}
