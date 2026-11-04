#include <cmath>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/assetDocumentFields.h"
#include "particles/particleAsset.h"
#include "particles/particleLightRuntime.h"
#include "render/frontend/renderScene.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;

ParticleEmitterAsset MakeLightEmitter(
    const std::string& name,
    uint32_t seed,
    uint32_t maxParticles,
    float lifetime)
{
    ParticleEmitterAsset emitter;
    emitter.name = name;
    emitter.renderMode = "light";
    emitter.hasLight = true;
    emitter.light.intensity = 300.0f;
    emitter.light.radius = 700.0f;
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
    emitter.colorStart = Eigen::Vector4f(4.0f, 2.0f, 0.6f, 1.0f);
    emitter.colorEnd = Eigen::Vector4f(1.0f, 0.1f, 0.0f, 0.0f);
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
    ParticleLightRuntime& runtime,
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

} // namespace

TEST(ParticleAsset, LightModeParsesAndValidates)
{
    const Json document = Json::parse(R"json({
        "name": "PS_light",
        "type": "particleSystem",
        "version": 2,
        "emitters": [{
            "name": "flash_light",
            "modules": [
                {"type": "emitter", "fields": {
                    "maxParticles": 2,
                    "renderMode": "light",
                    "light": {"intensity": 250.0, "radius": 900.0},
                    "burst": true,
                    "additive": false,
                    "sortParticles": false,
                    "lifetime": [0.08, 0.08]}},
                {"type": "spawn.rate", "fields": {
                    "spawnRate": 0.0,
                    "burst": true,
                    "burstInterval": 1.0}},
                {"type": "spawn.shapeSphere", "fields": {
                    "shape": {"type": "sphere", "radius": 3.0}}},
                {"type": "update.color", "fields": {
                    "colorStart": [4.0, 2.0, 0.6, 1.0],
                    "colorEnd": [1.0, 0.1, 0.0, 0.0]}}
            ]
        }]
    })json");
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_light.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    const ParticleEmitterAsset& emitter = asset.emitters.at(0);
    EXPECT_EQ(emitter.renderMode, "light");
    EXPECT_TRUE(emitter.hasLight);
    EXPECT_FLOAT_EQ(emitter.light.intensity, 250.0f);
    EXPECT_FLOAT_EQ(emitter.light.radius, 900.0f);

    Json withoutLight = document;
    withoutLight["emitters"][0]["modules"][0]["fields"].erase("light");
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            withoutLight, "Maps/SC_test/PS_light.json"),
        std::invalid_argument);

    Json tooManyLights = document;
    tooManyLights["emitters"][0]["modules"][0]["fields"]["maxParticles"] =
        65;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            tooManyLights, "Maps/SC_test/PS_light.json"),
        std::invalid_argument);

    Json zeroIntensity = document;
    zeroIntensity["emitters"][0]["modules"][0]["fields"]["light"]
        ["intensity"] = 0.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            zeroIntensity, "Maps/SC_test/PS_light.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, LightCapabilitiesExposeModeAndFields)
{
    const Json document = Json::parse(R"json({
        "name": "PS_light",
        "type": "particleSystem",
        "version": 2,
        "emitters": [{
            "name": "flash_light",
            "modules": [
                {"type": "renderer", "fields": {
                    "renderMode": "light",
                    "light": {"intensity": 300.0, "radius": 700.0}}}
            ]
        }]
    })json");
    const Json capabilities =
        Editor::DescribeAssetCapabilities(
            EditorAssetDocumentKind::ParticleSystem,
            document);
    const Json fields = capabilities.at("emitters").at(0).at("fields");
    bool hasIntensity = false;
    bool hasRadius = false;
    bool hasLightMode = false;
    for (const Json& field : fields)
    {
        const std::string name = field.value("field", "");
        if (name == "light.intensity") hasIntensity = true;
        if (name == "light.radius") hasRadius = true;
        if (name == "renderMode")
        {
            for (const Json& value : field.at("enum"))
            {
                if (value.get<std::string>() == "light")
                {
                    hasLightMode = true;
                }
            }
        }
    }
    EXPECT_TRUE(hasIntensity);
    EXPECT_TRUE(hasRadius);
    EXPECT_TRUE(hasLightMode);

    Json changedDocument = document;
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange intensityChange;
    intensityChange.field = "emitters.0.light.intensity";
    intensityChange.value = 420.0;
    changes.push_back(intensityChange);
    EditorAssetFieldChange radiusChange;
    radiusChange.field = "emitters.0.light.radius";
    radiusChange.value = 950.0;
    changes.push_back(radiusChange);
    Editor::ApplyAssetChanges(
        changedDocument,
        EditorAssetDocumentKind::ParticleSystem,
        changes);
    EXPECT_DOUBLE_EQ(
        changedDocument["emitters"][0]["modules"][0]["fields"]["light"]
            ["intensity"].get<double>(),
        420.0);
    EXPECT_DOUBLE_EQ(
        changedDocument["emitters"][0]["modules"][0]["fields"]["light"]
            ["radius"].get<double>(),
        950.0);
}

TEST(ParticleLightRuntime, BurstIsDeterministicAndUsesEmitterModel)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("flash_light", 42, 2, 0.5f);
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 1>(0, 3) = Eigen::Vector3f(10.0f, 20.0f, 30.0f);
    RenderScene sceneA;
    sceneA.worldGeneration = 7;
    sceneA.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter, model));
    RenderScene sceneB = sceneA;

    ParticleLightRuntime runtimeA;
    ParticleLightRuntime runtimeB;
    UpdateFrames(runtimeA, sceneA, 3, 1.0f / 60.0f);
    UpdateFrames(runtimeB, sceneB, 3, 1.0f / 60.0f);

    ASSERT_EQ(sceneA.lights.size(), 2u);
    ASSERT_EQ(sceneB.lights.size(), sceneA.lights.size());
    for (size_t index = 0; index < sceneA.lights.size(); ++index)
    {
        const LightSnapshot& a = sceneA.lights[index];
        const LightSnapshot& b = sceneB.lights[index];
        EXPECT_EQ(a.type, LightSnapshotType::Point);
        EXPECT_EQ(a.lightId, b.lightId);
        EXPECT_FLOAT_EQ(a.intensity, b.intensity);
        EXPECT_FLOAT_EQ(a.radius, 700.0f);
        EXPECT_NEAR(a.position.x(), 10.0f, 1.0e-5f);
        EXPECT_NEAR(a.position.y(), 20.0f, 1.0e-5f);
        EXPECT_NEAR(a.position.z(), 30.0f, 1.0e-5f);
        EXPECT_NEAR(b.position.x(), a.position.x(), 1.0e-6f);
        EXPECT_NEAR(b.position.y(), a.position.y(), 1.0e-6f);
        EXPECT_NEAR(b.position.z(), a.position.z(), 1.0e-6f);
    }
}

TEST(ParticleLightRuntime, LifetimeRemovesLights)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("short_light", 7, 2, 0.1f);
    RenderScene scene;
    scene.worldGeneration = 1;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));

    ParticleLightRuntime runtime;
    UpdateFrames(runtime, scene, 1, 1.0f / 60.0f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 2u);
    EXPECT_EQ(scene.lights.size(), 2u);

    UpdateFrames(runtime, scene, 8, 1.0f / 60.0f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 0u);
    EXPECT_TRUE(scene.lights.empty());
}

TEST(ParticleLightRuntime, BurstRetriggersWholePool)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("flash", 17u, 2u, 1.0f);
    emitter.burstInterval = 0.5f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("FlashSystem", emitter));
    ParticleLightRuntime runtime;

    runtime.Synchronize(scene);
    runtime.Update(scene);
    ASSERT_EQ(runtime.GetLiveLightCount(), 2u);
    ASSERT_EQ(scene.lights.size(), 2u);
    const uint64_t firstId = scene.lights[0].lightId;

    // 单帧 delta 会被运行时裁到 0.1 s；0.6 s 足以跨过 0.5 s 周期。
    UpdateFrames(runtime, scene, 6, 0.1f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 2u);
    ASSERT_EQ(scene.lights.size(), 2u);
    EXPECT_NE(scene.lights[0].lightId, firstId);
}

TEST(ParticleLightRuntime, GlobalCapIsSixtyFour)
{
    ParticleEmitterAsset first =
        MakeLightEmitter("light_a", 1, 40, 10.0f);
    ParticleEmitterAsset second =
        MakeLightEmitter("light_b", 2, 40, 10.0f);
    RenderScene scene;
    scene.worldGeneration = 4;
    ParticleSystemSnapshot snapshot =
        MakeSnapshot("Lamp", first);
    snapshot.asset.emitters.push_back(second);
    scene.particleSystems.push_back(snapshot);

    ParticleLightRuntime runtime;
    UpdateFrames(runtime, scene, 1, 1.0f / 60.0f);
    EXPECT_EQ(runtime.GetTrackedEmitterCount(), 2u);
    EXPECT_EQ(runtime.GetLiveLightCount(), 64u);
    EXPECT_EQ(scene.lights.size(), 64u);
}

TEST(ParticleLightRuntime, WorldGenerationChangeResetsState)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("flash_light", 9, 1, 10.0f);
    RenderScene scene;
    scene.worldGeneration = 10;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));
    ParticleLightRuntime runtime;
    UpdateFrames(runtime, scene, 1, 1.0f / 60.0f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 1u);

    LightSnapshot staticLight;
    staticLight.lightId = 12345;
    staticLight.type = LightSnapshotType::Point;
    staticLight.position = Eigen::Vector3f::Zero();
    scene.lights.push_back(staticLight);

    scene.worldGeneration = 11;
    scene.simulationDeltaSeconds = 1.0f / 60.0f;
    runtime.Synchronize(scene);
    EXPECT_EQ(runtime.GetLiveLightCount(), 0u);
    runtime.Update(scene);
    // 旧代际的动态灯已移除；静态灯保留，新代际重新生成一盏 burst 灯。
    EXPECT_EQ(scene.lights.size(), 2u);
    bool hasStatic = false;
    for (const LightSnapshot& light : scene.lights)
    {
        if (light.lightId == staticLight.lightId)
        {
            hasStatic = true;
        }
    }
    EXPECT_TRUE(hasStatic);
}

TEST(ParticleLightRuntime, LocalSpaceLightsFollowEmitterScale)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("local_light", 3, 1, 10.0f);
    emitter.localSpace = true;
    emitter.velocityMin = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    emitter.velocityMax = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 0) = 2.0f;
    model(1, 1) = 2.0f;
    model(2, 2) = 2.0f;
    model.block<3, 1>(0, 3) = Eigen::Vector3f(5.0f, 0.0f, 0.0f);
    RenderScene scene;
    scene.worldGeneration = 3;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter, model));

    ParticleLightRuntime runtime;
    UpdateFrames(runtime, scene, 2, 0.1f);
    ASSERT_EQ(scene.lights.size(), 1u);
    // localSpace：位置 0.1 s * 1 cm/s = 0.1，模型缩放 2 后为 0.2，
    // 再加平移 5 cm。
    EXPECT_NEAR(scene.lights.front().position.x(), 5.2f, 1.0e-4f);
}

TEST(ParticleLightRuntime, FixedTimeStepHoldsStateBetweenWholeSteps)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("fixed_light", 21, 1, 1.0f);
    emitter.fixedTimeStep = 0.1f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));
    ParticleLightRuntime runtime;

    UpdateFrames(runtime, scene, 1, 0.05f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 0u);
    EXPECT_TRUE(scene.lights.empty());

    UpdateFrames(runtime, scene, 1, 0.05f);
    ASSERT_EQ(runtime.GetLiveLightCount(), 1u);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_FLOAT_EQ(scene.lights.front().intensity, 300.0f);

    // 不足一步的累积不会老化灯光，也不会重新 burst。
    UpdateFrames(runtime, scene, 1, 0.05f);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_FLOAT_EQ(scene.lights.front().intensity, 300.0f);

    UpdateFrames(runtime, scene, 1, 0.05f);
    ASSERT_EQ(scene.lights.size(), 1u);
    // alpha 从 1 线性降到 0，因此 intensity 同时乘 age 与 alpha：
    // 300 * (1 - 0.1) * (1 - 0.1) = 243。
    EXPECT_FLOAT_EQ(scene.lights.front().intensity, 243.0f);
}

TEST(ParticleLightRuntime, ContinuousSpawnRateAndRuntimeScale)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("continuous_light", 31, 10, 10.0f);
    emitter.burst = false;
    emitter.spawnRate = 20.0f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));
    ParticleLightRuntime runtime;

    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 2u);

    scene.particleSystems.front().runtime.spawnRateScale = 0.5f;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 3u);

    scene.particleSystems.front().runtime.spawnRateScale = 0.0f;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 3u);
}

TEST(ParticleLightRuntime, GravityDragAndVelocityAreIntegrated)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("moving_light", 41, 1, 10.0f);
    emitter.velocityMin = Eigen::Vector3f(100.0f, 0.0f, 0.0f);
    emitter.velocityMax = emitter.velocityMin;
    emitter.gravity = Eigen::Vector3f(0.0f, -100.0f, 0.0f);
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));
    ParticleLightRuntime runtime;

    // 第一帧只负责生成；下一帧才把初速度/重力积分到位置。
    UpdateFrames(runtime, scene, 2, 0.1f);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_NEAR(scene.lights.front().position.x(), 10.0f, 1.0e-4f);
    EXPECT_NEAR(scene.lights.front().position.y(), -1.0f, 1.0e-4f);

    emitter.drag = 2.0f;
    emitter.randomSeed = 42;
    RenderScene draggedScene;
    draggedScene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));
    ParticleLightRuntime draggedRuntime;
    UpdateFrames(draggedRuntime, draggedScene, 2, 0.1f);
    ASSERT_EQ(draggedScene.lights.size(), 1u);
    EXPECT_LT(draggedScene.lights.front().position.x(), 10.0f);
    EXPECT_GT(draggedScene.lights.front().position.x(), 0.0f);
}

TEST(ParticleLightRuntime, ShapeSpawnersStayInTheirVolumes)
{
    struct ShapeExpectation
    {
        const char* shape;
        bool planar;
        float radialMinimum;
    };
    const ShapeExpectation shapes[] = {
        {"sphere", false, 0.0f},
        {"box", false, 0.0f},
        {"cone", false, 0.0f},
        {"cylinder", false, 0.0f},
        {"disc", true, 0.0f},
        {"ring", true, 4.0f}};
    for (const ShapeExpectation& expectation : shapes)
    {
        ParticleEmitterAsset emitter = MakeLightEmitter(
            std::string("shape_") + expectation.shape,
            51u, 1u, 10.0f);
        emitter.shapeType = expectation.shape;
        emitter.shapeRadius = 10.0f;
        emitter.shapeHeight = 20.0f;
        emitter.shapeExtent = Eigen::Vector3f(3.0f, 5.0f, 7.0f);
        emitter.shapeInnerRadius = 4.0f;
        RenderScene scene;
        scene.particleSystems.push_back(
            MakeSnapshot("Lamp", emitter));
        ParticleLightRuntime runtime;
        UpdateFrames(runtime, scene, 1, 0.1f);
        ASSERT_EQ(scene.lights.size(), 1u);
        const Eigen::Vector3f position =
            scene.lights.front().position;
        const float radial =
            std::sqrt(position.x() * position.x() +
                position.z() * position.z());
        if (expectation.planar)
        {
            EXPECT_NEAR(position.y(), 0.0f, 1.0e-5f)
                << expectation.shape;
        }
        if (std::string(expectation.shape) == "box")
        {
            EXPECT_LE(std::abs(position.x()), 3.0f);
            EXPECT_LE(std::abs(position.y()), 5.0f);
            EXPECT_LE(std::abs(position.z()), 7.0f);
        }
        else if (std::string(expectation.shape) == "cone")
        {
            EXPECT_GE(position.y(), 0.0f);
            EXPECT_LE(position.y(), 20.0f);
            EXPECT_LE(radial, 10.0f);
        }
        else if (std::string(expectation.shape) == "cylinder")
        {
            EXPECT_LE(std::abs(position.y()), 10.0f);
            EXPECT_LE(radial, 10.0f);
        }
        else
        {
            EXPECT_LE(radial, 10.0f);
            EXPECT_GE(radial, expectation.radialMinimum);
        }
    }
}

TEST(ParticleLightRuntime, ColorIntensityRadiusAndTintReachSnapshot)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("colored_light", 61, 1, 10.0f);
    emitter.colorStart = Eigen::Vector4f(4.0f, 2.0f, 0.6f, 1.0f);
    emitter.colorEnd = Eigen::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
    emitter.light.intensity = 300.0f;
    emitter.light.radius = 700.0f;
    RenderScene scene;
    ParticleSystemSnapshot snapshot = MakeSnapshot("Lamp", emitter);
    snapshot.runtime.colorTint =
        Eigen::Vector4f(2.0f, 0.5f, 1.0f, 1.0f);
    scene.particleSystems.push_back(snapshot);
    ParticleLightRuntime runtime;

    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_NEAR(scene.lights.front().color.x(), 8.0f, 1.0e-4f);
    EXPECT_NEAR(scene.lights.front().color.y(), 1.0f, 1.0e-4f);
    EXPECT_NEAR(scene.lights.front().color.z(), 0.6f, 1.0e-4f);
    EXPECT_FLOAT_EQ(scene.lights.front().intensity, 300.0f);
    EXPECT_FLOAT_EQ(scene.lights.front().radius, 700.0f);

    scene.particleSystems.front().runtime.colorTint =
        Eigen::Vector4f::Ones();
    // lifetime = 10 s，推进 5 s（每帧被安全上限裁到 0.1 s）。
    UpdateFrames(runtime, scene, 50, 0.1f);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_NEAR(scene.lights.front().intensity, 75.0f, 1.0e-3f);
}

TEST(ParticleLightRuntime, DisableRemovesLightsAndReenableRespawns)
{
    ParticleEmitterAsset emitter =
        MakeLightEmitter("toggle_light", 71, 1, 10.0f);
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", emitter));
    ParticleLightRuntime runtime;

    UpdateFrames(runtime, scene, 1, 0.1f);
    ASSERT_EQ(runtime.GetLiveLightCount(), 1u);
    scene.particleSystems.front().runtime.enabledEmitterMask = 0u;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 0u);
    EXPECT_TRUE(scene.lights.empty());

    scene.particleSystems.front().runtime.enabledEmitterMask = 1u;
    UpdateFrames(runtime, scene, 1, 0.1f);
    EXPECT_EQ(runtime.GetLiveLightCount(), 1u);
    EXPECT_EQ(scene.lights.size(), 1u);
}

TEST(ParticleLightRuntime, ResolvedAssetOverridesSnapshotEmitter)
{
    ParticleEmitterAsset snapshotEmitter =
        MakeLightEmitter("flash_light", 81, 1, 10.0f);
    snapshotEmitter.light.intensity = 111.0f;
    RenderScene scene;
    scene.particleSystems.push_back(
        MakeSnapshot("Lamp", snapshotEmitter));

    ParticleSystemAsset resolved;
    resolved.name = "PS_Lamp";
    ParticleEmitterAsset resolvedEmitter = snapshotEmitter;
    resolvedEmitter.light.intensity = 432.0f;
    resolved.emitters.push_back(resolvedEmitter);
    const std::unordered_map<std::string, ParticleSystemAsset>
        resolvedAssets{{"Lamp", resolved}};
    ParticleLightRuntime runtime;
    runtime.Synchronize(scene, resolvedAssets);
    runtime.Update(scene);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_FLOAT_EQ(scene.lights.front().intensity, 432.0f);
}
