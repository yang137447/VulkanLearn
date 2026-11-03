#include <limits>

#include <gtest/gtest.h>

#include "commonFunction.h"
#include "particles/particleLimits.h"
#include "particles/particleEmitterKey.h"
#include "sceneNode.h"
#include "world/world.h"
#include "world/particleRuntimeBridge.h"
#include "world/worldSnapshotBuilder.h"

namespace
{

VL::WorldParticleSystem MakeParticleSystemInstance(
    const std::string& name,
    const std::string& assetPath)
{
    VL::WorldParticleSystem system;
    system.name = name;
    system.assetPath = assetPath;
    return system;
}

VL::WorldParticleSystem MakeParticleSystemWithEmitters(
    const std::string& name,
    const std::string& assetPath)
{
    VL::WorldParticleSystem system =
        MakeParticleSystemInstance(name, assetPath);
    VL::ParticleEmitterAsset first;
    first.name = "first";
    first.maxParticles = 128;
    VL::ParticleEmitterAsset second;
    second.name = "second";
    second.maxParticles = 64;
    system.asset.emitters = {first, second};
    return system;
}

VL::World MakeParticleWorld()
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    auto camera = std::make_shared<Camera>();
    // 快照构建要求 World 有相机；测试不依赖 config/ 的工作目录。
    camera->SetProjection(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    world.SetCamera(std::move(camera));
    return world;
}

} // namespace

TEST(ParticleWorldRuntime, RemoveParticleSystemRemovesMatchingName)
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    world.AddParticleSystem(
        "Spark_B",
        MakeParticleSystemInstance(
            "Spark_B", "Maps/PS_sparks.json"));

    EXPECT_TRUE(world.RemoveParticleSystem("Spark_A"));
    EXPECT_EQ(world.FindParticleSystem("Spark_A"), nullptr);
    EXPECT_NE(world.FindParticleSystem("Spark_B"), nullptr);
    EXPECT_EQ(world.GetParticleSystems().size(), 1u);
}

TEST(ParticleWorldRuntime, RemoveMissingParticleSystemReturnsFalse)
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    EXPECT_FALSE(world.RemoveParticleSystem("Spark_Missing"));
    EXPECT_NE(world.FindParticleSystem("Spark_A"), nullptr);
}

TEST(ParticleWorldRuntime, AddParticleSystemWithSameNameReplaces)
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_fire.json"));

    ASSERT_EQ(world.GetParticleSystems().size(), 1u);
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->assetPath, "Maps/PS_fire.json");
}

TEST(ParticleWorldRuntime, ParticleSystemMapKeepsInsertionIdentity)
{
    VL::World world(4, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Campfire",
        MakeParticleSystemInstance(
            "Campfire", "Maps/PS_campfire.json"));

    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Campfire");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->name, "Campfire");
    EXPECT_EQ(system->assetPath, "Maps/PS_campfire.json");
}

TEST(ParticleWorldRuntime, TransformUpdatesSceneTrsAndModel)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    const Eigen::Vector3f position(1.5f, -2.0f, 3.25f);
    const Eigen::Vector3f rotation(10.0f, 20.0f, -30.0f);
    const Eigen::Vector3f scale(2.0f, 0.5f, -1.25f);
    ASSERT_TRUE(world.SetParticleSystemTransform(
        "Spark_A", position, rotation, scale));

    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_TRUE(system->scenePosition.isApprox(position));
    EXPECT_TRUE(system->sceneRotation.isApprox(rotation));
    EXPECT_TRUE(system->sceneScale.isApprox(scale));

    Eigen::Matrix4f scaleMatrix = Eigen::Matrix4f::Identity();
    scaleMatrix(0, 0) = scale.x();
    scaleMatrix(1, 1) = scale.y();
    scaleMatrix(2, 2) = scale.z();
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = position;
    const Eigen::Matrix4f expected = translation *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
    EXPECT_TRUE(system->model.isApprox(expected, 1.0e-5f));
}

TEST(ParticleWorldRuntime, TransformRejectsMissingSystemAndInvalidScale)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    const Eigen::Matrix4f originalModel = system->model;

    const Eigen::Vector3f position = Eigen::Vector3f::Zero();
    const Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
    EXPECT_FALSE(world.SetParticleSystemTransform(
        "Spark_Missing", position, rotation, Eigen::Vector3f::Ones()));
    EXPECT_FALSE(world.SetParticleSystemTransform(
        "Spark_A", position, rotation, Eigen::Vector3f(1.0f, 0.0f, 1.0f)));
    EXPECT_FALSE(world.SetParticleSystemTransform(
        "Spark_A", position, rotation,
        Eigen::Vector3f(1.0f, 1.0f, 1.0e-7f)));

    // 拒绝必须是无副作用的：失败后模型与作者 TRS 保持原值。
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_TRUE(system->model.isApprox(originalModel));
    EXPECT_TRUE(system->scenePosition.isZero());
    EXPECT_TRUE(system->sceneRotation.isZero());
    EXPECT_TRUE(system->sceneScale.isOnes());
}

TEST(ParticleWorldRuntime, VisibilityToggleUpdatesOnlyMatchingSystem)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    world.AddParticleSystem(
        "Spark_B",
        MakeParticleSystemInstance(
            "Spark_B", "Maps/PS_fire.json"));

    EXPECT_TRUE(world.FindParticleSystem("Spark_A")->visible);
    EXPECT_TRUE(world.SetParticleSystemVisible("Spark_A", false));
    EXPECT_FALSE(world.FindParticleSystem("Spark_A")->visible);
    EXPECT_TRUE(world.FindParticleSystem("Spark_B")->visible);
    EXPECT_FALSE(world.SetParticleSystemVisible("Spark_Missing", false));

    EXPECT_TRUE(world.SetParticleSystemVisible("Spark_A", true));
    EXPECT_TRUE(world.FindParticleSystem("Spark_A")->visible);
}

TEST(ParticleWorldRuntime, SnapshotCarriesHiddenParticleSystem)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    ASSERT_TRUE(world.SetParticleSystemVisible("Spark_A", false));

    VL::WorldSnapshotBuilder builder;
    VL::WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    auto result = builder.Build(world, desc);
    ASSERT_FALSE(result.IsFailure());

    const VL::WorldSnapshot& snapshot = result.Value();
    ASSERT_EQ(snapshot.particleSystems.size(), 1u);
    const VL::ParticleSystemSnapshot& particle =
        snapshot.particleSystems.front();
    EXPECT_EQ(particle.name, "Spark_A");
    EXPECT_FALSE(particle.visible);
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_TRUE(particle.model.isApprox(system->model));
}

TEST(ParticleWorldRuntime, RuntimeSpawnRateScaleAcceptsAndRejectsOutOfRange)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    ASSERT_TRUE(world.SetParticleSystemSpawnRateScale("Spark_A", 2.5f));
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_FLOAT_EQ(system->runtime.spawnRateScale, 2.5f);

    // 拒绝必须无副作用：越界与非有限值保持上一次成功值。
    EXPECT_FALSE(world.SetParticleSystemSpawnRateScale("Spark_A", -0.01f));
    EXPECT_FALSE(world.SetParticleSystemSpawnRateScale("Spark_A", 16.01f));
    EXPECT_FALSE(world.SetParticleSystemSpawnRateScale(
        "Spark_A",
        std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(world.SetParticleSystemSpawnRateScale(
        "Spark_A",
        std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(world.SetParticleSystemSpawnRateScale("Spark_Missing", 1.0f));

    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_FLOAT_EQ(system->runtime.spawnRateScale, 2.5f);

    // 边界值本身是合法的。
    EXPECT_TRUE(world.SetParticleSystemSpawnRateScale("Spark_A", 0.0f));
    EXPECT_TRUE(world.SetParticleSystemSpawnRateScale("Spark_A", 16.0f));
    EXPECT_FLOAT_EQ(
        world.FindParticleSystem("Spark_A")->runtime.spawnRateScale,
        16.0f);
}

TEST(ParticleWorldRuntime, RuntimeColorTintAcceptsAndRejectsInvalid)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    const Eigen::Vector4f tint(1.5f, 0.25f, 0.0f, 0.75f);
    ASSERT_TRUE(world.SetParticleSystemColorTint("Spark_A", tint));
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_TRUE(system->runtime.colorTint.isApprox(tint));

    EXPECT_FALSE(world.SetParticleSystemColorTint(
        "Spark_A", Eigen::Vector4f(-0.01f, 1.0f, 1.0f, 1.0f)));
    EXPECT_FALSE(world.SetParticleSystemColorTint(
        "Spark_A",
        Eigen::Vector4f(
            std::numeric_limits<float>::quiet_NaN(),
            0.0f,
            0.0f,
            1.0f)));
    EXPECT_FALSE(world.SetParticleSystemColorTint(
        "Spark_A", Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.01f)));
    EXPECT_FALSE(world.SetParticleSystemColorTint(
        "Spark_A", Eigen::Vector4f(1.0f, 1.0f, 1.0f, -0.01f)));
    EXPECT_FALSE(world.SetParticleSystemColorTint(
        "Spark_Missing", Eigen::Vector4f::Ones()));

    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_TRUE(system->runtime.colorTint.isApprox(tint));
}

TEST(ParticleWorldRuntime, RuntimeEmitterMaskTogglesOnlyTargetBit)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    const uint32_t fullMask =
        (1u << VL::kMaxParticleEmitters) - 1u;
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->runtime.enabledEmitterMask, fullMask);

    ASSERT_TRUE(world.SetParticleSystemEmitterEnabled("Spark_A", 3u, false));
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->runtime.enabledEmitterMask, fullMask & ~(1u << 3));
    EXPECT_NE(system->runtime.enabledEmitterMask & (1u << 2), 0u);
    EXPECT_NE(system->runtime.enabledEmitterMask & (1u << 4), 0u);

    ASSERT_TRUE(world.SetParticleSystemEmitterEnabled("Spark_A", 3u, true));
    EXPECT_EQ(
        world.FindParticleSystem("Spark_A")->runtime.enabledEmitterMask,
        fullMask);

    EXPECT_FALSE(world.SetParticleSystemEmitterEnabled(
        "Spark_A", VL::kMaxParticleEmitters, false));
    EXPECT_FALSE(world.SetParticleSystemEmitterEnabled(
        "Spark_Missing", 0u, false));
    EXPECT_EQ(
        world.FindParticleSystem("Spark_A")->runtime.enabledEmitterMask,
        fullMask);
}

TEST(ParticleWorldRuntime, RuntimeOverridesResetRestoresDefaults)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    ASSERT_TRUE(world.SetParticleSystemSpawnRateScale("Spark_A", 4.0f));
    ASSERT_TRUE(world.SetParticleSystemColorTint(
        "Spark_A", Eigen::Vector4f(1.0f, 0.5f, 0.25f, 0.5f)));
    ASSERT_TRUE(world.SetParticleSystemEmitterEnabled("Spark_A", 1u, false));

    ASSERT_TRUE(world.ResetParticleSystemRuntimeOverrides("Spark_A"));

    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_FLOAT_EQ(system->runtime.spawnRateScale, 1.0f);
    EXPECT_TRUE(system->runtime.colorTint.isOnes());
    EXPECT_EQ(
        system->runtime.enabledEmitterMask,
        (1u << VL::kMaxParticleEmitters) - 1u);

    EXPECT_FALSE(world.ResetParticleSystemRuntimeOverrides("Spark_Missing"));
}

TEST(ParticleWorldRuntime, SnapshotCarriesRuntimeOverrides)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    ASSERT_TRUE(world.SetParticleSystemSpawnRateScale("Spark_A", 3.5f));
    const Eigen::Vector4f tint(0.5f, 1.0f, 2.0f, 0.25f);
    ASSERT_TRUE(world.SetParticleSystemColorTint("Spark_A", tint));
    ASSERT_TRUE(world.SetParticleSystemEmitterEnabled("Spark_A", 2u, false));

    VL::WorldSnapshotBuilder builder;
    VL::WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    auto result = builder.Build(world, desc);
    ASSERT_FALSE(result.IsFailure());

    const VL::WorldSnapshot& snapshot = result.Value();
    ASSERT_EQ(snapshot.particleSystems.size(), 1u);
    const VL::ParticleSystemSnapshot& particle =
        snapshot.particleSystems.front();
    EXPECT_FLOAT_EQ(particle.runtime.spawnRateScale, 3.5f);
    EXPECT_TRUE(particle.runtime.colorTint.isApprox(tint));
    EXPECT_EQ(
        particle.runtime.enabledEmitterMask,
        ((1u << VL::kMaxParticleEmitters) - 1u) & ~(1u << 2));
}

TEST(ParticleWorldRuntime, EmitterLifecycleKeepsStableIndices)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemWithEmitters(
            "Spark_A", "Maps/PS_sparks.json"));

    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    ASSERT_EQ(system->emitterSlots.size(), 2u);
    EXPECT_EQ(system->asset.emitters.size(), 2u);

    ASSERT_TRUE(world.DestroyParticleEmitter("Spark_A", 0u));
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    ASSERT_EQ(system->emitterSlots.size(), 2u);
    EXPECT_FALSE(system->emitterSlots[0].active);
    EXPECT_EQ(system->asset.emitters.size(), 1u);
    EXPECT_EQ(system->asset.emitters[0].name, "second");
    EXPECT_EQ(system->telemetry.emitters[0].active, false);
    EXPECT_EQ(system->telemetry.emitters[1].active, true);

    ASSERT_TRUE(world.SetParticleSystemEmitterEnabled("Spark_A", 1u, false));
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->runtime.enabledEmitterMask, 0u);
    EXPECT_FALSE(world.SetParticleSystemEmitterEnabled("Spark_A", 0u, true));

    uint32_t stableIndex = 0;
    ASSERT_TRUE(world.SpawnParticleEmitter(
        "Spark_A", 1u, "runtime.extra", stableIndex));
    EXPECT_EQ(stableIndex, 2u);
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    ASSERT_EQ(system->emitterSlots.size(), 3u);
    EXPECT_EQ(system->asset.emitters.size(), 2u);
    EXPECT_EQ(system->asset.emitters[1].name, "runtime.extra");
    EXPECT_TRUE(world.FindParticleEmitterIndex(
        "Spark_A", "runtime.extra", stableIndex));
    EXPECT_EQ(stableIndex, 2u);

    ASSERT_TRUE(world.RestartParticleEmitter("Spark_A", 2u));
    EXPECT_EQ(
        world.FindParticleSystem("Spark_A")->emitterSlots[2].restartEpoch,
        1u);
    EXPECT_FALSE(world.RestartParticleEmitter("Spark_A", 0u));
    EXPECT_FALSE(world.DestroyParticleEmitter("Spark_A", 9u));
    EXPECT_FALSE(world.SpawnParticleEmitter(
        "Spark_A", 2u, "runtime.extra", stableIndex));

    ASSERT_TRUE(world.SetParticleSystemVisible("Spark_A", false));
    EXPECT_EQ(
        world.FindParticleSystem("Spark_A")->telemetry.visibleEmitterCount,
        0u);
    ASSERT_TRUE(world.SetParticleSystemVisible("Spark_A", true));
    EXPECT_EQ(
        world.FindParticleSystem("Spark_A")->telemetry.visibleEmitterCount,
        1u);
}

TEST(ParticleWorldRuntime, AttachmentResolvesMeshAndComponent)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    VL::WorldMeshObject anchor;
    anchor.sceneObjectIdentity = "mesh|Anchor";
    anchor.model = Eigen::Matrix4f::Identity();
    anchor.model(1, 3) = 100.0f;
    world.AddMeshObject("Anchor", anchor);

    ASSERT_TRUE(world.AttachParticleSystem(
        "Spark_A", "mesh", "mesh|Anchor", "",
        Eigen::Vector3f(0.0f, 25.0f, 0.0f),
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Ones()));
    bool resolved = false;
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    const Eigen::Matrix4f attached =
        world.ResolveParticleSystemModel(*system, resolved);
    EXPECT_TRUE(resolved);
    EXPECT_FLOAT_EQ(attached(1, 3), 125.0f);

    // 组件矩阵由正式 Blueprint 可视化同步路径写入，测试不直接碰私有表。
    const nlohmann::json componentBlueprint{
        {"id", "bp"},
        {"position", {0.0, 0.0, 0.0}},
        {"rotation", {0.0, 0.0, 0.0}},
        {"scale", {1.0, 1.0, 1.0}},
        {"components", nlohmann::json::array({
            {{"id", "arm"},
             {"type", "mesh"},
             {"modelPath", "Common/Meshes/SM_test.json"},
             {"position", {30.0, 0.0, 0.0}}}})}};
    world.ApplyScriptBlueprintVisuals(componentBlueprint, false);
    ASSERT_TRUE(world.AttachParticleSystem(
        "Spark_A", "component", "bp", "arm",
        Eigen::Vector3f(5.0f, 0.0f, 0.0f),
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Ones()));
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    const Eigen::Matrix4f component =
        world.ResolveParticleSystemModel(*system, resolved);
    EXPECT_TRUE(resolved);
    EXPECT_FLOAT_EQ(component(0, 3), 35.0f);

    ASSERT_TRUE(world.AttachParticleSystem(
        "Spark_A", "mesh", "mesh|Missing", "",
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Ones()));
    system = world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    const Eigen::Matrix4f missing =
        world.ResolveParticleSystemModel(*system, resolved);
    EXPECT_FALSE(resolved);
    EXPECT_TRUE(missing.isApprox(system->model));

    EXPECT_FALSE(world.AttachParticleSystem(
        "Spark_A", "mesh", "mesh|Anchor", "",
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f(1.0f, 0.0f, 1.0f)));
    ASSERT_TRUE(world.DetachParticleSystem("Spark_A"));
    EXPECT_FALSE(world.FindParticleSystem("Spark_A")->attachment.IsEnabled());
}

TEST(ParticleWorldRuntime, BudgetsAndUserParametersReachSnapshot)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemWithEmitters(
            "Spark_A", "Maps/PS_sparks.json"));

    ASSERT_TRUE(world.SetWorldParticleBudget(200u, 0.5f));
    ASSERT_TRUE(world.SetParticleSystemBudget("Spark_A", 100u, 0.5f));
    ASSERT_TRUE(world.SetParticleSystemUserParameter("Spark_A", "Wind", 2.5f));
    EXPECT_FALSE(world.SetParticleSystemUserParameter(
        "Spark_A", "", 1.0f));
    EXPECT_FALSE(world.SetParticleSystemUserParameter(
        "Spark_A", "Bad", std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(world.SetParticleSystemBudget("Spark_A", 0u, 16.1f));
    EXPECT_FALSE(world.SetParticleSystemBudget("Spark_A", 1000001u, 1.0f));
    EXPECT_FALSE(world.SetWorldParticleBudget(1000001u, 1.0f));
    EXPECT_FALSE(world.SetWorldParticleBudget(0u, -0.1f));

    VL::WorldSnapshotBuilder builder;
    VL::WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    auto result = builder.Build(world, desc);
    ASSERT_FALSE(result.IsFailure());
    const VL::ParticleSystemSnapshot& particle =
        result.Value().particleSystems.front();
    EXPECT_EQ(particle.budget.maxTotalParticles, 100u);
    // 实例 0.5 与世界 0.5 相乘，最终缩放是 0.25。
    EXPECT_FLOAT_EQ(particle.budget.emitterBudgetScale, 0.25f);
    EXPECT_FLOAT_EQ(particle.userParameters.at("Wind"), 2.5f);
    EXPECT_EQ(particle.telemetry.activeEmitterCount, 2u);
    EXPECT_EQ(particle.telemetry.authoredCapacity, 192u);
    EXPECT_GT(particle.telemetry.budgetedCapacity, 0u);

    ASSERT_TRUE(world.ClearParticleSystemUserParameter("Spark_A", "Wind"));
    EXPECT_TRUE(world.FindParticleSystem("Spark_A")->userParameters.empty());
}

TEST(ParticleWorldRuntime, BeamDoesNotConsumeParticleBudget)
{
    VL::World world = MakeParticleWorld();
    VL::WorldParticleSystem system =
        MakeParticleSystemInstance(
            "Arc", "Maps/PS_tesla.json");
    VL::ParticleEmitterAsset beam;
    beam.name = "arc";
    beam.renderMode = "beam";
    beam.maxParticles = 5;
    VL::ParticleEmitterAsset smoke;
    smoke.name = "smoke";
    smoke.maxParticles = 128;
    system.asset.emitters = {beam, smoke};
    world.AddParticleSystem("Arc", system);

    ASSERT_TRUE(world.SetWorldParticleBudget(200u, 1.0f));
    VL::WorldSnapshotBuilder builder;
    const auto result = builder.Build(world, {});
    ASSERT_FALSE(result.IsFailure());
    const auto& telemetry =
        result.Value().particleSystems.front().telemetry.emitters;
    ASSERT_EQ(telemetry.size(), 2u);
    EXPECT_EQ(telemetry[0].budgetedMaxParticles, 5u);
    EXPECT_EQ(telemetry[1].budgetedMaxParticles, 128u);
    // beam 不消耗总预算，有效容量只统计 smoke。
    EXPECT_EQ(
        result.Value().particleSystems.front().budget.effectiveCapacity,
        128u);
}

TEST(ParticleWorldRuntime, RendererTelemetryIsExplicitlyUnavailableUntilFed)
{
    VL::World world = MakeParticleWorld();
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemWithEmitters(
            "Spark_A", "Maps/PS_sparks.json"));

    VL::WorldSnapshotBuilder builder;
    VL::WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    auto result = builder.Build(world, desc);
    ASSERT_FALSE(result.IsFailure());
    EXPECT_FALSE(result.Value().particleSystems.front().telemetry.available);

    EXPECT_FALSE(world.SetParticleSystemRendererTelemetry(
        "Spark_A", 10, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
            11, 12, 13, 14, 15, 16, 17}, 20));
    ASSERT_TRUE(world.SetParticleSystemRendererTelemetry(
        "Spark_A", 30, {10, 20}, 40));
    result = builder.Build(world, desc);
    ASSERT_FALSE(result.IsFailure());
    const VL::ParticleSystemSnapshot& particle =
        result.Value().particleSystems.front();
    EXPECT_TRUE(particle.telemetry.available);
    EXPECT_EQ(particle.telemetry.aliveParticles, 30u);
    EXPECT_EQ(particle.telemetry.lastFrameBudget, 40u);
    ASSERT_EQ(particle.telemetry.emitters.size(), 2u);
    EXPECT_EQ(particle.telemetry.emitters[0].aliveParticles, 10u);
    EXPECT_EQ(particle.telemetry.emitters[1].aliveParticles, 20u);
    EXPECT_TRUE(world.GetParticleSystemTelemetry("Spark_A") != nullptr);
}

TEST(ParticleWorldRuntime, CaptureInterestIsOwnerCountedAndClearable)
{
    VL::World world(1, "Maps/SC_test/SC_test.json");
    EXPECT_FALSE(world.IsParticleEventCaptureActive());
    EXPECT_TRUE(world.SetParticleEventCaptureInterest("bp_a", true));
    EXPECT_TRUE(world.SetParticleEventCaptureInterest("bp_a", true));
    EXPECT_EQ(world.ParticleEventCaptureInterestCount(), 1u);
    EXPECT_TRUE(world.SetParticleEventCaptureInterest("bp_b", true));
    EXPECT_EQ(world.ParticleEventCaptureInterestCount(), 2u);
    EXPECT_TRUE(world.SetParticleEventCaptureInterest("bp_a", false));
    EXPECT_TRUE(world.IsParticleEventCaptureActive());
    EXPECT_TRUE(world.SetParticleEventCaptureInterest("bp_b", false));
    EXPECT_FALSE(world.IsParticleEventCaptureActive());
    EXPECT_FALSE(world.SetParticleEventCaptureInterest("", true));
    EXPECT_FALSE(world.SetParticleEventCaptureInterest(
        std::string(129, 'x'), true));
    EXPECT_TRUE(world.SetParticleEventCaptureInterest("bp_c", true));
    world.ClearParticleEventCaptureInterest();
    EXPECT_FALSE(world.IsParticleEventCaptureActive());
}

TEST(ParticleWorldRuntime, EventKeyDecodeAndKindMappingAreStable)
{
    const std::string key = VL::BuildParticleEmitterKey(
        "scene|with\\separators",
        "Maps/SC_x/PS_event.json",
        "emit|one\\two");
    VL::ParticleEmitterKeyIdentity identity;
    ASSERT_TRUE(VL::DecodeParticleEmitterKey(key, identity));
    EXPECT_EQ(identity.sceneName, "scene|with\\separators");
    EXPECT_EQ(identity.emitterName, "emit|one\\two");

    EXPECT_FALSE(VL::DecodeParticleEmitterKey("only-two|segments", identity));
    EXPECT_FALSE(VL::DecodeParticleEmitterKey("a|b|c|d", identity));
    EXPECT_FALSE(VL::DecodeParticleEmitterKey("a|b|", identity));
    EXPECT_FALSE(VL::DecodeParticleEmitterKey("a|b|c\\", identity));
    EXPECT_EQ(std::string(VL::ParticleEventKindName(0)), "spawn");
    EXPECT_EQ(std::string(VL::ParticleEventKindName(1)), "death");
    EXPECT_EQ(std::string(VL::ParticleEventKindName(2)), "collision");
    EXPECT_EQ(VL::ParticleEventKindName(3), nullptr);
}

TEST(ParticleWorldRuntime, EventPollClockIsPlayAndInterestGatedAtTenHz)
{
    VL::ParticleEventPollClock clock;
    EXPECT_FALSE(clock.Consume(0.05f, true, true));
    EXPECT_FALSE(clock.Consume(0.04f, true, true));
    EXPECT_TRUE(clock.Consume(0.02f, true, true));
    EXPECT_FALSE(clock.Consume(0.05f, true, true));
    EXPECT_TRUE(clock.Consume(0.05f, true, true));
    EXPECT_FALSE(clock.Consume(5.0f, false, true));
    EXPECT_FALSE(clock.Consume(5.0f, true, false));
    EXPECT_FALSE(clock.Consume(0.05f, true, true));
}

TEST(ParticleWorldRuntime, DataChannelsEnforceNameValueAndCapacityBounds)
{
    VL::World world(1, "Maps/SC_test/SC_test.json");

    // key：1..64 字符且不含 NUL；value：有限且 |v|<=1e6。
    EXPECT_FALSE(world.SetParticleDataChannel("", 1.0f));
    EXPECT_FALSE(world.SetParticleDataChannel(
        std::string(65, 'k'), 1.0f));
    EXPECT_FALSE(world.SetParticleDataChannel(
        std::string("bad\0key", 7), 1.0f));
    EXPECT_FALSE(world.SetParticleDataChannel(
        "Wind", std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(world.SetParticleDataChannel(
        "Wind", std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(world.SetParticleDataChannel("Wind", 1.000001e6f));
    EXPECT_TRUE(world.SetParticleDataChannel("Wind", -1.0e6f));
    EXPECT_TRUE(world.SetParticleDataChannel(std::string(64, 'k'), 1.0f));
    EXPECT_TRUE(world.ClearParticleDataChannel(std::string(64, 'k')));
    float value = 0.0f;
    ASSERT_TRUE(world.GetParticleDataChannel("Wind", value));
    EXPECT_FLOAT_EQ(value, -1.0e6f);

    // 容量按“新增 key”计算；已有 key 在满 64 项后仍可覆盖。
    for (size_t index = 0;
         index + 1 < VL::kMaxParticleDataChannels; ++index)
    {
        const std::string name = "channel." + std::to_string(index);
        EXPECT_TRUE(world.SetParticleDataChannel(name,
            static_cast<float>(index)));
    }
    EXPECT_EQ(world.GetParticleDataChannelCount(),
        VL::kMaxParticleDataChannels);
    EXPECT_FALSE(world.SetParticleDataChannel("overflow", 1.0f));
    EXPECT_TRUE(world.SetParticleDataChannel("channel.0", 7.5f));
    ASSERT_TRUE(world.GetParticleDataChannel("channel.0", value));
    EXPECT_FLOAT_EQ(value, 7.5f);
    EXPECT_EQ(world.GetParticleDataChannelCount(),
        VL::kMaxParticleDataChannels);
}

TEST(ParticleWorldRuntime, DataChannelsClearAndPublishIndependentSnapshot)
{
    VL::World world = MakeParticleWorld();
    ASSERT_TRUE(world.SetParticleDataChannel("Wind", 2.5f));
    ASSERT_TRUE(world.SetParticleDataChannel("Heat", -4.0f));
    EXPECT_TRUE(world.ClearParticleDataChannel("Heat"));
    EXPECT_FALSE(world.ClearParticleDataChannel("Heat"));
    EXPECT_FALSE(world.ClearParticleDataChannel(""));
    float value = 0.0f;
    EXPECT_FALSE(world.GetParticleDataChannel("Heat", value));
    EXPECT_FALSE(world.GetParticleDataChannel("Missing", value));

    VL::WorldSnapshotBuilder builder;
    VL::WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    auto result = builder.Build(world, desc);
    ASSERT_FALSE(result.IsFailure());
    const VL::WorldSnapshot& snapshot = result.Value();
    ASSERT_EQ(snapshot.particleDataChannels.size(), 1u);
    EXPECT_FLOAT_EQ(snapshot.particleDataChannels.at("Wind"), 2.5f);
    EXPECT_FALSE(snapshot.particleDataChannels.count("Heat"));

    // 快照是副本：World 后续写入不能穿透已发布的冻结帧。
    ASSERT_TRUE(world.SetParticleDataChannel("Wind", 9.0f));
    ASSERT_TRUE(world.SetParticleDataChannel("Spark", 1.0f));
    EXPECT_FLOAT_EQ(snapshot.particleDataChannels.at("Wind"), 2.5f);
    EXPECT_FALSE(snapshot.particleDataChannels.count("Spark"));
}

TEST(ParticleWorldRuntime, DataChannelCommandValidatorsMatchWorldRejection)
{
    std::string error;
    EXPECT_TRUE(VL::ValidateParticleDataChannelName("Wind", error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(VL::ValidateParticleDataChannelName("", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(VL::ValidateParticleDataChannelName(
        std::string(65, 'k'), error));
    EXPECT_FALSE(VL::ValidateParticleDataChannelName(
        std::string("bad\0key", 7), error));

    EXPECT_TRUE(VL::ValidateParticleDataChannelValue(1.0e6f, error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(VL::ValidateParticleDataChannelValue(
        std::numeric_limits<float>::quiet_NaN(), error));
    EXPECT_FALSE(VL::ValidateParticleDataChannelValue(
        std::numeric_limits<float>::infinity(), error));
    EXPECT_FALSE(VL::ValidateParticleDataChannelValue(
        -1.000001e6f, error));
}

TEST(ParticleWorldRuntime, DataChannelRuntimeCommandRejectsWithoutSideEffects)
{
    VL::World world(1, "Maps/SC_test/SC_test.json");
    ASSERT_TRUE(world.SetParticleDataChannel("Baseline", 1.0f));

    std::string error;
    EXPECT_FALSE(VL::ExecuteParticleDataChannelCommand(
        world, false, "", 1.0f, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(VL::ExecuteParticleDataChannelCommand(
        world, false, std::string(65, 'k'), 1.0f, error));
    EXPECT_FALSE(VL::ExecuteParticleDataChannelCommand(
        world, false, "Wind",
        std::numeric_limits<float>::quiet_NaN(), error));
    EXPECT_FALSE(VL::ExecuteParticleDataChannelCommand(
        world, false, "Wind", 1.000001e6f, error));
    EXPECT_FALSE(VL::ExecuteParticleDataChannelCommand(
        world, true, "Missing", 0.0f, error));
    EXPECT_FALSE(error.empty());
    // 失败路径不得创建/删除任何条目，也不能改变已有值。
    EXPECT_EQ(world.GetParticleDataChannelCount(), 1u);
    float value = 0.0f;
    ASSERT_TRUE(world.GetParticleDataChannel("Baseline", value));
    EXPECT_FLOAT_EQ(value, 1.0f);
    EXPECT_FALSE(world.GetParticleDataChannel("Wind", value));

    EXPECT_TRUE(VL::ExecuteParticleDataChannelCommand(
        world, false, "Wind", 2.5f, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(VL::ExecuteParticleDataChannelCommand(
        world, true, "Wind", 0.0f, error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(world.GetParticleDataChannel("Wind", value));
    EXPECT_EQ(world.GetParticleDataChannelCount(), 1u);
}
