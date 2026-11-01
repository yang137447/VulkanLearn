#include <limits>

#include <gtest/gtest.h>

#include "commonFunction.h"
#include "particles/particleLimits.h"
#include "sceneNode.h"
#include "world/world.h"
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
