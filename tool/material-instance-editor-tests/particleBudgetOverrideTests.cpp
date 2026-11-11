#include <cmath>
#include <limits>
#include <unordered_map>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleBudget.h"
#include "particles/particleDataChannel.h"
#include "particles/particleSdf.h"
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

TEST(ParticleUserParameterOverride, DataChannelsAreBaseAndInstanceOverridesWin)
{
    const std::unordered_map<std::string, float> channels = {
        {"Wind", 2.0f}, {"OnlyChannel", 3.0f}};
    const std::unordered_map<std::string, float> instance = {
        {"Wind", 7.0f}, {"OnlyInstance", 4.0f},
        {"Bad", std::numeric_limits<float>::quiet_NaN()}};

    const auto combined =
        CombineParticleRuntimeParameters(channels, instance);
    EXPECT_FLOAT_EQ(combined.at("Wind"), 7.0f);
    EXPECT_FLOAT_EQ(combined.at("OnlyChannel"), 3.0f);
    EXPECT_FLOAT_EQ(combined.at("OnlyInstance"), 4.0f);
    EXPECT_EQ(combined.find("Bad"), combined.end());
}

TEST(ParticleDataChannel, JsonInfersAndRoundTripsTypedValues)
{
    std::string error;
    ParticleDataChannelValue value;

    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(0.5f, value, error))
        << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Float);
    EXPECT_FLOAT_EQ(value.values[0], 0.5f);

    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(true, value, error))
        << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Bool);
    EXPECT_FLOAT_EQ(value.values[0], 1.0f);

    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json::array({1.0f, 2.0f}), value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec2);
    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json::array({1.0f, 2.0f, 3.0f}), value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec3);
    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json::array({1.0f, 2.0f, 3.0f, 4.0f}), value, error))
        << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec4);

    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"int", 42}}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Int);
    EXPECT_FLOAT_EQ(value.values[0], 42.0f);
    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"bool", false}}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Bool);
    EXPECT_FLOAT_EQ(value.values[0], 0.0f);
    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"quat", nlohmann::json::array(
            {0.0f, 0.0f, 0.0f, 1.0f})}}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Quat);
    EXPECT_FLOAT_EQ(value.values[3], 1.0f);
    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"type", "vec3"},
            {"values", nlohmann::json::array({4.0f, 5.0f, 6.0f})}},
        value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec3);

    const nlohmann::json encoded = value.ToTypedJson();
    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(encoded, value, error))
        << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec3);
    EXPECT_FLOAT_EQ(value.values[1], 5.0f);

    ASSERT_TRUE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"type", 6},
            {"values", nlohmann::json::array(
                {0.0f, 0.0f, 0.0f, 1.0f})}}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Quat);
}

TEST(ParticleDataChannel, JsonRejectsInvalidShapesTypesAndRanges)
{
    std::string error;
    ParticleDataChannelValue value;
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        std::numeric_limits<float>::quiet_NaN(), value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(1.0e7f, value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(1.0e300, value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        nlohmann::json::array({1.0f}), value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        nlohmann::json::array({1.0f, 2.0f, 3.0f, 4.0f, 5.0f}),
        value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"int", 1.5f}}, value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"bool", 2}}, value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"type", "not-a-type"},
            {"values", nlohmann::json::array({1.0f})}}, value, error));
    EXPECT_FALSE(ParticleDataChannelValue::ParseJson(
        nlohmann::json{{"type", "vec3"},
            {"values", nlohmann::json::array({1.0f, 2.0f})}},
        value, error));
}

TEST(ParticleDataChannel, ConsoleTextGrammarSupportsAllTypedForms)
{
    std::string error;
    ParticleDataChannelValue value;
    EXPECT_TRUE(ParseParticleDataChannelText({"0.5"}, value, error))
        << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Float);
    EXPECT_TRUE(ParseParticleDataChannelText(
        {"vec2", "1", "2"}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec2);
    EXPECT_TRUE(ParseParticleDataChannelText(
        {"vec3", "1", "2", "3"}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec3);
    EXPECT_TRUE(ParseParticleDataChannelText(
        {"vec4", "1", "2", "3", "4"}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Vec4);
    EXPECT_TRUE(ParseParticleDataChannelText(
        {"quat", "0", "0", "0", "1"}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Quat);
    EXPECT_TRUE(ParseParticleDataChannelText(
        {"bool", "true"}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Bool);
    EXPECT_FLOAT_EQ(value.values[0], 1.0f);
    EXPECT_TRUE(ParseParticleDataChannelText(
        {"int", "42"}, value, error)) << error;
    EXPECT_EQ(value.type, ParticleDataChannelType::Int);
    EXPECT_FLOAT_EQ(value.values[0], 42.0f);

    EXPECT_FALSE(ParseParticleDataChannelText(
        {"vec3", "1", "2"}, value, error));
    EXPECT_FALSE(ParseParticleDataChannelText(
        {"bool", "maybe"}, value, error));
    EXPECT_FALSE(ParseParticleDataChannelText(
        {"int", "1.5"}, value, error));
    EXPECT_FALSE(ParseParticleDataChannelText(
        {"vec3", "1", "2", "1e7"}, value, error));
}

TEST(ParticleUserParameterOverride, TypedChannelsReplaceVectorBoolAndFloatFields)
{
    const nlohmann::json document = nlohmann::json{
        {"name", "PS_typed_data_channel"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json::object()},
        {"modules", nlohmann::json::array({
            nlohmann::json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", nlohmann::json{{"maxParticles", 32}}}},
            nlohmann::json{
                {"type", "forces.wind"},
                {"enabled", true},
                {"fields", nlohmann::json{{"wind", "$WindVec"}}}},
            nlohmann::json{
                {"type", "forces.drag"},
                {"enabled", true},
                {"fields", nlohmann::json{{"drag", "$DragFloat"}}}},
            nlohmann::json{
                {"type", "renderer"},
                {"enabled", true},
                {"fields", nlohmann::json{{"additive", "$Additive"}}}}
        })}
    };
    ParticleSystemAsset base;
    base.name = "PS_typed_data_channel";
    base.version = 2;
    base.sourcePath = "Maps/SC_test/PS_typed_data_channel.json";
    base.authoringJson =
        std::make_shared<const nlohmann::json>(document);
    ParticleEmitterAsset snapshotEmitter;
    snapshotEmitter.name = "emitter0";
    base.emitters.push_back(snapshotEmitter);

    ParticleDataChannelMap channels;
    channels.emplace("WindVec", ParticleDataChannelValue::MakeVec3(1, 2, 3));
    channels.emplace("DragFloat", ParticleDataChannelValue::MakeFloat(9.0f));
    channels.emplace("Additive", ParticleDataChannelValue::MakeBool(true));

    const ParticleSystemAsset parsed =
        ParseParticleAssetWithRuntimeParametersTyped(
            base, channels, {{"DragFloat", 1.5f}});
    ASSERT_EQ(parsed.emitters.size(), 1u);
    EXPECT_FLOAT_EQ(parsed.emitters[0].wind.x(), 1.0f);
    EXPECT_FLOAT_EQ(parsed.emitters[0].wind.y(), 2.0f);
    EXPECT_FLOAT_EQ(parsed.emitters[0].wind.z(), 3.0f);
    EXPECT_FLOAT_EQ(parsed.emitters[0].drag, 1.5f);
    EXPECT_TRUE(parsed.emitters[0].additive);
    EXPECT_EQ(parsed.userParameterReferences.count("WindVec"), 1u);
    EXPECT_EQ(parsed.userParameterReferences.count("Additive"), 1u);
    EXPECT_EQ(parsed.userParameterReferences.count("DragFloat"), 1u);
}

TEST(ParticleUserParameterOverride, TypedScalarProjectionKeepsInstanceOverride)
{
    ParticleDataChannelMap channels;
    channels.emplace("Wind", ParticleDataChannelValue::MakeFloat(2.0f));
    channels.emplace("Direction", ParticleDataChannelValue::MakeVec3(1, 2, 3));
    const auto combined = CombineParticleRuntimeParametersTyped(
        channels, {{"Wind", 7.0f}});
    EXPECT_FLOAT_EQ(combined.at("Wind"), 7.0f);
    EXPECT_EQ(combined.find("Direction"), combined.end());
}

TEST(ParticleUserParameterOverride, DataChannelValuesReparseModuleFields)
{
    const nlohmann::json document = nlohmann::json{
        {"name", "PS_data_channel"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json{{"Wind", 1.0f}}},
        {"modules", nlohmann::json::array({
            nlohmann::json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", nlohmann::json{{"maxParticles", 32}}}},
            nlohmann::json{
                {"type", "forces.wind"},
                {"enabled", true},
                {"fields", nlohmann::json{
                    {"wind", nlohmann::json::array(
                        {"$Wind", 0.0f, 0.0f})}}}}
        })}
    };
    const ParticleSystemAsset base = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_data_channel.json");
    const auto combined = CombineParticleRuntimeParameters(
        {{"Wind", 7.0f}}, {});
    const ParticleSystemAsset parsed =
        ParseParticleAssetWithRuntimeParameters(base, combined);
    ASSERT_EQ(parsed.emitters.size(), 1u);
    EXPECT_FLOAT_EQ(parsed.emitters[0].wind.x(), 7.0f);
}

TEST(ParticleUserParameterOverride, FilterKeepsOnlyReferencedChannels)
{
    const nlohmann::json document = nlohmann::json{
        {"name", "PS_reference_filter"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json{{"Wind", 1.0f}}},
        {"modules", nlohmann::json::array({
            nlohmann::json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", nlohmann::json{{"maxParticles", 32}}}},
            nlohmann::json{
                {"type", "forces.wind"},
                {"enabled", true},
                {"fields", nlohmann::json{
                    {"wind", nlohmann::json::array(
                        {"$Wind", 0.0f, 0.0f})}}}}
        })}
    };
    const ParticleSystemAsset base = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_reference_filter.json");
    ASSERT_EQ(base.userParameterReferences.size(), 1u);
    EXPECT_EQ(base.userParameterReferences.count("Wind"), 1u);

    const std::unordered_map<std::string, float> all = {
        {"Wind", 3.0f}, {"Unused", 9.0f}};
    const auto filtered =
        FilterParticleRuntimeParameters(base, all);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_FLOAT_EQ(filtered.at("Wind"), 3.0f);

    EXPECT_TRUE(
        FilterParticleRuntimeParameters(
            base, {{"Unused", 9.0f}}).empty());
}

TEST(ParticleUserParameterOverride, RuntimeReparseReusesCollisionSdf)
{
    const nlohmann::json document = nlohmann::json{
        {"name", "PS_reuse_collision"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json{{"Wind", 1.0f}}},
        {"modules", nlohmann::json::array({
            nlohmann::json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", nlohmann::json{{"maxParticles", 32}}}},
            nlohmann::json{
                {"type", "collision.analytic"},
                {"enabled", true},
                {"fields", nlohmann::json{{"collision", nlohmann::json{
                    {"type", "mesh"},
                    {"mesh", nlohmann::json{
                        {"path", "Meshes/does_not_exist.json"}}},
                    {"radius", 0.0}}}}}},
            nlohmann::json{
                {"type", "forces.wind"},
                {"enabled", true},
                {"fields", nlohmann::json{
                    {"wind", nlohmann::json::array(
                        {"$Wind", 0.0f, 0.0f})}}}}
        })}
    };
    auto sdf = std::make_shared<ParticleSdfAsset>();
    sdf->sourceIdentity = "mesh|Meshes/does_not_exist.json|10|20";
    sdf->distances = {0.0f};
    ParticleSystemAsset base;
    base.name = "PS_reuse_collision";
    base.version = 2;
    base.sourcePath = "Maps/SC_test/PS_reuse_collision.json";
    base.userParameters = {{"Wind", 1.0f}};
    base.userParameterReferences = {"Wind"};
    base.authoringJson =
        std::make_shared<const nlohmann::json>(document);
    ParticleEmitterAsset emitter;
    emitter.name = "emitter0";
    emitter.collisionType = "mesh";
    emitter.collisionMeshPath = "Meshes/does_not_exist.json";
    emitter.collisionSdf = sdf;
    base.emitters.push_back(emitter);

    const ParticleSystemAsset parsed =
        ParseParticleAssetWithRuntimeParameters(
            base, {{"Wind", 4.0f}});
    ASSERT_EQ(parsed.emitters.size(), 1u);
    EXPECT_EQ(parsed.emitters[0].collisionSdf, sdf);
    EXPECT_FLOAT_EQ(parsed.emitters[0].wind.x(), 4.0f);
}

TEST(ParticleUserParameterOverride, RuntimeReparseRejectsRadiusAboveMeshPadding)
{
    const nlohmann::json document = nlohmann::json{
        {"name", "PS_reuse_padding"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", nlohmann::json{{"Radius", 1.0f}}},
        {"modules", nlohmann::json::array({
            nlohmann::json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", nlohmann::json{{"maxParticles", 32}}}},
            nlohmann::json{
                {"type", "collision.analytic"},
                {"enabled", true},
                {"fields", nlohmann::json{{"collision", nlohmann::json{
                    {"type", "mesh"},
                    {"mesh", nlohmann::json{
                        {"path", "Meshes/padding_cube.json"}}},
                    {"radius", "$Radius"}}}}}}
        })}
    };
    auto sdf = std::make_shared<ParticleSdfAsset>();
    sdf->sourceIdentity = "mesh|Meshes/padding_cube.json|10|20";
    sdf->sourcePadding = 5.0f;
    sdf->distances = {0.0f};
    ParticleSystemAsset base;
    base.name = "PS_reuse_padding";
    base.version = 2;
    base.sourcePath = "Maps/SC_test/PS_reuse_padding.json";
    base.userParameters = {{"Radius", 1.0f}};
    base.userParameterReferences = {"Radius"};
    base.authoringJson =
        std::make_shared<const nlohmann::json>(document);
    ParticleEmitterAsset emitter;
    emitter.name = "emitter0";
    emitter.collisionType = "mesh";
    emitter.collisionMeshPath = "Meshes/padding_cube.json";
    emitter.collisionRadius = 1.0f;
    emitter.collisionSdf = sdf;
    base.emitters.push_back(emitter);

    EXPECT_NO_THROW(
        (void)ParseParticleAssetWithRuntimeParameters(
            base, {{"Radius", 5.0f}}));
    EXPECT_THROW(
        (void)ParseParticleAssetWithRuntimeParameters(
            base, {{"Radius", 6.0f}}),
        std::invalid_argument);
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
