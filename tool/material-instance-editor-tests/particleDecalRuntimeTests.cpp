#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Geometry>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/assetDocumentFields.h"
#include "editor/particles/particleModuleEdit.h"
#include "particles/particleAsset.h"
#include "particles/particleDecalRuntime.h"
#include "particles/particleEmitterKey.h"
#include "render/frontend/renderScene.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;

constexpr float kPi = 3.14159265358979323846f;

ParticleEmitterAsset MakeDecalEmitter(
    const std::string& name,
    uint32_t seed,
    uint32_t maxParticles,
    float lifetime)
{
    ParticleEmitterAsset emitter;
    emitter.name = name;
    emitter.renderMode = "decal";
    emitter.hasDecal = true;
    emitter.decal.size = Eigen::Vector3f(100.0f, 400.0f, 400.0f);
    emitter.decal.color = Eigen::Vector3f::Ones();
    emitter.decal.opacity = 0.8f;
    emitter.maxParticles = maxParticles;
    emitter.burst = true;
    emitter.burstInterval = 0.0f;
    emitter.spawnRate = 0.0f;
    emitter.lifetimeMin = lifetime;
    emitter.lifetimeMax = lifetime;
    emitter.shapeType = "sphere";
    emitter.shapeRadius = 0.0f;
    emitter.velocityMin = Eigen::Vector3f::Zero();
    emitter.velocityMax = Eigen::Vector3f::Zero();
    emitter.gravity = Eigen::Vector3f::Zero();
    emitter.drag = 0.0f;
    emitter.colorStart = Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    emitter.colorEnd = Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    emitter.randomSeed = seed;
    emitter.deterministicRandom = true;
    emitter.additive = true;
    emitter.sortParticles = false;
    return emitter;
}

ParticleSystemSnapshot MakeSnapshot(
    const std::string& name,
    const ParticleEmitterAsset& emitter,
    const Eigen::Matrix4f& model = Eigen::Matrix4f::Identity())
{
    ParticleSystemSnapshot snapshot;
    snapshot.particleSystemId = 1;
    snapshot.name = name;
    snapshot.assetPath = "Maps/SC_test/" + name + ".json";
    snapshot.asset.name = "PS_" + name;
    snapshot.asset.emitters.push_back(emitter);
    snapshot.model = model;
    snapshot.visible = true;
    return snapshot;
}

void UpdateFrames(
    ParticleDecalRuntime& runtime,
    RenderScene& scene,
    uint32_t frames,
    float delta)
{
    scene.simulationDeltaSeconds = delta;
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        runtime.Synchronize(scene);
        runtime.Update(scene);
    }
}

DecalSnapshot MakeStaticDecal(uint64_t id)
{
    DecalSnapshot decal;
    decal.decalId = id;
    decal.visible = true;
    return decal;
}

std::set<uint64_t> DynamicDecalIds(const RenderScene& scene,
    const std::set<uint64_t>& staticIds)
{
    std::set<uint64_t> ids;
    for (const DecalSnapshot& decal : scene.decals)
    {
        if (staticIds.find(decal.decalId) == staticIds.end())
        {
            ids.insert(decal.decalId);
        }
    }
    return ids;
}

Json DecalEmitterDocument()
{
    return Json::parse(R"json({
        "name": "PS_decal",
        "type": "particleSystem",
        "version": 2,
        "emitters": [{
            "name": "scorch",
            "modules": [
                {"type": "emitter", "fields": {
                    "maxParticles": 4,
                    "renderMode": "decal"}},
                {"type": "renderer", "fields": {
                    "renderMode": "decal",
                    "decal": {
                        "size": [60.0, 300.0, 240.0],
                        "rotation": [0.0, 0.0, 90.0],
                        "color": [0.5, 0.25, 0.125],
                        "opacity": 0.75,
                        "edgeSoftness": 0.4,
                        "noiseStrength": 0.6,
                        "roughness": 0.8,
                        "metallic": 0.1,
                        "ambientOcclusion": 0.7,
                        "normalStrength": 0.3,
                        "emissive": [4.0, 2.0, 1.0],
                        "emissiveStrength": 6.0,
                        "decalResponse": "metallicRoughnessNormal",
                        "albedoMap": "Maps/SC_test/Textures/T_burn_albedo.json",
                        "normalMap": "Maps/SC_test/Textures/T_burn_normal.json",
                        "ormMap": "Maps/SC_test/Textures/T_burn_orm.json",
                        "sortOrder": 7
                    }}},
                {"type": "spawn.rate", "fields": {
                    "spawnRate": 0.0,
                    "burst": true,
                    "burstInterval": 1.0}},
                {"type": "update.lifetime", "fields": {
                    "lifetime": [0.2, 0.4]}}
            ]
        }]
    })json");
}

} // namespace

TEST(ParticleAsset, DecalModeParsesDefaultsAndRejectsInvalidFields)
{
    const Json document = Json::parse(R"json({
        "name": "PS_decal",
        "type": "particleSystem",
        "version": 2,
        "emitters": [{
            "name": "scorch",
            "modules": [{
                "type": "renderer",
                "fields": {
                    "renderMode": "decal",
                    "additive": false,
                    "sortParticles": false,
                    "decal": {"size": [40.0, 120.0, 80.0]}
                }
            }]
        }]
    })json");
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_decal.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    const ParticleEmitterAsset& emitter = asset.emitters.front();
    EXPECT_EQ(emitter.renderMode, "decal");
    EXPECT_TRUE(emitter.hasDecal);
    EXPECT_EQ(emitter.decal.size,
        Eigen::Vector3f(40.0f, 120.0f, 80.0f));
    EXPECT_EQ(emitter.decal.rotation, Eigen::Vector3f::Zero());
    EXPECT_FLOAT_EQ(emitter.decal.opacity, 0.85f);
    EXPECT_FLOAT_EQ(emitter.decal.edgeSoftness, 0.35f);
    EXPECT_FLOAT_EQ(emitter.decal.noiseStrength, 0.55f);
    EXPECT_EQ(emitter.decal.emissive, Eigen::Vector3f::Zero());
    EXPECT_FLOAT_EQ(emitter.decal.emissiveStrength, 0.0f);
    EXPECT_EQ(emitter.decal.responseMask, DefaultDecalResponse());
    EXPECT_EQ(emitter.decal.sortOrder, 0u);

    Json missing = document;
    missing["emitters"][0]["modules"][0]["fields"].erase("decal");
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            missing, "Maps/SC_test/PS_decal_missing.json"),
        std::invalid_argument);

    Json zeroSize = document;
    zeroSize["emitters"][0]["modules"][0]["fields"]["decal"]["size"][0] =
        0.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            zeroSize, "Maps/SC_test/PS_decal_zero.json"),
        std::invalid_argument);

    Json badOpacity = document;
    badOpacity["emitters"][0]["modules"][0]["fields"]["decal"]["opacity"] =
        1.1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badOpacity, "Maps/SC_test/PS_decal_opacity.json"),
        std::invalid_argument);

    Json badResponse = document;
    badResponse["emitters"][0]["modules"][0]["fields"]["decal"]
        ["decalResponse"] = "Color";
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badResponse, "Maps/SC_test/PS_decal_response.json"),
        std::invalid_argument);

    Json badSort = document;
    badSort["emitters"][0]["modules"][0]["fields"]["decal"]["sortOrder"] =
        256;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badSort, "Maps/SC_test/PS_decal_sort.json"),
        std::invalid_argument);

    Json badColor = document;
    badColor["emitters"][0]["modules"][0]["fields"]["decal"]["color"] =
        Json::array({65.0, 0.0, 0.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badColor, "Maps/SC_test/PS_decal_color.json"),
        std::invalid_argument);

    Json badTexture = document;
    badTexture["emitters"][0]["modules"][0]["fields"]["decal"]
        ["albedoMap"] = 3;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badTexture, "Maps/SC_test/PS_decal_texture.json"),
        std::invalid_argument);

    Json badEmissive = document;
    badEmissive["emitters"][0]["modules"][0]["fields"]["decal"]
        ["emissive"] = Json::array({65.0, 0.0, 0.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badEmissive, "Maps/SC_test/PS_decal_emissive.json"),
        std::invalid_argument);

    Json badEmissiveStrength = document;
    badEmissiveStrength["emitters"][0]["modules"][0]["fields"]["decal"]
        ["emissiveStrength"] = 1001.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            badEmissiveStrength,
            "Maps/SC_test/PS_decal_emissive_strength.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, DecalCapabilitiesExposeFieldsAndRoundTrip)
{
    Json document = DecalEmitterDocument();
    const Json capabilities = Editor::DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json& fields = capabilities.at("emitters").at(0).at("fields");
    const auto field = [&fields](const char* name) -> const Json*
    {
        for (const Json& entry : fields)
        {
            if (entry.value("field", std::string()) == name)
                return &entry;
        }
        return nullptr;
    };
    const Json* mode = field("renderMode");
    ASSERT_NE(mode, nullptr);
    bool exposesDecalMode = false;
    for (const Json& value : mode->at("enum"))
    {
        if (value.get<std::string>() == "decal")
            exposesDecalMode = true;
    }
    EXPECT_TRUE(exposesDecalMode);
    EXPECT_NE(field("decal.size"), nullptr);
    EXPECT_NE(field("decal.rotation"), nullptr);
    EXPECT_NE(field("decal.color"), nullptr);
    EXPECT_NE(field("decal.opacity"), nullptr);
    EXPECT_NE(field("decal.edgeSoftness"), nullptr);
    EXPECT_NE(field("decal.noiseStrength"), nullptr);
    EXPECT_NE(field("decal.roughness"), nullptr);
    EXPECT_NE(field("decal.metallic"), nullptr);
    EXPECT_NE(field("decal.ambientOcclusion"), nullptr);
    EXPECT_NE(field("decal.normalStrength"), nullptr);
    EXPECT_NE(field("decal.emissive"), nullptr);
    EXPECT_NE(field("decal.emissiveStrength"), nullptr);
    EXPECT_NE(field("decal.decalResponse"), nullptr);
    EXPECT_NE(field("decal.albedoMap"), nullptr);
    EXPECT_NE(field("decal.normalMap"), nullptr);
    EXPECT_NE(field("decal.ormMap"), nullptr);
    EXPECT_NE(field("decal.sortOrder"), nullptr);

    EXPECT_EQ(ParticleModuleTypeForField("decal.size"), "renderer");
    EXPECT_EQ(ParticleModuleTypeForField("decal.albedoMap"), "renderer");
    const ParticleModuleSpec* renderer =
        FindParticleModuleSpec("renderer");
    ASSERT_NE(renderer, nullptr);
    EXPECT_NE(
        std::find(renderer->fields.begin(), renderer->fields.end(),
            "decal"),
        renderer->fields.end());

    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange sizeChange;
    sizeChange.field = "emitters.0.decal.size";
    sizeChange.value = std::array<double, 3>{70.0, 320.0, 260.0};
    changes.push_back(sizeChange);
    EditorAssetFieldChange responseChange;
    responseChange.field = "emitters.0.decal.decalResponse";
    responseChange.value = std::string("metallicRoughnessNormal");
    changes.push_back(responseChange);
    EditorAssetFieldChange sortChange;
    sortChange.field = "emitters.0.decal.sortOrder";
    sortChange.value = 11.0;
    changes.push_back(sortChange);
    Editor::ApplyAssetChanges(
        document,
        EditorAssetDocumentKind::ParticleSystem,
        changes);

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_decal_roundtrip.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(asset.emitters[0].decal.size,
        Eigen::Vector3f(70.0f, 320.0f, 260.0f));
    EXPECT_EQ(asset.emitters[0].decal.responseMask,
        DecalResponseMetallic | DecalResponseRoughness |
            DecalResponseNormal);
    EXPECT_EQ(asset.emitters[0].decal.sortOrder, 11u);
    EXPECT_EQ(asset.emitters[0].decal.emissive,
        Eigen::Vector3f(4.0f, 2.0f, 1.0f));
    EXPECT_FLOAT_EQ(asset.emitters[0].decal.emissiveStrength, 6.0f);

    Json emitter = Json{{"name", "scorch"}};
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "decal.size", Json::array({1.0, 2.0, 3.0}), false));
    const Json flattened = FlattenParticleEmitter(emitter);
    EXPECT_EQ(
        flattened["decal"]["size"],
        Json::array({1.0, 2.0, 3.0}));
}

TEST(ParticleDecalRuntime, BurstIsDeterministicAndBuildsDecalTransform)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("scorch", 42, 2, 0.5f);
    emitter.decal.size = Eigen::Vector3f(2.0f, 4.0f, 6.0f);
    emitter.decal.color = Eigen::Vector3f(0.5f, 0.25f, 0.125f);
    emitter.decal.opacity = 0.8f;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 1>(0, 3) =
        Eigen::Vector3f(10.0f, 20.0f, 30.0f);
    RenderScene sceneA;
    sceneA.worldGeneration = 7;
    sceneA.particleSystems.push_back(
        MakeSnapshot("ScorchSystem", emitter, model));
    RenderScene sceneB = sceneA;

    ParticleDecalRuntime runtimeA;
    ParticleDecalRuntime runtimeB;
    UpdateFrames(runtimeA, sceneA, 1, 1.0f / 60.0f);
    UpdateFrames(runtimeB, sceneB, 1, 1.0f / 60.0f);

    ASSERT_EQ(sceneA.decals.size(), 2u);
    ASSERT_EQ(sceneB.decals.size(), 2u);
    std::set<uint64_t> idsA;
    for (size_t index = 0; index < sceneA.decals.size(); ++index)
    {
        const DecalSnapshot& a = sceneA.decals[index];
        const DecalSnapshot& b = sceneB.decals[index];
        idsA.insert(a.decalId);
        EXPECT_EQ(a.decalId, b.decalId);
        EXPECT_EQ(a.color, b.color);
        EXPECT_FLOAT_EQ(a.opacity, b.opacity);
        EXPECT_NEAR(a.decalToWorld(0, 3), 10.0f, 1.0e-5f);
        EXPECT_NEAR(a.decalToWorld(1, 3), 20.0f, 1.0e-5f);
        EXPECT_NEAR(a.decalToWorld(2, 3), 30.0f, 1.0e-5f);
        EXPECT_NEAR((a.decalToWorld.block<3, 1>(0, 0).norm()),
            1.0f, 1.0e-5f);
        EXPECT_NEAR((a.decalToWorld.block<3, 1>(0, 1).norm()),
            2.0f, 1.0e-5f);
        EXPECT_NEAR((a.decalToWorld.block<3, 1>(0, 2).norm()),
            3.0f, 1.0e-5f);
        EXPECT_NEAR(a.color.x(), 0.5f, 1.0e-5f);
        EXPECT_NEAR(a.color.y(), 0.25f, 1.0e-5f);
        EXPECT_NEAR(a.color.z(), 0.125f, 1.0e-5f);
        EXPECT_NEAR(a.opacity, 0.8f, 1.0e-5f);
    }
    EXPECT_EQ(idsA.size(), 2u);

    const std::set<uint64_t> firstIds = idsA;
    UpdateFrames(runtimeA, sceneA, 1, 1.0f / 60.0f);
    EXPECT_EQ(DynamicDecalIds(sceneA, {}), firstIds);
}

TEST(ParticleDecalRuntime, ParentRotationAndDecalRotationCompose)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("oriented", 3, 1, 10.0f);
    emitter.decal.size = Eigen::Vector3f(2.0f, 4.0f, 6.0f);
    emitter.decal.rotation = Eigen::Vector3f(0.0f, 0.0f, 90.0f);
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(kPi * 0.5f, Eigen::Vector3f::UnitY())
            .toRotationMatrix();
    model.block<3, 1>(0, 3) = Eigen::Vector3f(5.0f, 0.0f, 0.0f);
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Oriented", emitter, model));
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 1u);
    const Eigen::Matrix4f& matrix = scene.decals.front().decalToWorld;
    const Eigen::Vector3f axisX =
        matrix.block<3, 1>(0, 0).normalized();
    const Eigen::Vector3f axisY =
        matrix.block<3, 1>(0, 1).normalized();
    // 父级绕 Y 旋转 90°，再叠加 decal 自身绕 Z 旋转 90°。
    EXPECT_NEAR(axisX.x(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(axisX.y(), 1.0f, 1.0e-5f);
    EXPECT_NEAR(axisX.z(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(axisY.x(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(axisY.y(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(axisY.z(), 1.0f, 1.0e-5f);
}

TEST(ParticleDecalRuntime, FixedTimeStepHoldsAndLifetimeRemoves)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("fixed", 5, 1, 0.1f);
    emitter.fixedTimeStep = 0.1f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("FixedSystem", emitter));
    ParticleDecalRuntime runtime;

    UpdateFrames(runtime, scene, 1, 0.05f);
    EXPECT_EQ(runtime.GetLiveDecalCount(), 0u);
    EXPECT_TRUE(scene.decals.empty());

    UpdateFrames(runtime, scene, 1, 0.05f);
    ASSERT_EQ(runtime.GetLiveDecalCount(), 1u);
    ASSERT_EQ(scene.decals.size(), 1u);
    const float opacity = scene.decals.front().opacity;

    UpdateFrames(runtime, scene, 1, 0.05f);
    ASSERT_EQ(scene.decals.size(), 1u);
    EXPECT_FLOAT_EQ(scene.decals.front().opacity, opacity);

    UpdateFrames(runtime, scene, 1, 0.05f);
    EXPECT_EQ(runtime.GetLiveDecalCount(), 0u);
    EXPECT_TRUE(scene.decals.empty());
}

TEST(ParticleDecalRuntime, ShapeGravityDragAndContinuousSpawn)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("falling", 7, 2, 10.0f);
    emitter.shapeType = "box";
    emitter.shapeExtent = Eigen::Vector3f(3.0f, 5.0f, 7.0f);
    emitter.gravity = Eigen::Vector3f(0.0f, -100.0f, 0.0f);
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Falling", emitter));
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 2u);
    for (const DecalSnapshot& decal : scene.decals)
    {
        const Eigen::Vector3f position =
            decal.decalToWorld.block<3, 1>(0, 3);
        EXPECT_LE(std::abs(position.x()), 3.0f);
        EXPECT_LE(std::abs(position.y()), 5.0f);
        EXPECT_LE(std::abs(position.z()), 7.0f);
    }
    const Eigen::Vector3f firstPosition =
        scene.decals.front().decalToWorld.block<3, 1>(0, 3);
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 2u);
    EXPECT_LT(
        scene.decals.front().decalToWorld(1, 3),
        firstPosition.y());

    ParticleEmitterAsset continuous =
        MakeDecalEmitter("continuous", 9, 10, 10.0f);
    continuous.burst = false;
    continuous.spawnRate = 20.0f;
    RenderScene continuousScene;
    continuousScene.particleSystems.push_back(
        MakeSnapshot("Continuous", continuous));
    ParticleDecalRuntime continuousRuntime;
    UpdateFrames(continuousRuntime, continuousScene, 1, 0.1f);
    EXPECT_EQ(continuousRuntime.GetLiveDecalCount(), 2u);
    continuousScene.particleSystems.front().runtime.spawnRateScale = 0.5f;
    UpdateFrames(continuousRuntime, continuousScene, 1, 0.1f);
    EXPECT_EQ(continuousRuntime.GetLiveDecalCount(), 3u);
}

TEST(ParticleDecalRuntime, ColorAndOpacityFollowLifetime)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("colored", 11, 1, 10.0f);
    emitter.decal.color = Eigen::Vector3f(0.5f, 2.0f, 4.0f);
    emitter.decal.opacity = 0.5f;
    emitter.colorStart = Eigen::Vector4f(2.0f, 1.0f, 1.0f, 1.0f);
    emitter.colorEnd = Eigen::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
    RenderScene scene;
    ParticleSystemSnapshot snapshot =
        MakeSnapshot("Colored", emitter);
    snapshot.runtime.colorTint =
        Eigen::Vector4f(1.0f, 2.0f, 0.5f, 0.5f);
    scene.particleSystems.push_back(snapshot);
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 1u);
    EXPECT_NEAR(scene.decals.front().color.x(), 1.0f, 1.0e-5f);
    EXPECT_NEAR(scene.decals.front().color.y(), 4.0f, 1.0e-5f);
    EXPECT_NEAR(scene.decals.front().color.z(), 2.0f, 1.0e-5f);
    EXPECT_NEAR(scene.decals.front().opacity, 0.25f, 1.0e-5f);

    scene.particleSystems.front().runtime.colorTint =
        Eigen::Vector4f::Ones();
    UpdateFrames(runtime, scene, 50, 0.1f);
    ASSERT_EQ(scene.decals.size(), 1u);
    EXPECT_NEAR(scene.decals.front().opacity, 0.25f, 1.0e-4f);
    EXPECT_NEAR(scene.decals.front().color.x(), 0.5f, 1.0e-4f);
}

TEST(ParticleDecalRuntime, EmissiveFieldsReachDecalSnapshot)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("emissive", 23, 1, 10.0f);
    emitter.decal.emissive = Eigen::Vector3f(4.0f, 2.0f, 1.0f);
    emitter.decal.emissiveStrength = 6.0f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Emissive", emitter));
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 1u);
    EXPECT_EQ(scene.decals.front().emissive,
        Eigen::Vector3f(4.0f, 2.0f, 1.0f));
    EXPECT_FLOAT_EQ(scene.decals.front().emissiveStrength, 6.0f);

    scene.particleSystems.front().runtime.colorTint =
        Eigen::Vector4f(2.0f, 1.0f, 0.5f, 1.0f);
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 1u);
    EXPECT_EQ(scene.decals.front().emissive,
        Eigen::Vector3f(8.0f, 2.0f, 0.5f));
}

TEST(ParticleDecalRuntime, DynamicDecalsAreRemovedAndDoNotStack)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("continuous", 13, 4, 10.0f);
    emitter.burst = false;
    emitter.spawnRate = 40.0f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Continuous", emitter));
    ParticleDecalRuntime runtime;
    for (uint32_t frame = 0; frame < 12; ++frame)
    {
        UpdateFrames(runtime, scene, 1, 0.1f);
        EXPECT_EQ(scene.decals.size(), runtime.GetLiveDecalCount());
        EXPECT_LE(scene.decals.size(), 4u);
        std::set<uint64_t> ids;
        for (const DecalSnapshot& decal : scene.decals)
            ids.insert(decal.decalId);
        EXPECT_EQ(ids.size(), scene.decals.size());
    }
}

TEST(ParticleDecalRuntime, StableSlotIdsSurviveBurstRetrigger)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("burst", 17, 2, 10.0f);
    emitter.burstInterval = 0.5f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Burst", emitter));
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), 2u);
    const std::set<uint64_t> firstIds = DynamicDecalIds(scene, {});

    UpdateFrames(runtime, scene, 6, 0.1f);
    ASSERT_EQ(scene.decals.size(), 2u);
    EXPECT_EQ(DynamicDecalIds(scene, {}), firstIds);
}

TEST(ParticleDecalRuntime, StaticDecalsConsumeGlobalCapacity)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("burst", 19, 4, 10.0f);
    RenderScene scene;
    std::set<uint64_t> staticIds;
    for (uint64_t index = 0; index < 63; ++index)
    {
        staticIds.insert(1000 + index);
        scene.decals.push_back(MakeStaticDecal(1000 + index));
    }
    scene.particleSystems.push_back(
        MakeSnapshot("Burst", emitter));
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveDecalCount(), 1u);
    EXPECT_EQ(scene.decals.size(), kMaxDecalInstances);

    staticIds.insert(2000);
    scene.decals.push_back(MakeStaticDecal(2000));
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveDecalCount(), 0u);
    EXPECT_EQ(scene.decals.size(), kMaxDecalInstances);
    for (const DecalSnapshot& decal : scene.decals)
    {
        EXPECT_NE(staticIds.find(decal.decalId), staticIds.end());
    }

    // 容量从初始就是 0 时，initial burst 不能标记为已完成；释放一个
    // 静态槽后下一次 Update 必须补发，否则一次性 burst 会永久丢失。
    RenderScene blockedScene;
    for (uint64_t index = 0; index < kMaxDecalInstances; ++index)
    {
        blockedScene.decals.push_back(MakeStaticDecal(3000 + index));
    }
    blockedScene.particleSystems.push_back(
        MakeSnapshot("Blocked", emitter));
    ParticleDecalRuntime blockedRuntime;
    UpdateFrames(blockedRuntime, blockedScene, 1, 0.1f);
    EXPECT_EQ(blockedRuntime.GetLiveDecalCount(), 0u);
    blockedScene.decals.pop_back();
    UpdateFrames(blockedRuntime, blockedScene, 1, 0.1f);
    EXPECT_EQ(blockedRuntime.GetLiveDecalCount(), 1u);
    EXPECT_EQ(blockedScene.decals.size(), kMaxDecalInstances);

}

TEST(ParticleDecalRuntime, GlobalCapacityTruncatesInEmitterOrder)
{
    ParticleEmitterAsset first =
        MakeDecalEmitter("first", 23, 40, 10.0f);
    ParticleEmitterAsset second =
        MakeDecalEmitter("second", 29, 40, 10.0f);
    RenderScene scene;
    ParticleSystemSnapshot snapshot =
        MakeSnapshot("Many", first);
    snapshot.asset.emitters.push_back(second);
    scene.particleSystems.push_back(snapshot);
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.decals.size(), kMaxDecalInstances);
    uint32_t firstCount = 0;
    uint32_t secondCount = 0;
    for (const DecalSnapshot& decal : scene.decals)
    {
        if (decal.name.rfind("first#", 0) == 0) ++firstCount;
        if (decal.name.rfind("second#", 0) == 0) ++secondCount;
    }
    EXPECT_EQ(firstCount, 40u);
    EXPECT_EQ(secondCount, 24u);
}

TEST(ParticleDecalRuntime, DisableRemovesDecalsAndReenableRespawns)
{
    ParticleEmitterAsset emitter =
        MakeDecalEmitter("toggle", 31, 1, 10.0f);
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Toggle", emitter));
    ParticleDecalRuntime runtime;
    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(runtime.GetLiveDecalCount(), 1u);
    ASSERT_EQ(scene.decals.size(), 1u);

    scene.particleSystems.front().runtime.enabledEmitterMask = 0u;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveDecalCount(), 0u);
    EXPECT_TRUE(scene.decals.empty());

    scene.particleSystems.front().runtime.enabledEmitterMask = 1u;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveDecalCount(), 1u);
    EXPECT_EQ(scene.decals.size(), 1u);
}

TEST(ParticleDecalRuntime, ResolvedAssetsSeparateSameNamedSystems)
{
    ParticleEmitterAsset authored =
        MakeDecalEmitter("scorch", 37, 1, 10.0f);
    RenderScene scene;
    ParticleSystemSnapshot firstSnapshot =
        MakeSnapshot("Decal", authored);
    firstSnapshot.assetPath = "Maps/SC_test/PS_first.json";
    ParticleSystemSnapshot secondSnapshot =
        MakeSnapshot("Decal", authored);
    secondSnapshot.assetPath = "Maps/SC_test/PS_second.json";
    scene.particleSystems = {firstSnapshot, secondSnapshot};

    ParticleEmitterAsset firstEmitter = authored;
    firstEmitter.decal.color = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    ParticleSystemAsset firstResolved;
    firstResolved.emitters.push_back(firstEmitter);
    ParticleEmitterAsset secondEmitter = authored;
    secondEmitter.decal.color = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    ParticleSystemAsset secondResolved;
    secondResolved.emitters.push_back(secondEmitter);
    const std::unordered_map<std::string, ParticleSystemAsset>
        resolvedAssets{
            {BuildParticleSystemAssetKey(
                 "Decal", firstSnapshot.assetPath),
             firstResolved},
            {BuildParticleSystemAssetKey(
                 "Decal", secondSnapshot.assetPath),
             secondResolved}};

    ParticleDecalRuntime runtime;
    runtime.Synchronize(scene, resolvedAssets);
    runtime.Update(scene);
    ASSERT_EQ(scene.decals.size(), 2u);
    EXPECT_NE(scene.decals[0].decalId, scene.decals[1].decalId);
    const auto hasColor = [&scene](const Eigen::Vector3f& expected)
    {
        for (const DecalSnapshot& decal : scene.decals)
        {
            if ((decal.color.head<3>() - expected).norm() < 1.0e-5f)
                return true;
        }
        return false;
    };
    EXPECT_TRUE(hasColor(Eigen::Vector3f(1.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(hasColor(Eigen::Vector3f(0.0f, 1.0f, 0.0f)));
}
