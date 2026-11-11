#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "resource/image/hostImage.h"
#include "terrain/terrainCollision.h"
#include "terrain/terrainHeightmap.h"

namespace
{

HostImage MakeHeightmap(
    uint32_t width,
    uint32_t height,
    const std::function<float(uint32_t, uint32_t)>& value)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float sample = value(x, y);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            pixels[index + 0] = sample;
            pixels[index + 1] = sample;
            pixels[index + 2] = sample;
            pixels[index + 3] = 1.0f;
        }
    }
    return image;
}

VL::TerrainBuildPlan MakePlan(
    std::shared_ptr<const HostImage> heightmap,
    float sizeX = 10000.0f,
    float sizeY = 10000.0f,
    float heightScale = 1000.0f,
    float heightOffset = 0.0f,
    uint32_t gridResolution = 64)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_collision.json";
    plan.desc.name = "TR_collision";
    plan.desc.heightmapPath = "Generated/Validation/TerrainTests/heightmap.exr";
    plan.desc.size = Eigen::Vector2f(sizeX, sizeY);
    plan.desc.heightScale = heightScale;
    plan.desc.heightOffset = heightOffset;
    plan.desc.gridResolution = gridResolution;
    plan.desc.chunkQuads = 16;
    plan.heightmap = std::move(heightmap);
    plan.placementId = "terrain|CollisionProbe";
    return plan;
}

std::shared_ptr<const HostImage> FlatHeightmap(float value = 1.0f)
{
    return std::make_shared<const HostImage>(MakeHeightmap(4, 4,
        [value](uint32_t, uint32_t) { return value; }));
}

// v（高度图的 +Z 方向）线性升高；用于验证局部坐标系、旋转和缩放。
std::shared_ptr<const HostImage> RampHeightmap(uint32_t resolution = 8)
{
    return std::make_shared<const HostImage>(MakeHeightmap(resolution, resolution,
        [resolution](uint32_t, uint32_t y)
        {
            return static_cast<float>(y) / static_cast<float>(resolution - 1);
        }));
}

float ExpectedCpuHeightCm(
    const VL::TerrainBuildPlan& plan,
    float u,
    float v,
    float placementScaleY = 1.0f,
    float placementOffsetY = 0.0f)
{
    return VL::TerrainHeightmap::SampleHeightCm(plan.desc, *plan.heightmap, u, v) *
        placementScaleY + placementOffsetY;
}

bool RaycastDownCm(
    const VL::TerrainCollisionWorld& world,
    float x,
    float z,
    float fromHeightCm,
    float distanceCm,
    float& outHeightCm)
{
    Eigen::Vector3f point;
    if (!world.RaycastDown(Eigen::Vector3f(x, fromHeightCm, z), distanceCm, point))
        return false;
    outHeightCm = point.y();
    return true;
}

} // namespace

TEST(TerrainCollision, FlatHeightmapRaycastHitsAuthoredHeight)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(FlatHeightmap(), 10000.0f, 10000.0f, 500.0f);
    VL::TerrainCollisionBody body = world.AddTerrain(plan);
    ASSERT_TRUE(body.IsValid());

    const float probes[][2] = {
        {0.0f, 0.0f}, {-4900.0f, -4900.0f}, {4900.0f, 4900.0f}, {2500.0f, -3000.0f}};
    for (const auto& probe : probes)
    {
        float height = 0.0f;
        ASSERT_TRUE(RaycastDownCm(world, probe[0], probe[1], 2000.0f, 4000.0f, height))
            << "miss at " << probe[0] << ", " << probe[1];
        EXPECT_NEAR(height, 500.0f, 0.01f) << "at " << probe[0] << ", " << probe[1];
    }
}

TEST(TerrainCollision, RampRaycastMatchesCpuHeightSampling)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(RampHeightmap(), 10000.0f, 10000.0f, 1000.0f);
    VL::TerrainCollisionBody body = world.AddTerrain(plan);
    ASSERT_TRUE(body.IsValid());

    // 平面斜坡上 heightfield 与渲染网格的三角化都退化成同一平面，误差只来自
    // float 与 heightfield 的 16 位块内量化。
    const float probes[][2] = {
        {0.0f, -4000.0f}, {0.0f, 0.0f}, {0.0f, 3750.0f}, {-3000.0f, 2000.0f}, {3000.0f, 4800.0f}};
    for (const auto& probe : probes)
    {
        const float u = probe[0] / plan.desc.size.x() + 0.5f;
        const float v = probe[1] / plan.desc.size.y() + 0.5f;
        const float expected = ExpectedCpuHeightCm(plan, u, v);
        float height = 0.0f;
        ASSERT_TRUE(RaycastDownCm(world, probe[0], probe[1], 2000.0f, 4000.0f, height));
        EXPECT_NEAR(height, expected, 0.5f) << "at " << probe[0] << ", " << probe[1];
    }
}

TEST(TerrainCollision, AppliesHeightOffsetAndPlacementPosition)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(FlatHeightmap(), 10000.0f, 10000.0f, 500.0f, 200.0f);
    VL::TerrainCollisionPlacement placement;
    placement.position = Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f);
    VL::TerrainCollisionBody body = world.AddTerrain(plan, placement);
    ASSERT_TRUE(body.IsValid());

    float height = 0.0f;
    ASSERT_TRUE(RaycastDownCm(world, 1000.0f, -2000.0f, 6000.0f, 5000.0f, height));
    EXPECT_NEAR(height, 3000.0f + 500.0f + 200.0f, 0.01f);

    // 摆放位置之外的垂直射线不应命中。
    EXPECT_FALSE(RaycastDownCm(world, 7000.0f, -2000.0f, 6000.0f, 5000.0f, height));
}

TEST(TerrainCollision, AppliesPlacementScaleAndRotation)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(RampHeightmap(), 10000.0f, 10000.0f, 1000.0f);

    // scale.y=2 把高度范围翻倍，scale.x=0.5 把 X 范围缩到 5000cm。
    VL::TerrainCollisionPlacement scaled;
    scaled.scale = Eigen::Vector3f(0.5f, 2.0f, 1.0f);
    VL::TerrainCollisionBody scaledBody = world.AddTerrain(plan, scaled);
    ASSERT_TRUE(scaledBody.IsValid());
    float height = 0.0f;
    ASSERT_TRUE(RaycastDownCm(world, 2000.0f, 0.0f, 4000.0f, 6000.0f, height));
    EXPECT_NEAR(height, 1000.0f, 1.0f);
    EXPECT_FALSE(RaycastDownCm(world, 4000.0f, 0.0f, 4000.0f, 6000.0f, height));
    scaledBody.Reset();

    // 绕 Y 旋转 +90°：局部 +Z 变成世界 +X，坡面沿 +X 升高。
    VL::TerrainCollisionPlacement rotated;
    rotated.rotation = Eigen::Vector3f(0.0f, 90.0f, 0.0f);
    VL::TerrainCollisionBody rotatedBody = world.AddTerrain(plan, rotated);
    ASSERT_TRUE(rotatedBody.IsValid());
    ASSERT_TRUE(RaycastDownCm(world, 3000.0f, 0.0f, 2000.0f, 3000.0f, height));
    // 局部 z=+3000 → v=0.8 → 800cm。
    EXPECT_NEAR(height, 800.0f, 1.5f);
    Eigen::Vector3f point;
    Eigen::Vector3f normal;
    ASSERT_TRUE(world.RaycastDown(Eigen::Vector3f(-3000.0f, 2000.0f, 0.0f), 3000.0f, point, &normal));
    EXPECT_NEAR(point.y(), 200.0f, 1.5f);
    EXPECT_LT(normal.x(), -0.05f) << "slope should face -X after yaw";
    EXPECT_NEAR(normal.y(), 0.995f, 0.01f);
}

TEST(TerrainCollision, DestroyAndRebuildLeavesNoBody)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(FlatHeightmap(), 10000.0f, 10000.0f, 500.0f);
    JPH::PhysicsSystem& physics = world.GetPhysicsSystem();

    uint32_t previousId = 0xFFFFFFFFu;
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        VL::TerrainCollisionBody body = world.AddTerrain(plan);
        ASSERT_TRUE(body.IsValid());
        ASSERT_EQ(physics.GetNumBodies(), 1u);
        const uint32_t id = body.GetBodyIdValue();
        EXPECT_NE(id, previousId) << "destroy must retire the BodyID sequence number";
        previousId = id;
        body.Reset();
        EXPECT_FALSE(body.IsValid());
        EXPECT_EQ(physics.GetNumBodies(), 0u);
    }
}

TEST(TerrainCollision, RejectsInvalidInputsWithoutLeavingBodies)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan valid = MakePlan(FlatHeightmap(), 10000.0f, 10000.0f, 500.0f);

    VL::TerrainBuildPlan missingHeightmap = valid;
    missingHeightmap.heightmap.reset();
    EXPECT_THROW(world.AddTerrain(missingHeightmap), std::invalid_argument);

    VL::TerrainBuildPlan tinyHeightmap = valid;
    tinyHeightmap.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(1, 1, [](uint32_t, uint32_t) { return 1.0f; }));
    EXPECT_THROW(world.AddTerrain(tinyHeightmap), std::invalid_argument);

    VL::TerrainBuildPlan badSize = valid;
    badSize.desc.size = Eigen::Vector2f(0.0f, 10000.0f);
    EXPECT_THROW(world.AddTerrain(badSize), std::invalid_argument);

    VL::TerrainCollisionPlacement badScale;
    badScale.scale = Eigen::Vector3f(1.0f, 0.0f, 1.0f);
    EXPECT_THROW(world.AddTerrain(valid, badScale), std::invalid_argument);

    VL::TerrainCollisionSettings tooFewSamples;
    tooFewSamples.sampleCount = 3;
    EXPECT_THROW(world.AddTerrain(valid, {}, tooFewSamples), std::invalid_argument);

    VL::TerrainCollisionSettings tooManySamples;
    tooManySamples.sampleCount = 5000;
    EXPECT_THROW(world.AddTerrain(valid, {}, tooManySamples), std::invalid_argument);

    VL::TerrainCollisionSettings badObjectLayer;
    badObjectLayer.objectLayer = 2;
    EXPECT_THROW(world.AddTerrain(valid, {}, badObjectLayer), std::invalid_argument);

    EXPECT_EQ(world.GetPhysicsSystem().GetNumBodies(), 0u);
}

TEST(TerrainCollision, NonSquareHeightmapMapsXAndZIndependently)
{
    VL::TerrainCollisionWorld world;
    // u 沿 +X 线性升高：x=+5000cm（u=0.75）应为 375cm，z 不改变高度。
    const std::shared_ptr<const HostImage> rampX =
        std::make_shared<const HostImage>(MakeHeightmap(8, 8,
            [](uint32_t x, uint32_t)
            {
                return static_cast<float>(x) / 7.0f;
            }));
    const VL::TerrainBuildPlan plan =
        MakePlan(rampX, 20000.0f, 10000.0f, 500.0f);
    VL::TerrainCollisionBody body = world.AddTerrain(plan);
    ASSERT_TRUE(body.IsValid());

    float height = 0.0f;
    ASSERT_TRUE(RaycastDownCm(world, 5000.0f, 0.0f, 1000.0f, 2000.0f, height));
    EXPECT_NEAR(height, 375.0f, 1.0f);
    ASSERT_TRUE(RaycastDownCm(world, 0.0f, 2500.0f, 1000.0f, 2000.0f, height));
    EXPECT_NEAR(height, 250.0f, 1.0f);
}

TEST(TerrainCollision, MissesWhenRayIsOutOfBoundsOrTooShort)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(FlatHeightmap(), 10000.0f, 10000.0f, 500.0f);
    VL::TerrainCollisionBody body = world.AddTerrain(plan);
    ASSERT_TRUE(body.IsValid());

    float height = 0.0f;
    EXPECT_FALSE(RaycastDownCm(world, 20000.0f, 0.0f, 2000.0f, 4000.0f, height));
    // 起点已经在地表下方：heightfield 是单面表面，向下的射线不会命中背面。
    EXPECT_FALSE(RaycastDownCm(world, 0.0f, 0.0f, 400.0f, 100.0f, height));
    Eigen::Vector3f ignored;
    EXPECT_FALSE(world.RaycastDown(Eigen::Vector3f(0.0f, 2000.0f, 0.0f), 0.0f, ignored));
}

TEST(TerrainCollision, DynamicSphereSettlesOnTerrainSurface)
{
    VL::TerrainCollisionWorld world;
    const VL::TerrainBuildPlan plan = MakePlan(FlatHeightmap(), 10000.0f, 10000.0f, 500.0f);
    VL::TerrainCollisionBody terrain = world.AddTerrain(plan);
    ASSERT_TRUE(terrain.IsValid());

    JPH::BodyInterface& bodies = world.GetPhysicsSystem().GetBodyInterface();
    JPH::BodyCreationSettings sphere(
        new JPH::SphereShape(1.0f),
        JPH::RVec3(0.0f, 30.0f, 0.0f),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic,
        1);
    const JPH::BodyID sphereId = bodies.CreateAndAddBody(sphere, JPH::EActivation::Activate);
    ASSERT_FALSE(sphereId.IsInvalid());

    for (int step = 0; step < 240; ++step)
        world.Step(1.0f / 60.0f);

    const JPH::RVec3 position = bodies.GetPosition(sphereId);
    // 平地 500cm + 球半径 100cm；允许穿透容差与少量弹跳误差。
    EXPECT_NEAR(static_cast<float>(position.GetY()) * 100.0f, 600.0f, 5.0f);
    EXPECT_LT(std::abs(static_cast<float>(position.GetX())), 50.0f);
}
