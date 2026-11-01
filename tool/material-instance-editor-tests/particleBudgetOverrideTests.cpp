#include <cmath>
#include <limits>
#include <unordered_map>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleBudget.h"
#include "particles/particleUserParameterOverride.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleBudget, ClampsAuthoredCapacityToBudget)
{
    EXPECT_EQ(ResolveParticleEmitterCapacity(2048u, 512u, true), 512u);
    EXPECT_EQ(ResolveParticleEmitterCapacity(128u, 512u, true), 128u);
    EXPECT_EQ(ResolveParticleEmitterCapacity(2048u, 0u, true), 0u);
    EXPECT_EQ(ResolveParticleEmitterCapacity(2048u, 0u, false), 2048u);
    EXPECT_TRUE(ShouldAllocateParticlePool(1u));
    EXPECT_FALSE(ShouldAllocateParticlePool(0u));
}

TEST(ParticleBudget, LocalMirrorConsumesTotalCapInEmitterOrder)
{
    const std::vector<uint32_t> authored = {2048u, 1024u, 512u};
    const std::vector<uint32_t> budgets =
        ComputeParticleEmitterBudgetCapacities(
            authored, 3000u, 1.0f);
    ASSERT_EQ(budgets.size(), 3u);
    EXPECT_EQ(budgets[0], 2048u);
    EXPECT_EQ(budgets[1], 952u);
    EXPECT_EQ(budgets[2], 0u);

    const std::vector<uint32_t> scaled =
        ComputeParticleEmitterBudgetCapacities(
            authored, 0u, 0.5f);
    ASSERT_EQ(scaled.size(), 3u);
    EXPECT_EQ(scaled[0], 1024u);
    EXPECT_EQ(scaled[1], 512u);
    EXPECT_EQ(scaled[2], 256u);
}

TEST(ParticleUserParameterOverride, SignatureIsOrderIndependentAndChangesWithValues)
{
    const std::unordered_map<std::string, float> first = {
        {"Speed", 2.0f}, {"Radius", 4.0f}};
    const std::unordered_map<std::string, float> reordered = {
        {"Radius", 4.0f}, {"Speed", 2.0f}};
    EXPECT_EQ(
        BuildParticleUserParameterSignature(first),
        BuildParticleUserParameterSignature(reordered));

    auto changed = first;
    changed["Speed"] = 2.5f;
    EXPECT_NE(
        BuildParticleUserParameterSignature(first),
        BuildParticleUserParameterSignature(changed));
    changed["Extra"] = 1.0f;
    EXPECT_NE(
        BuildParticleUserParameterSignature(first),
        BuildParticleUserParameterSignature(changed));
}

TEST(ParticleUserParameterOverride, MergeOverwritesAndAddsRootParameters)
{
    const nlohmann::json authoring = nlohmann::json{
        {"name", "PS_params"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json{
            {"Speed", 1.0f},
            {"Radius", 4.0f}}},
        {"modules", nlohmann::json::array()}};
    const std::unordered_map<std::string, float> runtime = {
        {"Speed", 3.0f},
        {"Extra", 7.0f},
        {"Bad", std::numeric_limits<float>::quiet_NaN()}};

    const nlohmann::json merged =
        MergeParticleUserParameters(authoring, runtime);
    EXPECT_FLOAT_EQ(
        merged.at("userParameters").at("Speed").get<float>(), 3.0f);
    EXPECT_FLOAT_EQ(
        merged.at("userParameters").at("Radius").get<float>(), 4.0f);
    EXPECT_FLOAT_EQ(
        merged.at("userParameters").at("Extra").get<float>(), 7.0f);
    // 非有限运行时值被过滤，不会污染 ParseJson 输入。
    EXPECT_FALSE(merged.at("userParameters").contains("Bad"));
    // 作者结构的其它字段保持不变。
    EXPECT_EQ(merged.at("name"), "PS_params");
    EXPECT_TRUE(merged.at("modules").is_array());
}

TEST(ParticleUserParameterOverride, ReconcilePreservesRuntimeOnlyEmitters)
{
    std::vector<ParticleEmitterAsset> snapshot(2);
    snapshot[0].name = "authored";
    snapshot[0].spawnRate = 1.0f;
    snapshot[1].name = "runtime.2";
    snapshot[1].spawnRate = 9.0f;
    snapshot[1].maxParticles = 64;

    std::vector<ParticleEmitterAsset> parsed(1);
    parsed[0].name = "authored";
    parsed[0].spawnRate = 5.0f;
    parsed[0].maxParticles = 128;

    const std::vector<ParticleEmitterAsset> reconciled =
        ReconcileParticleEmitterOverrides(parsed, snapshot);
    ASSERT_EQ(reconciled.size(), 2u);
    EXPECT_EQ(reconciled[0].name, "authored");
    EXPECT_FLOAT_EQ(reconciled[0].spawnRate, 5.0f);
    EXPECT_EQ(reconciled[0].maxParticles, 128u);
    EXPECT_EQ(reconciled[1].name, "runtime.2");
    EXPECT_FLOAT_EQ(reconciled[1].spawnRate, 9.0f);
    EXPECT_EQ(reconciled[1].maxParticles, 64u);
}

TEST(ParticleUserParameterOverride, FullOverrideReparsesAuthoringAndKeepsRuntimeEmitter)
{
    const nlohmann::json document = nlohmann::json{
        {"name", "PS_runtime_params"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json{{"Speed", 1.0f}}},
        {"modules", nlohmann::json::array({
            nlohmann::json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", nlohmann::json{{"maxParticles", 64}}}},
            nlohmann::json{
                {"type", "spawn.rate"},
                {"enabled", true},
                {"fields", nlohmann::json{{"spawnRate", "$Speed"}}}}
        })}
    };
    ParticleSystemAsset base = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_runtime_params.json");
    ASSERT_EQ(base.emitters.size(), 1u);
    ASSERT_TRUE(base.authoringJson != nullptr);
    EXPECT_FLOAT_EQ(base.emitters[0].spawnRate, 1.0f);

    ParticleEmitterAsset runtimeEmitter = base.emitters[0];
    runtimeEmitter.name = "runtime.1";
    runtimeEmitter.spawnRate = 9.0f;
    base.emitters.push_back(runtimeEmitter);

    const std::unordered_map<std::string, float> runtime = {
        {"Speed", 5.0f}};
    const ParticleSystemAsset parsed =
        ParseParticleAssetWithRuntimeParameters(base, runtime);
    ASSERT_EQ(parsed.emitters.size(), 2u);
    EXPECT_EQ(parsed.emitters[0].name, "emitter0");
    EXPECT_FLOAT_EQ(parsed.emitters[0].spawnRate, 5.0f);
    EXPECT_EQ(parsed.emitters[1].name, "runtime.1");
    EXPECT_FLOAT_EQ(parsed.emitters[1].spawnRate, 9.0f);
    EXPECT_EQ(parsed.authoringJson, base.authoringJson);
}
