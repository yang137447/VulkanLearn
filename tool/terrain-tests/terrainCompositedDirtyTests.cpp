#include <gtest/gtest.h>

#include "terrain/terrainCompositedDirty.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using namespace VL;
using Change = TerrainCompositedDependencyChange;
using Edge = TerrainCompositedSurfaceEdge;
using Rect = TerrainCompositedVTUVRect;

void Horizontal(std::vector<TerrainCompositedAdjacencyLink>& links,
    const std::string& first, const std::string& second)
{
    links.push_back({first, second, Edge::MaxU, Edge::MinU,
        {0, 1}, {0, 1}, {1, 0, -1, 0, 1, 0}});
    links.push_back({second, first, Edge::MinU, Edge::MaxU,
        {0, 1}, {0, 1}, {1, 0, 1, 0, 1, 0}});
}

void Vertical(std::vector<TerrainCompositedAdjacencyLink>& links,
    const std::string& first, const std::string& second)
{
    links.push_back({first, second, Edge::MaxV, Edge::MinV,
        {0, 1}, {0, 1}, {1, 0, 0, 0, 1, -1}});
    links.push_back({second, first, Edge::MinV, Edge::MaxV,
        {0, 1}, {0, 1}, {1, 0, 0, 0, 1, 1}});
}

bool Contains(const TerrainCompositedDirtyResult& result,
    const std::string& surface, uint32_t mip, uint32_t horizontal, uint32_t vertical)
{
    return std::binary_search(result.pages.begin(), result.pages.end(),
        TerrainCompositedVTPageKey{surface, mip, horizontal, vertical});
}

TerrainCompositedDirtyPropagation Single(
    uint32_t width = 1024, uint32_t height = 1024,
    TerrainCompositedVTAddressMode mode = TerrainCompositedVTAddressMode::Clamp)
{
    return TerrainCompositedDirtyPropagation(
        TerrainCompositedAdjacencyGraph({"a"}, {}), {{"a", width, height, mode}});
}

bool Inside(const Rect& rect, const std::array<double, 2>& point)
{
    return point[0] >= rect.minU && point[0] <= rect.maxU &&
        point[1] >= rect.minV && point[1] <= rect.maxV;
}
}

TEST(TerrainCompositedDirty, PaintInvalidatesInteriorBorderAndAllMipAncestors)
{
    const auto result = Single().Collect({{"a", Change::Paint, {0.124, 0.30, 0.126, 0.31}}});
    EXPECT_TRUE(Contains(result, "a", 0, 0, 2));
    EXPECT_TRUE(Contains(result, "a", 0, 1, 2));
    EXPECT_FALSE(Contains(result, "a", 0, 4, 2));
    const TerrainCompositedVTLayout layout("a", 1024, 1024);
    for (const auto& page : result.pages)
    {
        EXPECT_TRUE(layout.Contains(page));
        if (page.mip + 1 < layout.MipCount())
        {
            const auto parent = layout.Parent(page);
            EXPECT_TRUE(Contains(result, "a", parent.mip, parent.x, parent.y));
        }
    }
    EXPECT_TRUE(Contains(result, "a", 10, 0, 0));
}

TEST(TerrainCompositedDirty, CoarseIntegrationAddsSupportBeyondBorderAlone)
{
    const auto result = Single().Collect({{"a", Change::Paint,
        {0.2583, 0.10, 0.2585, 0.11}}}, 0);
    EXPECT_FALSE(Contains(result, "a", 0, 1, 0));
    EXPECT_TRUE(Contains(result, "a", 1, 0, 0));
}

TEST(TerrainCompositedDirty, CrossPlacementBorderDoesNotInvalidateDistantFinePages)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "terrain|left", "terrain|right");
    TerrainCompositedDirtyPropagation dirty(
        TerrainCompositedAdjacencyGraph({"terrain|left", "terrain|right", "terrain|detached"}, links),
        {{"terrain|left", 1024, 1024}, {"terrain|right", 1024, 1024}, {"terrain|detached", 1024, 1024}});
    const auto result = dirty.Collect({{"terrain|left", Change::Paint,
        {0.998, 0.30, 1, 0.31}}});
    EXPECT_TRUE(Contains(result, "terrain|right", 0, 0, 2));
    EXPECT_FALSE(Contains(result, "terrain|right", 0, 4, 2));
    EXPECT_TRUE(Contains(result, "terrain|right", 10, 0, 0));
    for (const auto& page : result.pages)
        EXPECT_NE(page.surfaceId, "terrain|detached");
}

TEST(TerrainCompositedDirty, FourWayCornerReachesDiagonalOverTwoExplicitLinks)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Horizontal(links, "c", "d");
    Vertical(links, "a", "c");
    Vertical(links, "b", "d");
    TerrainCompositedDirtyPropagation dirty(
        TerrainCompositedAdjacencyGraph({"a", "b", "c", "d"}, links),
        {{"a", 1024, 1024}, {"b", 1024, 1024}, {"c", 1024, 1024}, {"d", 1024, 1024}});
    const auto result = dirty.Collect({{"a", Change::Sculpt, {0.998, 0.998, 1, 1}}});
    EXPECT_TRUE(Contains(result, "b", 0, 0, 7));
    EXPECT_TRUE(Contains(result, "c", 0, 7, 0));
    EXPECT_TRUE(Contains(result, "d", 0, 0, 0));
    EXPECT_TRUE(Contains(result, "d", 10, 0, 0));
    EXPECT_TRUE(result.surfaces.front().consumers.geometry);
    EXPECT_TRUE(result.surfaces.front().consumers.collision);
    EXPECT_FALSE(result.surfaces.back().consumers.geometry);
}

TEST(TerrainCompositedDirty, CoarseFootprintReachesNonAdjacentSurfaceWithoutFloodingFineMip)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Horizontal(links, "b", "c");
    TerrainCompositedDirtyPropagation dirty(
        TerrainCompositedAdjacencyGraph({"a", "b", "c"}, links),
        {{"a", 1024, 1024}, {"b", 1024, 1024}, {"c", 1024, 1024}});
    const auto result = dirty.Collect({{"a", Change::Paint, {0.4, 0.4, 0.41, 0.41}}});
    EXPECT_TRUE(Contains(result, "c", 10, 0, 0));
    EXPECT_FALSE(Contains(result, "c", 0, 0, 0));
    EXPECT_FALSE(Contains(result, "b", 0, 0, 0));
}

TEST(TerrainCompositedDirty, SplitEdgeMappingUsesTargetResolutionAndScale)
{
    std::vector<TerrainCompositedAdjacencyLink> links{
        {"a", "b", Edge::MaxU, Edge::MinU, {0, 0.5}, {0, 1}, {1, 0, -1, 0, 2, 0}},
        {"b", "a", Edge::MinU, Edge::MaxU, {0, 1}, {0, 0.5}, {1, 0, 1, 0, 0.5, 0}},
        {"a", "c", Edge::MaxU, Edge::MinU, {0.5, 1}, {0, 1}, {1, 0, -1, 0, 2, -1}},
        {"c", "a", Edge::MinU, Edge::MaxU, {0, 1}, {0.5, 1}, {1, 0, 1, 0, 0.5, 0.5}},
        {"b", "c", Edge::MaxV, Edge::MinV, {0, 1}, {0, 1}, {1, 0, 0, 0, 1, -1}},
        {"c", "b", Edge::MinV, Edge::MaxV, {0, 1}, {0, 1}, {1, 0, 0, 0, 1, 1}}};
    TerrainCompositedDirtyPropagation dirty(
        TerrainCompositedAdjacencyGraph({"a", "b", "c"}, links),
        {{"a", 1024, 1024}, {"b", 512, 256}, {"c", 512, 256}});
    const auto result = dirty.Collect({{"a", Change::Paint, {0.999, 0.74, 1, 0.76}}});
    EXPECT_TRUE(Contains(result, "c", 0, 0, 0));
    EXPECT_TRUE(Contains(result, "c", 0, 0, 1));
    EXPECT_FALSE(Contains(result, "b", 0, 0, 0));
    EXPECT_FALSE(Contains(result, "b", 0, 0, 1));
}

TEST(TerrainCompositedDirty, RotatedNeighborMapsBothAxes)
{
    std::vector<TerrainCompositedAdjacencyLink> links{
        {"a", "b", Edge::MaxU, Edge::MinV, {0, 1}, {0, 1}, {0, 1, 0, 1, 0, -1}},
        {"b", "a", Edge::MinV, Edge::MaxU, {0, 1}, {0, 1}, {0, 1, 1, 1, 0, 0}}};
    TerrainCompositedDirtyPropagation dirty(TerrainCompositedAdjacencyGraph({"a", "b"}, links),
        {{"a", 1024, 1024}, {"b", 1024, 1024}});
    const auto result = dirty.Collect({{"a", Change::Paint, {0.999, 0.30, 1, 0.31}}});
    EXPECT_TRUE(Contains(result, "b", 0, 2, 0));
    EXPECT_FALSE(Contains(result, "b", 0, 0, 2));
}

TEST(TerrainCompositedDirty, OddRectangularLayoutInvalidatesEveryOverlappingCoarsePage)
{
    const TerrainCompositedVTLayout layout("a", 777, 515);
    const auto result = Single(777, 515).Collect({{"a", Change::Sculpt,
        {0.49, 0.49, 0.51, 0.51}}});
    EXPECT_TRUE(Contains(result, "a", 1, 1, 0));
    EXPECT_TRUE(Contains(result, "a", 1, 1, 1));
    EXPECT_TRUE(Contains(result, "a", layout.MipCount() - 1, 0, 0));
    for (const auto& page : result.pages)
        EXPECT_TRUE(layout.Contains(page));
}

TEST(TerrainCompositedDirty, InputSupportExpandsFrameAndCoverageDependencyRegion)
{
    TerrainCompositedDirtyChange change{"a", Change::Sculpt, {0.132, 0.30, 0.133, 0.31}};
    const auto narrow = Single().Collect({change}, 0);
    EXPECT_FALSE(Contains(narrow, "a", 0, 0, 2));
    change.inputSupportUv = {0.01, 0.02};
    const auto expanded = Single().Collect({change}, 0);
    EXPECT_TRUE(Contains(expanded, "a", 0, 0, 2));
}

TEST(TerrainCompositedDirty, DynamicChangesNeverInvalidateBasePages)
{
    for (const auto kind : {Change::DynamicDetail, Change::DynamicMacro,
        Change::DynamicGlobal, Change::DynamicTessellation})
    {
        TerrainCompositedDirtyChange change{"a", kind};
        change.staticHeightBlendBefore = true;
        change.staticHeightBlendAfter = true;
        const auto result = Single().Collect({change});
        EXPECT_TRUE(result.pages.empty());
        ASSERT_EQ(result.surfaces.size(), 1u);
        EXPECT_FALSE(result.surfaces.front().consumers.basePages);
        EXPECT_TRUE(result.surfaces.front().consumers.dynamicMaterial);
        EXPECT_FALSE(result.surfaces.front().consumers.tilingHeight);
    }
}

TEST(TerrainCompositedDirty, HeightResourcesInvalidateBaseOnlyForStaticHeightBlendConsumption)
{
    for (const auto kind : {Change::TilingHeight, Change::LayerDisplacement})
    {
        TerrainCompositedDirtyChange change{"a", kind};
        auto result = Single().Collect({change});
        EXPECT_TRUE(result.pages.empty());
        ASSERT_EQ(result.surfaces.size(), 1u);
        EXPECT_EQ(result.surfaces.front().consumers.tilingHeight, kind == Change::TilingHeight);
        for (const auto enabled : {std::array<bool, 2>{true, false},
            std::array<bool, 2>{false, true}, std::array<bool, 2>{true, true}})
        {
            change.staticHeightBlendBefore = enabled[0];
            change.staticHeightBlendAfter = enabled[1];
            result = Single().Collect({change});
            EXPECT_FALSE(result.pages.empty());
        }
    }
}

TEST(TerrainCompositedDirty, StaticDependencyKindsInvalidateBase)
{
    for (const auto kind : {Change::BaseMaterial, Change::BaseSource, Change::LayerMapping,
        Change::WeightSource, Change::Placement, Change::Frame, Change::Filter,
        Change::Generator, Change::HeightBlend})
    {
        const auto result = Single().Collect({{"a", kind}});
        EXPECT_FALSE(result.pages.empty());
        EXPECT_TRUE(Contains(result, "a", 10, 0, 0));
    }
}

TEST(TerrainCompositedDirty, HeightBlendDependencyChangePropagatesToNeighborAncestors)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    const TerrainCompositedDirtyPropagation dirty(TerrainCompositedAdjacencyGraph({"a", "b"}, links),
        {{"a", 1024, 1024}, {"b", 1024, 1024}});
    TerrainCompositedDirtyChange change{"a", Change::TilingHeight, {0.999, 0.3, 1, 0.31}};
    EXPECT_TRUE(dirty.Collect({change}).pages.empty());
    change.staticHeightBlendAfter = true;
    const auto result = dirty.Collect({change});
    EXPECT_TRUE(Contains(result, "b", 0, 0, 2));
    EXPECT_TRUE(Contains(result, "b", 10, 0, 0));
}

TEST(TerrainCompositedDirty, ExternalClampPreservesBoundaryChangesAcrossNeighborFootprints)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    const TerrainCompositedDirtyPropagation dirty(TerrainCompositedAdjacencyGraph({"a", "b"}, links),
        {{"a", 1024, 1024}, {"b", 1024, 1024}});
    const auto result = dirty.Collect({{"a", Change::Paint, {0, 0, 0.01, 0.01}}});
    EXPECT_TRUE(Contains(result, "a", 0, 0, 0));
    EXPECT_TRUE(Contains(result, "b", 10, 0, 0));
    EXPECT_FALSE(Contains(result, "b", 0, 0, 0));
}

TEST(TerrainCompositedDirty, DuplicateAndReorderedChangesHaveIdenticalSortedOutput)
{
    const TerrainCompositedDirtyChange first{"a", Change::Paint, {0.1, 0.2, 0.2, 0.3}};
    const TerrainCompositedDirtyChange second{"a", Change::Sculpt, {0.7, 0.8, 0.8, 0.9}};
    const auto dirty = Single();
    const auto once = dirty.Collect({first, second});
    const auto repeated = dirty.Collect({second, first, first, second});
    EXPECT_EQ(once.pages, repeated.pages);
    EXPECT_TRUE(std::is_sorted(once.pages.begin(), once.pages.end()));
    EXPECT_EQ(std::adjacent_find(once.pages.begin(), once.pages.end()), once.pages.end());
    ASSERT_EQ(repeated.surfaces.size(), 1u);
    EXPECT_TRUE(repeated.surfaces.front().consumers.geometry);
    EXPECT_TRUE(repeated.surfaces.front().consumers.collision);
}

TEST(TerrainCompositedDirty, EmptyAndOutsideChangesDoNotInvalidateClampSurface)
{
    const auto dirty = Single();
    EXPECT_TRUE(dirty.Collect({}).pages.empty());
    const auto result = dirty.Collect({
        {"a", Change::Paint, {0.2, 0.2, 0.2, 0.3}},
        {"a", Change::Paint, {-0.3, 0.2, -0.1, 0.3}},
        {"a", Change::Sculpt, {1.1, 0.2, 1.3, 0.3}}});
    EXPECT_TRUE(result.pages.empty());
    EXPECT_TRUE(result.surfaces.empty());
}

TEST(TerrainCompositedDirty, IsolatedRepeatInvalidatesWrappedBorder)
{
    const auto result = Single(1024, 1024, TerrainCompositedVTAddressMode::Repeat)
        .Collect({{"a", Change::Paint, {0.999, 0.30, 1, 0.31}}});
    EXPECT_TRUE(Contains(result, "a", 0, 0, 2));
    EXPECT_TRUE(Contains(result, "a", 0, 7, 2));
    EXPECT_FALSE(Contains(result, "a", 0, 4, 2));
}

TEST(TerrainCompositedDirty, RejectsUnregisteredLayoutsAndRepeatAdjacency)
{
    EXPECT_THROW((TerrainCompositedDirtyPropagation(
        TerrainCompositedAdjacencyGraph({"a"}, {}), {{"b", 256, 256}})), std::exception);
    EXPECT_THROW((TerrainCompositedDirtyPropagation(
        TerrainCompositedAdjacencyGraph({"a"}, {}), {{"a", 256, 256}, {"a", 256, 256}})), std::exception);
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    EXPECT_THROW((TerrainCompositedDirtyPropagation(
        TerrainCompositedAdjacencyGraph({"a", "b"}, links),
        {{"a", 256, 256, TerrainCompositedVTAddressMode::Repeat}, {"b", 256, 256}})), std::exception);
}

TEST(TerrainCompositedDirty, RejectsInvalidDirtyInput)
{
    const auto dirty = Single();
    EXPECT_THROW(dirty.Collect({{"missing", Change::Paint}}), std::exception);
    EXPECT_THROW(dirty.Collect({{"a", Change::Paint, {0.5, 0, 0.4, 1}}}), std::exception);
    EXPECT_THROW(dirty.Collect({{"a", Change::Paint,
        {0, 0, std::numeric_limits<double>::quiet_NaN(), 1}}}), std::exception);
    EXPECT_THROW(dirty.Collect({}, -1), std::exception);
    EXPECT_THROW(dirty.Collect({}, std::numeric_limits<double>::infinity()), std::exception);
    TerrainCompositedDirtyChange change{"a", Change::Sculpt};
    change.inputSupportUv[0] = -0.1;
    EXPECT_THROW(dirty.Collect({change}), std::exception);
    change.inputSupportUv = {};
    change.kind = static_cast<Change>(999);
    EXPECT_THROW(dirty.Collect({change}), std::exception);
}

TEST(TerrainCompositedDirty, SampledAdjacencyReadFootprintsAreConservativelyInvalidated)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Horizontal(links, "c", "d");
    Vertical(links, "a", "c");
    Vertical(links, "b", "d");
    const TerrainCompositedAdjacencyGraph graph({"a", "b", "c", "d"}, links);
    const std::vector<TerrainCompositedVTLayout> layouts{
        {"a", 257, 259}, {"b", 513, 255}, {"c", 129, 257}, {"d", 255, 513}};
    const TerrainCompositedDirtyPropagation dirty(graph, layouts);
    const std::vector<Rect> regions{{0.998, 0.998, 1, 1}, {0, 0, 0.05, 0.05},
        {0.45, 0.2, 0.55, 0.3}, {0.98, 0, 1, 0.1}};
    for (const auto& source : graph.SurfaceIds())
        for (const auto& region : regions)
        {
            const auto result = dirty.Collect({{source, Change::Paint, region}});
            for (const auto& layout : layouts)
                for (uint32_t mipIndex = 0; mipIndex < layout.MipCount(); ++mipIndex)
                {
                    const auto& mip = layout.Mip(mipIndex);
                    for (uint32_t pageY = 0; pageY < mip.pagesY; ++pageY)
                        for (uint32_t pageX = 0; pageX < mip.pagesX; ++pageX)
                        {
                            const TerrainCompositedVTPageKey page{layout.SurfaceId(), mipIndex, pageX, pageY};
                            const auto extent = layout.ValidExtent(page);
                            bool readsDirty = false;
                            for (uint32_t sampleY = 0; sampleY <= 8 && !readsDirty; ++sampleY)
                                for (uint32_t sampleX = 0; sampleX <= 8 && !readsDirty; ++sampleX)
                                {
                                    const double horizontal = (double(pageX * layout.InnerExtent) - 4 +
                                        sampleX * (extent.width + 8.) / 8.) / mip.width;
                                    const double vertical = (double(pageY * layout.InnerExtent) - 4 +
                                        sampleY * (extent.height + 8.) / 8.) / mip.height;
                                    const auto resolved = graph.Resolve(layout.SurfaceId(), {horizontal, vertical});
                                    readsDirty = resolved.surfaceId == source && Inside(region, resolved.uv);
                                }
                            if (readsDirty)
                            {
                                EXPECT_TRUE(Contains(result, layout.SurfaceId(), mipIndex, pageX, pageY))
                                    << source << " -> " << layout.SurfaceId() << " mip=" << mipIndex
                                    << " page=" << pageX << "," << pageY;
                            }
                        }
                }
        }
}
