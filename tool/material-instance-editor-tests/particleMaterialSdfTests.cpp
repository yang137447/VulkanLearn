#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleGlobalSort.h"
#include "particles/particleDeterminism.h"
#include "particles/particleMaterial.h"
#include "particles/particleSdf.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;

} // namespace

TEST(ParticleMaterial, ParsesSupportedNumericSubset)
{
    const Json document = Json{
        {"type", "materialInstance"},
        {"parameters", Json{
            {"u_baseColor", Json::array({0.25, 0.5, 0.75, 0.8})},
            {"u_emissive", Json::array({1.0, 0.5, 0.25})},
            {"u_emissiveStrength", 3.0},
            {"u_roughness", 0.35},
            {"u_metallic", 0.9},
            {"u_unknown", 123.0}}}
    };
    const ParticleMaterialParams params =
        ParseParticleMaterialInstanceJson(document);
    EXPECT_TRUE(params.valid);
    EXPECT_FLOAT_EQ(params.baseColor.x(), 0.25f);
    EXPECT_FLOAT_EQ(params.baseColor.w(), 0.8f);
    EXPECT_FLOAT_EQ(params.emissive.y(), 0.5f);
    EXPECT_FLOAT_EQ(params.emissiveStrength, 3.0f);
    EXPECT_FLOAT_EQ(params.roughness, 0.35f);
    EXPECT_FLOAT_EQ(params.metallic, 0.9f);
}

TEST(ParticleMaterial, ClampsPbrAndRejectsWrongTypes)
{
    const Json clamped = Json{
        {"type", "materialInstance"},
        {"parameters", Json{
            {"u_roughness", 4.0},
            {"u_metallic", -2.0}}}
    };
    const ParticleMaterialParams params =
        ParseParticleMaterialInstanceJson(clamped);
    EXPECT_FLOAT_EQ(params.roughness, 1.0f);
    EXPECT_FLOAT_EQ(params.metallic, 0.0f);

    const Json badColor = Json{
        {"type", "materialInstance"},
        {"parameters", Json{{"u_baseColor", 1.0}}}};
    EXPECT_THROW(
        ParseParticleMaterialInstanceJson(badColor),
        std::invalid_argument);

    const Json badType = Json{
        {"type", "material"},
        {"parameters", Json::object()}};
    EXPECT_THROW(
        ParseParticleMaterialInstanceJson(badType),
        std::invalid_argument);
}

TEST(ParticleSdf, ParsesAndSamplesTrilinearDistance)
{
    Json document = Json{
        {"type", "particleSdf"},
        {"resolution", Json::array({2, 2, 2})},
        {"boundsMin", Json::array({-10.0, -10.0, -10.0})},
        {"boundsMax", Json::array({10.0, 10.0, 10.0})},
        {"distances", Json::array({
            -10.0, 10.0, -10.0, 10.0,
            -10.0, 10.0, -10.0, 10.0})}
    };
    const ParticleSdfAsset sdf = ParseParticleSdfJson(document);
    EXPECT_EQ(sdf.resolution, Eigen::Vector3i(2, 2, 2));
    EXPECT_NEAR(sdf.SampleDistance(Eigen::Vector3f(0.0f, 0.0f, 0.0f)),
        0.0f, 1.0e-4f);
    EXPECT_NEAR(sdf.SampleDistance(Eigen::Vector3f(5.0f, 0.0f, 0.0f)),
        5.0f, 1.0e-4f);
    EXPECT_FALSE(std::isfinite(
        sdf.SampleDistance(Eigen::Vector3f(100.0f, 0.0f, 0.0f))));
    const Eigen::Vector3f gradient =
        sdf.SampleGradient(Eigen::Vector3f(0.0f, 0.0f, 0.0f));
    EXPECT_NEAR(gradient.x(), 1.0f, 1.0e-4f);
    EXPECT_NEAR(gradient.y(), 0.0f, 1.0e-4f);

    document["distances"] = Json::array({0.0, 0.0});
    EXPECT_THROW(ParseParticleSdfJson(document), std::invalid_argument);
    document["distances"] = Json::array({
        -10.0, 10.0, -10.0, 10.0,
        -10.0, 10.0, -10.0, 10.0});
    document["resolution"] = Json::array({1, 2, 2});
    EXPECT_THROW(ParseParticleSdfJson(document), std::invalid_argument);
}

TEST(ParticleGlobalSort, SortsBackToFrontWithStableTieBreak)
{
    std::vector<ParticleGlobalSortKey> keys = {
        ParticleGlobalSortKey{5.0f, 0, 1, 0},
        ParticleGlobalSortKey{9.0f, 1, 0, 1},
        ParticleGlobalSortKey{5.0f, 0, 0, 2},
        ParticleGlobalSortKey{1.0f, 2, 3, 3}};
    SortParticleGlobalKeysBackToFront(keys);
    ASSERT_EQ(keys.size(), 4u);
    EXPECT_FLOAT_EQ(keys[0].viewDepth, 9.0f);
    EXPECT_FLOAT_EQ(keys[1].viewDepth, 5.0f);
    EXPECT_EQ(keys[1].particleIndex, 0u);
    EXPECT_FLOAT_EQ(keys[2].viewDepth, 5.0f);
    EXPECT_EQ(keys[2].particleIndex, 1u);
    EXPECT_FLOAT_EQ(keys[3].viewDepth, 1.0f);
}

TEST(ParticleGlobalSort, CapacityAndTextureLimits)
{
    EXPECT_TRUE(CanUseParticleGlobalSort(1, 0));
    EXPECT_TRUE(CanUseParticleGlobalSort(
        kParticleGlobalSortCapacity, kParticleGlobalTextureSlots));
    EXPECT_FALSE(CanUseParticleGlobalSort(
        kParticleGlobalSortCapacity + 1u, 1));
    EXPECT_FALSE(CanUseParticleGlobalSort(
        16, kParticleGlobalTextureSlots + 1u));
    EXPECT_FALSE(CanUseParticleGlobalSort(0, 1));
}

TEST(ParticleDeterminism, FixedStepAccumulatesAndCaps)
{
    ParticleFixedStepState state;
    const float fixed = 1.0f / 60.0f;
    ParticleFixedStepTick tick =
        AdvanceParticleFixedStep(1.0f / 60.0f, fixed, 8, state);
    EXPECT_EQ(tick.steps, 1u);
    EXPECT_FLOAT_EQ(tick.stepSeconds, fixed);
    EXPECT_EQ(state.stepIndex, 1u);

    // 30 FPS 一帧覆盖两个固定步。
    tick = AdvanceParticleFixedStep(1.0f / 30.0f, fixed, 8, state);
    EXPECT_EQ(tick.steps, 2u);
    EXPECT_EQ(state.stepIndex, 3u);

    // 不足一步：不推进 stepIndex，余量保留到下一帧。
    tick = AdvanceParticleFixedStep(fixed * 0.5f, fixed, 8, state);
    EXPECT_EQ(tick.steps, 0u);
    EXPECT_FLOAT_EQ(tick.stepSeconds, 0.0f);
    EXPECT_EQ(state.stepIndex, 3u);
    EXPECT_GT(state.accumulator, 0.0f);

    // 长停顿最多 8 步，剩余余量仍保留。
    tick = AdvanceParticleFixedStep(1.0f, fixed, 8, state);
    EXPECT_EQ(tick.steps, 8u);
    EXPECT_EQ(state.stepIndex, 11u);
    EXPECT_GT(state.accumulator, 0.0f);

    // 非有限/负 dt 不推进。
    tick = AdvanceParticleFixedStep(
        std::numeric_limits<float>::quiet_NaN(), fixed, 8, state);
    EXPECT_EQ(tick.steps, 0u);
}
