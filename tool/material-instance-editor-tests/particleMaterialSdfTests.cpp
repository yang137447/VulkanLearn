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

TEST(ParticleMaterial, ParsesTextureSlotsAndUvTransform)
{
    const Json document = Json{
        {"type", "materialInstance"},
        {"uvScale", Json::array({2.0, 3.0})},
        {"uvOffset", Json::array({0.25, 0.5})},
        {"textures", Json{
            {"albedoMap",
             "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_BaseColor.json"},
            {"normalMap",
             "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_Normal.json"},
            {"pbrParamMap",
             "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_ORM.json"},
            {"emissionMap",
             "Maps/SC_particle_ue_explosion/Textures/T_Explosion_Scorch_BaseColor.json"}}}
    };
    const ParticleMaterialParams params =
        ParseParticleMaterialInstanceJson(document);
    EXPECT_TRUE(params.valid);
    EXPECT_FLOAT_EQ(params.uvScale.x(), 2.0f);
    EXPECT_FLOAT_EQ(params.uvScale.y(), 3.0f);
    EXPECT_FLOAT_EQ(params.uvOffset.x(), 0.25f);
    EXPECT_FLOAT_EQ(params.uvOffset.y(), 0.5f);
    EXPECT_NE(params.albedoMap.find("T_Impact_Concrete_BaseColor"),
        std::string::npos);
    EXPECT_NE(params.normalMap.find("T_Impact_Concrete_Normal"),
        std::string::npos);
    // M_pbr 的 pbrParamMap 与 decal 的 ormMap 是同一语义槽位。
    EXPECT_NE(params.ormMap.find("T_Impact_Concrete_ORM"),
        std::string::npos);
    // emissionMap 与 emissiveMap 同义。
    EXPECT_NE(params.emissiveMap.find("T_Explosion_Scorch_BaseColor"),
        std::string::npos);
    // emissiveMap 存在但未显式给 u_emissive 时用白色 tint，避免自发光为黑色。
    EXPECT_FLOAT_EQ(params.emissive.x(), 1.0f);
    EXPECT_FLOAT_EQ(params.emissive.z(), 1.0f);
    // 与 M_pbr 的 u_emissiveStrength 默认值对齐，贴图不会被 0 强度吞掉。
    EXPECT_FLOAT_EQ(params.emissiveStrength, 1.0f);
}

TEST(ParticleMaterial, MapsPbrTintAndUvParameterAliases)
{
    const Json document = Json{
        {"type", "materialInstance"},
        {"parameters", Json{
            {"u_tintColor", Json::array({0.6, 0.4, 0.2, 0.9})},
            {"u_pbrFactors", Json::array({0.3, 0.8, 0.7, 0.0})},
            {"u_alphaClipThreshold", 0.35},
            {"u_normalStrength", 0.5},
            {"u_uvScale", Json::array({4.0, 5.0})},
            {"u_uvOffset", Json::array({-0.1, 0.2})}}}
    };
    const ParticleMaterialParams params =
        ParseParticleMaterialInstanceJson(document);
    EXPECT_FLOAT_EQ(params.baseColor.x(), 0.6f);
    EXPECT_FLOAT_EQ(params.baseColor.w(), 0.9f);
    EXPECT_FLOAT_EQ(params.roughness, 0.3f);
    EXPECT_FLOAT_EQ(params.metallic, 0.8f);
    EXPECT_FLOAT_EQ(params.ambientOcclusion, 0.7f);
    EXPECT_FLOAT_EQ(params.alphaClipThreshold, 0.35f);
    EXPECT_FLOAT_EQ(params.normalStrength, 0.5f);
    EXPECT_FLOAT_EQ(params.uvScale.x(), 4.0f);
    EXPECT_FLOAT_EQ(params.uvOffset.y(), 0.2f);
}

TEST(ParticleMaterial, RejectsInvalidTexturePathsAndUvTypes)
{
    const Json absolute = Json{
        {"type", "materialInstance"},
        {"textures", Json{{"albedoMap", "C:/textures/T_albedo.json"}}}};
    EXPECT_THROW(
        ParseParticleMaterialInstanceJson(absolute),
        std::invalid_argument);

    const Json traversal = Json{
        {"type", "materialInstance"},
        {"textures", Json{{"normalMap", "../T_normal.json"}}}};
    EXPECT_THROW(
        ParseParticleMaterialInstanceJson(traversal),
        std::invalid_argument);

    const Json nonJson = Json{
        {"type", "materialInstance"},
        {"textures", Json{{"ormMap", "Maps/foo/T_orm.png"}}}};
    EXPECT_THROW(
        ParseParticleMaterialInstanceJson(nonJson),
        std::invalid_argument);

    const Json badUv = Json{
        {"type", "materialInstance"},
        {"parameters", Json{{"u_uvScale", Json::array({1.0})}}}};
    EXPECT_THROW(
        ParseParticleMaterialInstanceJson(badUv),
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
    static_assert(
        kParticleGlobalRecordVec4Count == 11u,
        "GatheredBillboard must keep 8 base + 3 lighting vec4 fields");
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
