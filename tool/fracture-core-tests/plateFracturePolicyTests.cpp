#include <gtest/gtest.h>

#include "physics/fracture/destructibleProfile.h"
#include "physics/fracture/plateFracturePolicy.h"

namespace
{
using namespace VL::Physics::Fracture;

Polygon2 Rectangle(double minX, double minY, double maxX, double maxY)
{
    return {{minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}};
}
} // namespace

TEST(PlateFracturePolicy, DamageRadiusGrowsWithSpeedAndClamps)
{
    const DestructibleProfile& profile = DefaultGlassProfile();
    const double slow =
        ComputePlateDamageRadius(120.0, 100.0, 1.0f, profile);
    const double fast =
        ComputePlateDamageRadius(120.0, 100.0, 12.0f, profile);
    EXPECT_GT(fast, slow);
    EXPECT_GE(slow, 120.0 * profile.damageRadiusMinFraction);
    EXPECT_LE(fast, 120.0 * profile.damageRadiusMaxFraction);
}

TEST(PlateFracturePolicy, AttachedThresholdUsesProfileFraction)
{
    const DestructibleProfile& profile = DefaultGlassProfile();
    EXPECT_FALSE(IsPlateShardAttached(10.0, 100.0, profile));
    EXPECT_TRUE(IsPlateShardAttached(60.0, 100.0, profile));
}

TEST(PlateFracturePolicy, ProjectileAndLandingUseDifferentAreaThresholds)
{
    const DestructibleProfile& profile = DefaultGlassProfile();
    EXPECT_TRUE(EvaluateSubfracture(
        10.0, 0, 20.0f, true, profile).split);
    EXPECT_FALSE(EvaluateSubfracture(
        10.0, 0, 20.0f, false, profile).split);
    EXPECT_EQ(EvaluateSubfracture(
        100.0, 0, 20.0f, true, profile).crackCount, 2);
}

TEST(PlateFracturePolicy, SubfractureHonorsDepthSpeedAndAreaLimits)
{
    const DestructibleProfile& profile = DefaultGlassProfile();
    EXPECT_FALSE(EvaluateSubfracture(
        100.0, profile.maxSubfractureDepth, 20.0f, true, profile).split);
    EXPECT_FALSE(EvaluateSubfracture(
        100.0, 0, profile.minSubfractureSpeed * 0.5f, true, profile).split);
    EXPECT_FALSE(EvaluateSubfracture(
        profile.minProjectileSubfractureArea * 0.5, 0, 20.0f,
        true, profile).split);
    EXPECT_TRUE(EvaluateSubfracture(
        profile.minProjectileSubfractureArea, 0, 20.0f,
        true, profile).split);
}

TEST(PlateFracturePolicy, IsolatedAndSmallAnchoredIslandsDrop)
{
    const DestructibleProfile& profile = DefaultGlassProfile();
    const std::vector<Polygon2> outlines = {
        Rectangle(-10.0, -10.0, 10.0, 10.0),  // isolated 20x20
        Rectangle(-50.0, -5.0, -45.0, 5.0)     // anchored but area 100
    };
    const std::vector<double> areas = {400.0, 100.0};
    const std::vector<size_t> dropped = FindUnsupportedPlateIndices(
        outlines, areas, 100.0, 100.0, 1.0, profile);
    EXPECT_EQ(dropped, (std::vector<size_t>{0, 1}));
}

TEST(PlateFracturePolicy, BorderToleranceIsConvertedFromWorldToLocalUnits)
{
    const DestructibleProfile& profile = DefaultGlassProfile();
    const std::vector<Polygon2> outlines = {
        Rectangle(29.7, -10.0, 49.7, 10.0)
    };
    const std::vector<double> areas = {400.0};
    EXPECT_TRUE(FindUnsupportedPlateIndices(
        outlines, areas, 100.0, 100.0, 1.0, profile).empty());
    EXPECT_FALSE(FindUnsupportedPlateIndices(
        outlines, areas, 100.0, 100.0, 2.0, profile).empty());
}
