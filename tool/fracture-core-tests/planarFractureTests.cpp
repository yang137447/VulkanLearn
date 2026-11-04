#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "physics/fracture/planarFracture.h"

namespace
{
using namespace VL::Physics::Fracture;

Polygon2 Rectangle(double width, double height)
{
    return {
        {0.0, 0.0},
        {width, 0.0},
        {width, height},
        {0.0, height}};
}

double TotalArea(const PlanarPattern& pattern)
{
    double area = 0.0;
    for (const PlanarShard& shard : pattern.shards) area += shard.area;
    return area;
}

bool SamePolygon(const Polygon2& first, const Polygon2& second, double epsilon)
{
    if (first.size() != second.size()) return false;
    for (size_t index = 0; index < first.size(); ++index)
        if ((first[index] - second[index]).norm() > epsilon) return false;
    return true;
}

void ExpectConvex(const Polygon2& polygon)
{
    ASSERT_GE(polygon.size(), 3u);
    double sign = 0.0;
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Point2 first = polygon[(index + 1) % polygon.size()] - polygon[index];
        const Point2 second = polygon[(index + 2) % polygon.size()] -
            polygon[(index + 1) % polygon.size()];
        const double cross = first.x() * second.y() - first.y() * second.x();
        if (std::abs(cross) <= 1e-12) continue;
        if (sign == 0.0) sign = cross;
        EXPECT_GT(cross * sign, 0.0);
    }
}

} // namespace

TEST(PlanarFracture, HalfPlaneClipPreservesCCWOrderAndArea)
{
    const Polygon2 square = Rectangle(10.0, 10.0);
    const Polygon2 clipped = ClipPolygonByHalfPlane(
        square, Point2(1.0, 0.0), 5.0);
    ASSERT_EQ(clipped.size(), 4u);
    EXPECT_NEAR(SignedArea(clipped), 50.0, 1e-12);
    EXPECT_TRUE(IsConvex(clipped));
}

TEST(PlanarFracture, ImpactPatternCoversConvexDomainWithoutHoles)
{
    const Polygon2 domain = Rectangle(120.0, 80.0);
    PlanarFractureSettings settings;
    settings.seed = 42;
    settings.siteCount = 32;
    const PlanarPattern pattern = GenerateImpactPattern(
        domain, Point2(73.0, 31.0), settings);

    ASSERT_GE(pattern.shards.size(), 8u);
    EXPECT_NEAR(TotalArea(pattern), 120.0 * 80.0, 1e-8);
    EXPECT_NEAR(pattern.coveredArea, TotalArea(pattern), 1e-12);
    for (const PlanarShard& shard : pattern.shards)
    {
        ExpectConvex(shard.outline);
        EXPECT_GT(shard.area, 0.0);
        EXPECT_TRUE((shard.centroid.array() >= 0.0).all());
        EXPECT_LE(shard.centroid.x(), 120.0);
        EXPECT_LE(shard.centroid.y(), 80.0);
    }
}

TEST(PlanarFracture, SameSeedProducesIdenticalLayout)
{
    const Polygon2 domain = Rectangle(40.0, 30.0);
    PlanarFractureSettings settings;
    settings.seed = 7;
    settings.siteCount = 16;
    settings.radialBias = 0.4;
    const PlanarPattern first = GenerateImpactPattern(domain, Point2(12.0, 9.0), settings);
    const PlanarPattern second = GenerateImpactPattern(domain, Point2(12.0, 9.0), settings);

    ASSERT_EQ(first.shards.size(), second.shards.size());
    for (size_t index = 0; index < first.shards.size(); ++index)
    {
        EXPECT_TRUE(SamePolygon(
            first.shards[index].outline,
            second.shards[index].outline,
            1e-12));
        EXPECT_NEAR(first.shards[index].area, second.shards[index].area, 1e-12);
    }
}

TEST(PlanarFracture, DifferentSeedChangesTheLayout)
{
    const Polygon2 domain = Rectangle(40.0, 30.0);
    PlanarFractureSettings settings;
    settings.seed = 7;
    settings.siteCount = 16;
    settings.radialBias = 0.0;
    const PlanarPattern first = GenerateImpactPattern(domain, Point2(20.0, 15.0), settings);
    settings.seed = 8;
    const PlanarPattern second = GenerateImpactPattern(domain, Point2(20.0, 15.0), settings);

    bool different = first.shards.size() != second.shards.size();
    if (!different)
        for (size_t index = 0; index < first.shards.size(); ++index)
            if (!SamePolygon(first.shards[index].outline, second.shards[index].outline, 1e-9))
            {
                different = true;
                break;
            }
    EXPECT_TRUE(different);
}

TEST(PlanarFracture, RadialPatternCoversDomainWithSharpSizeGradient)
{
    const Polygon2 domain = Rectangle(160.0, 100.0);
    RadialFractureSettings settings;
    settings.seed = 21;
    settings.radialCracks = 13;
    settings.ringCount = 2;
    settings.coreRadius = 8.0;
    const Point2 impact(20.0, 80.0);
    const PlanarPattern pattern =
        GenerateRadialImpactPattern(domain, impact, settings);

    ASSERT_GE(pattern.shards.size(), 20u);
    EXPECT_NEAR(TotalArea(pattern), 160.0 * 100.0, 1e-5);
    for (const PlanarShard& shard : pattern.shards)
    {
        EXPECT_GT(shard.area, 0.0);
        EXPECT_NEAR(shard.area, std::abs(SignedArea(shard.outline)), 1e-9);
        ExpectConvex(shard.outline);
    }

    // 冲击点附近必须明显更碎、最外侧的板明显更大；这是径向裂纹图案与
    // “接近等大的各向同性 Voronoi”区分开的核心特征。
    double nearArea = std::numeric_limits<double>::max();
    double largestArea = 0.0;
    for (const PlanarShard& shard : pattern.shards)
    {
        if ((shard.centroid - impact).norm() < 22.0)
            nearArea = std::min(nearArea, shard.area);
        largestArea = std::max(largestArea, shard.area);
    }
    const double average = TotalArea(pattern) /
        static_cast<double>(pattern.shards.size());
    ASSERT_LT(nearArea, average * 0.5);
    EXPECT_GT(largestArea, average * 2.0);
}

TEST(PlanarFracture, RadialPatternCoversArbitraryConvexPlate)
{
    // 后续命中的目标是被第一刀切出来的楔形板片，不是矩形。
    const Polygon2 wedge{{0.0, 0.0}, {60.0, 0.0}, {10.0, 40.0}};
    const double domainArea = SignedArea(wedge);
    ASSERT_GT(domainArea, 0.0);
    RadialFractureSettings settings;
    settings.seed = 17;
    settings.radialCracks = 5;
    settings.ringCount = 1;
    settings.coreRadius = 4.0;
    const PlanarPattern pattern = GenerateRadialImpactPattern(
        wedge, {20.0, 12.0}, settings);
    ASSERT_GE(pattern.shards.size(), 4u);
    EXPECT_NEAR(TotalArea(pattern), domainArea, domainArea * 1e-9);
    for (const PlanarShard& shard : pattern.shards)
    {
        EXPECT_GT(shard.area, 0.0);
        ExpectConvex(shard.outline);
    }
}

TEST(PlanarFracture, RadialPatternIsDeterministic)
{
    const Polygon2 domain = Rectangle(160.0, 100.0);
    RadialFractureSettings settings;
    settings.seed = 33;
    settings.radialCracks = 11;
    settings.ringCount = 3;
    const PlanarPattern first =
        GenerateRadialImpactPattern(domain, {70.0, 40.0}, settings);
    const PlanarPattern second =
        GenerateRadialImpactPattern(domain, {70.0, 40.0}, settings);
    ASSERT_EQ(first.shards.size(), second.shards.size());
    for (size_t index = 0; index < first.shards.size(); ++index)
    {
        EXPECT_TRUE(SamePolygon(
            first.shards[index].outline, second.shards[index].outline, 1e-12));
        EXPECT_NEAR(first.shards[index].area, second.shards[index].area, 1e-12);
    }
}

TEST(PlanarFracture, CrackSplitProducesSharpPiecesWithoutAreaLoss)
{
    const Polygon2 domain = Rectangle(100.0, 80.0);
    PlanarFractureSettings settings;
    settings.seed = 5;
    settings.siteCount = 8;
    const PlanarPattern parent = GenerateImpactPattern(domain, {50.0, 40.0}, settings);
    const PlanarShard* largest = &parent.shards.front();
    for (const PlanarShard& shard : parent.shards)
        if (shard.area > largest->area) largest = &shard;

    const PlanarPattern split = SplitPlanarShardByCracks(*largest, 2, 99);
    ASSERT_EQ(split.shards.size(), 3u);
    EXPECT_NEAR(TotalArea(split), largest->area, 1e-9);
    for (const PlanarShard& shard : split.shards)
    {
        EXPECT_GT(shard.area, 0.0);
        ExpectConvex(shard.outline);
    }
}

TEST(PlanarFracture, SplitShardPreservesParentArea)
{
    const Polygon2 domain = Rectangle(60.0, 40.0);
    PlanarFractureSettings initial;
    initial.seed = 11;
    initial.siteCount = 4;
    initial.radialBias = 0.0;
    const PlanarPattern pattern = GenerateImpactPattern(domain, Point2(30.0, 20.0), initial);
    ASSERT_FALSE(pattern.shards.empty());

    const PlanarShard& parent = pattern.shards.front();
    PlanarFractureSettings split;
    split.seed = 99;
    split.siteCount = 9;
    split.radialBias = 0.8;
    const PlanarPattern children = SplitPlanarShard(parent, split);
    ASSERT_GE(children.shards.size(), 2u);
    EXPECT_NEAR(TotalArea(children), parent.area, 1e-8);
    for (const PlanarShard& child : children.shards)
        ExpectConvex(child.outline);
}

TEST(PlanarFracture, DegenerateDomainAndSiteCountAreRejected)
{
    PlanarFractureSettings settings;
    EXPECT_THROW(GenerateImpactPattern({{0, 0}, {1, 0}}, Point2::Zero(), settings),
        std::invalid_argument);
    EXPECT_THROW(GenerateImpactPattern({{0, 0}, {2, 0}, {0, 0}}, Point2::Zero(), settings),
        std::invalid_argument);

    PlanarFractureSettings clockwise;
    clockwise.siteCount = 2;
    EXPECT_THROW(GenerateImpactPattern(
        {{0, 0}, {0, 10}, {10, 10}, {10, 0}}, Point2(5.0, 5.0), clockwise),
        std::invalid_argument);
}

TEST(PlanarFracture, ImpactOutsideDomainIsProjectedInside)
{
    const Polygon2 domain = Rectangle(20.0, 10.0);
    PlanarFractureSettings settings;
    settings.seed = 3;
    settings.siteCount = 8;
    settings.radialBias = 0.5;
    const PlanarPattern pattern = GenerateImpactPattern(
        domain, Point2(-100.0, 5.0), settings);
    EXPECT_NEAR(TotalArea(pattern), 200.0, 1e-8);
}
