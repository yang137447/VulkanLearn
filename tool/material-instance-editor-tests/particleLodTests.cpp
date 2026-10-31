#include <limits>

#include <gtest/gtest.h>

#include "particles/particleLod.h"

namespace
{

using namespace VL;

ParticleEmitterAsset LodEmitter()
{
    ParticleEmitterAsset emitter;
    emitter.spawnRate = 100.0f;
    emitter.lodDistance = 3000.0f;
    emitter.lodSpawnRateScale = 0.4f;
    return emitter;
}

} // namespace

TEST(ParticleLod, DisabledLodKeepsSpawnRate)
{
    ParticleEmitterAsset emitter = LodEmitter();
    emitter.lodDistance = 0.0f;
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(emitter, 100000.0f), 100.0f);
}

TEST(ParticleLod, WithinThresholdKeepsFullSpawnRate)
{
    const ParticleEmitterAsset emitter = LodEmitter();
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(emitter, 0.0f), 100.0f);
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(emitter, 3000.0f), 100.0f);
}

TEST(ParticleLod, BeyondThresholdScalesContinuousSpawnBudget)
{
    const ParticleEmitterAsset emitter = LodEmitter();
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(emitter, 3000.1f), 40.0f);
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(emitter, 12000.0f), 40.0f);

    ParticleEmitterAsset off = LodEmitter();
    off.lodSpawnRateScale = 0.0f;
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(off, 5000.0f), 0.0f);
}

TEST(ParticleLod, LodDoesNotTouchBurstOrRenderingFields)
{
    ParticleEmitterAsset emitter = LodEmitter();
    emitter.burst = true;
    emitter.burstInterval = 2.5f;
    emitter.sizeStart = 30.0f;

    (void)EffectiveParticleSpawnRate(emitter, 9000.0f);

    // helper 只返回缩放后的 spawnRate；burst、尺寸与池大小保持作者值。
    EXPECT_TRUE(emitter.burst);
    EXPECT_FLOAT_EQ(emitter.burstInterval, 2.5f);
    EXPECT_FLOAT_EQ(emitter.sizeStart, 30.0f);
    EXPECT_EQ(emitter.maxParticles, 2048u);
}

TEST(ParticleLod, NonFiniteDistanceDoesNotScale)
{
    const ParticleEmitterAsset emitter = LodEmitter();
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(
            emitter, std::numeric_limits<float>::quiet_NaN()),
        100.0f);
}

namespace
{

ParticleEmitterAsset TieredEmitter()
{
    ParticleEmitterAsset emitter;
    emitter.spawnRate = 100.0f;
    emitter.lodTiers = {
        ParticleLodTier{1000.0f, 0.6f},
        ParticleLodTier{2500.0f, 0.3f},
        ParticleLodTier{5000.0f, 0.1f}};
    return emitter;
}

} // namespace

TEST(ParticleLod, SelectsFurthestCrossedTier)
{
    const ParticleEmitterAsset emitter = TieredEmitter();
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 0.0f), 100.0f);
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 999.0f), 100.0f);
    // 边界是“已越过”：distance 恰好等于档位时立即应用。
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 1000.0f), 60.0f);
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 2499.0f), 60.0f);
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 2500.0f), 30.0f);
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 5000.0f), 10.0f);
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 90000.0f), 10.0f);
}

TEST(ParticleLod, TiersTakePriorityOverLegacySingleTier)
{
    ParticleEmitterAsset emitter = TieredEmitter();
    // 旧字段即使配置了更低阈值，也不参与有 tiers 时的选择。
    emitter.lodDistance = 100.0f;
    emitter.lodSpawnRateScale = 0.0f;

    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 500.0f), 100.0f);
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 2500.0f), 30.0f);
}

TEST(ParticleLod, EmptyTiersFallBackToLegacyTier)
{
    ParticleEmitterAsset emitter = LodEmitter();
    emitter.lodTiers.clear();
    EXPECT_FLOAT_EQ(EffectiveParticleSpawnRate(emitter, 3000.0f), 100.0f);
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(emitter, 3001.0f), 40.0f);
}

TEST(ParticleLod, NonFiniteDistanceWithTiersDoesNotScale)
{
    const ParticleEmitterAsset emitter = TieredEmitter();
    EXPECT_FLOAT_EQ(
        EffectiveParticleSpawnRate(
            emitter, std::numeric_limits<float>::infinity()),
        100.0f);
}
