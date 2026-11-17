#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "terrain/terrainCollision.h"
#include "terrain/terrainCollisionProxy.h"
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
    float heightmapValue = 0.5f,
    float sizeXCm = 10000.0f,
    float sizeZCm = 10000.0f,
    float heightScale = 1000.0f,
    uint32_t gridResolution = 64)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_proxy.json";
    plan.desc.name = "TR_proxy";
    plan.desc.size = Eigen::Vector2f(sizeXCm, sizeZCm);
    plan.desc.heightScale = heightScale;
    plan.desc.gridResolution = gridResolution;
    plan.desc.chunkQuads = 16;
    plan.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(
            4,
            4,
            [heightmapValue](uint32_t, uint32_t)
            {
                return heightmapValue;
            }));
    plan.placementId = "terrain|Proxy";
    return plan;
}

} // namespace

TEST(TerrainCollisionProxy, BuildsAlignedSampleGridAndWorldHeights)
{
    const VL::TerrainBuildPlan plan = MakePlan(0.5f, 10000.0f, 10000.0f, 1000.0f);
    const auto proxy = VL::BuildTerrainCollisionProxy(plan, {});
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(proxy->sampleCount, 66u);
    EXPECT_FLOAT_EQ(proxy->sizeXCm, 10000.0f);
    EXPECT_FLOAT_EQ(proxy->sizeZCm, 10000.0f);
    ASSERT_EQ(proxy->heightCm.size(), 66u * 66u);
    for (const float height : proxy->heightCm)
        EXPECT_NEAR(height, 500.0f, 0.001f);
    EXPECT_TRUE(proxy->IsValid());
}

TEST(TerrainCollisionProxy, AppliesPlacementScaleToExtentsAndHeights)
{
    const VL::TerrainBuildPlan plan = MakePlan(0.5f, 10000.0f, 10000.0f, 1000.0f);
    VL::TerrainCollisionPlacement placement;
    placement.position = Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f);
    placement.scale = Eigen::Vector3f(0.5f, 2.0f, 1.0f);
    const auto proxy = VL::BuildTerrainCollisionProxy(plan, placement);
    ASSERT_NE(proxy, nullptr);
    EXPECT_FLOAT_EQ(proxy->sizeXCm, 5000.0f);
    EXPECT_FLOAT_EQ(proxy->sizeZCm, 10000.0f);
    for (const float height : proxy->heightCm)
        EXPECT_NEAR(height, 1000.0f, 0.001f);
    EXPECT_NEAR(proxy->boundsMinCm.x(), -1500.0f, 0.01f);
    EXPECT_NEAR(proxy->boundsMaxCm.x(), 3500.0f, 0.01f);
    EXPECT_NEAR(proxy->boundsMinCm.y(), 4000.0f, 0.01f);
    EXPECT_NEAR(proxy->boundsMaxCm.y(), 4000.0f, 0.01f);
    EXPECT_NEAR(proxy->boundsMinCm.z(), -7000.0f, 0.01f);
    EXPECT_NEAR(proxy->boundsMaxCm.z(), 3000.0f, 0.01f);
}

TEST(TerrainCollisionProxy, RejectsInvalidInputs)
{
    VL::TerrainBuildPlan valid = MakePlan();

    VL::TerrainBuildPlan missing = valid;
    missing.heightmap.reset();
    EXPECT_THROW(
        VL::BuildTerrainCollisionProxy(missing, {}),
        std::invalid_argument);

    VL::TerrainBuildPlan tiny = valid;
    tiny.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(1, 1, [](uint32_t, uint32_t) { return 0.5f; }));
    EXPECT_THROW(
        VL::BuildTerrainCollisionProxy(tiny, {}),
        std::invalid_argument);

    VL::TerrainBuildPlan badSize = valid;
    badSize.desc.size = Eigen::Vector2f(0.0f, 10000.0f);
    EXPECT_THROW(
        VL::BuildTerrainCollisionProxy(badSize, {}),
        std::invalid_argument);

    VL::TerrainCollisionPlacement badScale;
    badScale.scale = Eigen::Vector3f(1.0f, 0.0f, 1.0f);
    EXPECT_THROW(
        VL::BuildTerrainCollisionProxy(valid, badScale),
        std::invalid_argument);

    EXPECT_THROW(
        VL::BuildTerrainCollisionProxy(valid, {}, 3),
        std::invalid_argument);
    EXPECT_THROW(
        VL::BuildTerrainCollisionProxy(valid, {}, 5000),
        std::invalid_argument);
}

TEST(TerrainCollisionProxy, BodyFromProxyRaycastMatchesPlanPath)
{
    const VL::TerrainBuildPlan plan = MakePlan(0.5f, 10000.0f, 10000.0f, 1000.0f);
    VL::TerrainCollisionPlacement placement;
    placement.position = Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f);
    placement.rotation = Eigen::Vector3f(0.0f, 17.0f, 0.0f);

    VL::TerrainCollisionWorld world;
    {
        VL::TerrainCollisionBody legacy = world.AddTerrain(plan, placement);
        ASSERT_TRUE(legacy.IsValid());
        Eigen::Vector3f point;
        ASSERT_TRUE(world.RaycastDown(
            Eigen::Vector3f(1000.0f, 6000.0f, -2000.0f),
            4000.0f,
            point));
        EXPECT_NEAR(point.y(), 3500.0f, 0.5f);
        legacy.Reset();
    }

    const auto proxy = VL::BuildTerrainCollisionProxy(plan, placement);
    ASSERT_NE(proxy, nullptr);
    VL::TerrainCollisionBody body = VL::CreateTerrainCollisionBody(
        world.GetPhysicsSystem(), *proxy);
    ASSERT_TRUE(body.IsValid());
    EXPECT_EQ(body.GetSampleCount(), proxy->sampleCount);
    Eigen::Vector3f point;
    ASSERT_TRUE(world.RaycastDown(
        Eigen::Vector3f(1000.0f, 6000.0f, -2000.0f),
        4000.0f,
        point));
    EXPECT_NEAR(point.y(), 3500.0f, 0.5f);
    EXPECT_GE(body.GetWorldBoundsMinCm().y(), proxy->boundsMinCm.y() - 1.0f);
    EXPECT_LE(body.GetWorldBoundsMaxCm().y(), proxy->boundsMaxCm.y() + 1.0f);
}
