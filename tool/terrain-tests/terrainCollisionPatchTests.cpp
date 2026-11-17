#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/terrainCollisionPatchStreamer.h"
#include "resource/image/hostImage.h"
#include "terrain/terrainCollisionPatch.h"
#include "terrain/terrainCollisionProxy.h"

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
    float sizeCm = 20000.0f,
    float heightmapValue = 0.5f)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_patch.json";
    plan.desc.name = "TR_patch";
    plan.desc.size = Eigen::Vector2f(sizeCm, sizeCm);
    plan.desc.heightScale = 1000.0f;
    plan.desc.gridResolution = 64;
    plan.desc.chunkQuads = 16;
    plan.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(
            4,
            4,
            [heightmapValue](uint32_t, uint32_t)
            {
                return heightmapValue;
            }));
    plan.placementId = "terrain|Patch";
    return plan;
}

bool WaitForPatch(
    VL::TerrainCollisionPatchStreamer& streamer,
    const Eigen::Vector3f& position,
    bool expected)
{
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        streamer.Update(position);
        if (streamer.HasActivePatch() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return streamer.HasActivePatch() == expected;
}

bool WaitForPatchIdChange(
    VL::TerrainCollisionPatchStreamer& streamer,
    const Eigen::Vector3f& position,
    uint32_t previousId)
{
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        streamer.Update(position);
        if (streamer.HasActivePatch() &&
            streamer.ActivePatchBodyId().GetIndexAndSequenceNumber() != previousId)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

TEST(TerrainCollisionPatch, BuildsMacroHeightPatchAtHalfMetreSpacing)
{
    const auto macro = VL::BuildTerrainCollisionProxy(MakePlan(), {});
    ASSERT_NE(macro, nullptr);
    const auto patch = VL::BuildTerrainCollisionPatch(
        *macro,
        Eigen::Vector2f::Zero(),
        VL::TerrainCollisionPatchBuildSettings{});
    ASSERT_NE(patch, nullptr);
    EXPECT_TRUE(patch->IsValid());
    EXPECT_EQ(patch->proxy.sampleCount, 130u);
    EXPECT_FLOAT_EQ(patch->proxy.sizeXCm, 6400.0f);
    EXPECT_FLOAT_EQ(patch->proxy.sizeZCm, 6400.0f);
    EXPECT_FLOAT_EQ(patch->centerLocalCm.x(), 0.0f);
    EXPECT_FLOAT_EQ(patch->centerLocalCm.y(), 0.0f);
    for (const float height : patch->proxy.heightCm)
        EXPECT_NEAR(height, 500.0f, 0.001f);
    EXPECT_NEAR(patch->proxy.boundsMinCm.x(), -3200.0f, 0.01f);
    EXPECT_NEAR(patch->proxy.boundsMaxCm.x(), 3200.0f, 0.01f);
    EXPECT_NEAR(patch->proxy.boundsMinCm.y(), 500.0f, 0.01f);
    EXPECT_NEAR(patch->proxy.boundsMaxCm.y(), 500.0f, 0.01f);

    const auto start = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < 10; ++repeat)
    {
        const auto rebuilt = VL::BuildTerrainCollisionPatch(
            *macro,
            Eigen::Vector2f::Zero(),
            VL::TerrainCollisionPatchBuildSettings{});
        ASSERT_NE(rebuilt, nullptr);
    }
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count() / 10.0;
    std::cout << "[TerrainCollisionPatch] buildMs=" << elapsedMs
              << " samples=" << patch->proxy.sampleCount << std::endl;
}

TEST(TerrainCollisionPatch, RejectsPatchOutsideMacroProxy)
{
    const auto macro = VL::BuildTerrainCollisionProxy(MakePlan(10000.0f), {});
    ASSERT_NE(macro, nullptr);
    EXPECT_EQ(
        VL::BuildTerrainCollisionPatch(
            *macro,
            Eigen::Vector2f(2000.0f, 0.0f),
            VL::TerrainCollisionPatchBuildSettings{}),
        nullptr);
}

TEST(TerrainCollisionPatch, RejectsInvalidSettings)
{
    const auto macro = VL::BuildTerrainCollisionProxy(MakePlan(), {});
    ASSERT_NE(macro, nullptr);
    VL::TerrainCollisionPatchBuildSettings settings;
    settings.patchSizeCm = 0.0f;
    EXPECT_THROW(
        VL::BuildTerrainCollisionPatch(
            *macro, Eigen::Vector2f::Zero(), settings),
        std::invalid_argument);
    settings = {};
    settings.spacingCm = 0.0f;
    EXPECT_THROW(
        VL::BuildTerrainCollisionPatch(
            *macro, Eigen::Vector2f::Zero(), settings),
        std::invalid_argument);
}

TEST(TerrainCollisionPatchStreamer, BuildsReplacesAndClearsPatch)
{
    VL::TerrainCollisionWorld world;
    const auto macro = VL::BuildTerrainCollisionProxy(MakePlan(), {});
    ASSERT_NE(macro, nullptr);
    VL::TerrainCollisionPatchStreamer::Settings settings;
    settings.body.collisionGroup = 31;
    VL::TerrainCollisionPatchStreamer streamer(
        world.GetPhysicsSystem(), settings);
    streamer.SetProxies({macro});

    ASSERT_TRUE(WaitForPatch(streamer, Eigen::Vector3f(0.0f, 500.0f, 0.0f), true));
    ASSERT_TRUE(streamer.HasActivePatch());
    const uint32_t firstId = streamer.ActivePatchBodyId().GetIndexAndSequenceNumber();
    EXPECT_EQ(streamer.ActivePatchSampleCount(), 130u);

    // 移动到一个新的 32 m cell，等 worker 构建新 patch 后替换旧 body。
    ASSERT_TRUE(WaitForPatchIdChange(
        streamer, Eigen::Vector3f(6400.0f, 500.0f, 0.0f), firstId));
    ASSERT_TRUE(streamer.HasActivePatch());
    const uint32_t secondId = streamer.ActivePatchBodyId().GetIndexAndSequenceNumber();
    EXPECT_NE(firstId, secondId);

    // 离开代理范围后回退宏 body。
    ASSERT_TRUE(WaitForPatch(streamer, Eigen::Vector3f(100000.0f, 500.0f, 0.0f), false));
    EXPECT_FALSE(streamer.HasActivePatch());
    EXPECT_TRUE(streamer.ActivePatchBodyId().IsInvalid());
    EXPECT_EQ(world.GetPhysicsSystem().GetNumBodies(), 0u);
}
