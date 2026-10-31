#include <gtest/gtest.h>

#include <limits>

#include "physics/fracture/glassDamagePolicy.h"

namespace
{
using namespace VL::Physics::Fracture;

} // namespace

TEST(GlassDamagePolicy, SingleLowEnergyImpactOnlyCracks)
{
    GlassDamagePolicy policy;
    const GlassHitDecision decision = policy.RegisterImpact(3.0f);

    EXPECT_EQ(decision.outcome, GlassHitOutcome::Crack);
    EXPECT_FALSE(policy.IsBroken());
    EXPECT_GT(policy.AccumulatedEnergy(), 0.0f);
    EXPECT_LT(policy.AccumulatedEnergy(), policy.Settings().breakEnergy);
    // 开裂图案从一开始就存在，但小于完整破碎范围。
    EXPECT_LT(decision.crackRadiusScale, policy.Settings().maxCrackRadiusScale);
    EXPECT_GE(decision.radialCracks, policy.Settings().minRadialCracks);
}

TEST(GlassDamagePolicy, AccumulatedLowEnergyImpactsFractureAtThreshold)
{
    GlassDamagePolicy policy;
    int cracks = 0;
    GlassHitDecision decision;
    for (int hit = 0; hit < 8; ++hit)
    {
        decision = policy.RegisterImpact(3.8f);
        if (decision.outcome == GlassHitOutcome::Fracture) break;
        EXPECT_EQ(decision.outcome, GlassHitOutcome::Crack);
        ++cracks;
    }

    // 接近阈值的低能量命中约 3 次达到 1.0 归一化能量。
    EXPECT_EQ(cracks, 2);
    EXPECT_EQ(decision.outcome, GlassHitOutcome::Fracture);
    EXPECT_TRUE(policy.IsBroken());
    EXPECT_FLOAT_EQ(policy.AccumulatedEnergy(), policy.Settings().breakEnergy);
    EXPECT_FLOAT_EQ(decision.normalizedEnergy, 1.0f);
}

TEST(GlassDamagePolicy, StrongImpactFracturesImmediately)
{
    GlassDamagePolicy policy;
    const GlassHitDecision decision = policy.RegisterImpact(8.0f);

    EXPECT_EQ(decision.outcome, GlassHitOutcome::Fracture);
    EXPECT_TRUE(policy.IsBroken());
    EXPECT_FLOAT_EQ(decision.addedEnergy, policy.Settings().breakEnergy);
    EXPECT_FLOAT_EQ(decision.crackRadiusScale,
        policy.Settings().maxCrackRadiusScale);
}

TEST(GlassDamagePolicy, ResetClearsAccumulatedDamage)
{
    GlassDamagePolicy policy;
    policy.RegisterImpact(3.0f);
    policy.RegisterImpact(3.0f);
    ASSERT_GT(policy.AccumulatedEnergy(), 0.0f);

    // World 换代时运行时会对每块玻璃调用 Reset。
    policy.Reset();

    EXPECT_FLOAT_EQ(policy.AccumulatedEnergy(), 0.0f);
    EXPECT_FALSE(policy.IsBroken());
    const GlassHitDecision decision = policy.RegisterImpact(3.0f);
    EXPECT_EQ(decision.outcome, GlassHitOutcome::Crack);
    EXPECT_FLOAT_EQ(policy.AccumulatedEnergy(), decision.addedEnergy);
}

TEST(GlassDamagePolicy, CrackPatternGrowsWithAccumulatedEnergy)
{
    GlassDamagePolicy policy;
    const GlassHitDecision first = policy.RegisterImpact(2.0f);
    policy.RegisterImpact(3.9f);
    policy.RegisterImpact(3.9f);
    const GlassHitDecision last = policy.RegisterImpact(3.9f);

    EXPECT_GT(last.radialCracks, first.radialCracks);
    EXPECT_GT(last.ringCount, first.ringCount);
    EXPECT_GT(last.crackRadiusScale, first.crackRadiusScale);
    EXPECT_LE(last.radialCracks, policy.Settings().maxRadialCracks);
    EXPECT_LE(last.ringCount, policy.Settings().maxRingCount);
}

TEST(GlassDamagePolicy, InvalidSpeedAddsNoEnergy)
{
    GlassDamagePolicy policy;
    const GlassHitDecision negative = policy.RegisterImpact(-5.0f);
    EXPECT_EQ(negative.outcome, GlassHitOutcome::Crack);
    EXPECT_FLOAT_EQ(negative.addedEnergy, 0.0f);

    const GlassHitDecision notFinite = policy.RegisterImpact(
        std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(notFinite.outcome, GlassHitOutcome::Crack);
    EXPECT_FLOAT_EQ(policy.AccumulatedEnergy(), 0.0f);
}
