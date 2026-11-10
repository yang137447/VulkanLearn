#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "script/scriptPhysics.h"
#include "script/scriptRuntime.h"
#include "terrain/terrainCollisionTypes.h"
#include "terrain/terrainHeightmap.h"
#include "world/world.h"

namespace
{

using Json = nlohmann::json;

std::shared_ptr<const HostImage> FlatHeightmap(float value = 1.0f)
{
    HostImage image;
    image.width = 4;
    image.height = 4;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (size_t index = 0; index < image.width * image.height; ++index)
    {
        pixels[index * 4 + 0] = value;
        pixels[index * 4 + 1] = value;
        pixels[index * 4 + 2] = value;
        pixels[index * 4 + 3] = 1.0f;
    }
    return std::make_shared<const HostImage>(std::move(image));
}

VL::TerrainBuildPlan Plan(const std::string& placementId)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_script_collision.json";
    plan.desc.size = Eigen::Vector2f(10000.0f, 10000.0f);
    plan.desc.heightScale = 500.0f;
    plan.desc.gridResolution = 64;
    plan.desc.chunkQuads = 16;
    plan.heightmap = FlatHeightmap();
    plan.placementId = placementId;
    return plan;
}

VL::TerrainCollisionInstance Instance(
    const std::string& placementId,
    const Eigen::Vector3f& position)
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = placementId;
    instance.plan = Plan(placementId);
    instance.placement.position = position;
    return instance;
}

Json StaticProbeState()
{
    return {
        {"id", "probe"},
        {"position", {1000.0, 6000.0, -2000.0}},
        {"rotation", {0.0, 0.0, 0.0}},
        {"scale", {1.0, 1.0, 1.0}}};
}

Json StaticProbeDeclarations()
{
    return {
        {"probe", {
            {"id", "probe"},
            {"position", {1000.0, 6000.0, -2000.0}},
            {"rotation", {0.0, 0.0, 0.0}},
            {"scale", {1.0, 1.0, 1.0}},
            {"physics", {
                {"shape", "box"},
                {"motion", "static"},
                {"halfExtents", {50.0, 50.0, 50.0}}}}}}};
}

} // namespace

TEST(ScriptTerrainCollision, SetTerrainCollisionsCreatesOneBodyPerPlacement)
{
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions({
        Instance("terrain|A", Eigen::Vector3f(-6000.0f, 0.0f, 0.0f)),
        Instance("terrain|B", Eigen::Vector3f(6000.0f, 100.0f, 0.0f))});

    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 2u);
    const auto placements = physics.TerrainCollisionPlacementIds();
    ASSERT_EQ(placements.size(), 2u);
    EXPECT_NE(std::find(placements.begin(), placements.end(), "terrain|A"), placements.end());
    EXPECT_NE(std::find(placements.begin(), placements.end(), "terrain|B"), placements.end());
}

TEST(ScriptTerrainCollision, SetTerrainCollisionsReplacesInsteadOfAppending)
{
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions({Instance("terrain|A", Eigen::Vector3f::Zero())});
    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 1u);

    physics.SetTerrainCollisions({
        Instance("terrain|A", Eigen::Vector3f::Zero()),
        Instance("terrain|B", Eigen::Vector3f(1000.0f, 0.0f, 0.0f))});
    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 2u);

    physics.SetTerrainCollisions({});
    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 0u);
    EXPECT_TRUE(physics.TerrainCollisionPlacementIds().empty());
}

TEST(ScriptTerrainCollision, RaycastHitsHeightfieldAtCpuSampledHeight)
{
    VL::ScriptPhysicsWorld physics;
    const VL::TerrainCollisionInstance instance =
        Instance("terrain|Probe", Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f));
    physics.SetTerrainCollisions({instance});

    const float expected = VL::TerrainHeightmap::SampleHeightCm(
        instance.plan.desc, *instance.plan.heightmap, 0.5f, 0.5f) +
        instance.placement.position.y();
    const Json request = {
        {"id", "ground"},
        {"owner", "none"},
        {"from", {1000.0, 6000.0, -2000.0}},
        {"to", {1000.0, 0.0, -2000.0}}};
    const Json hits = physics.Raycast(Json::array({request}));
    ASSERT_EQ(hits.size(), 1u);
    ASSERT_TRUE(hits[0].contains("point")) << hits[0].dump();
    EXPECT_NEAR(hits[0].at("point")[1].get<float>(), expected, 0.5f);
    EXPECT_FALSE(hits[0].contains("blueprint"));
    EXPECT_EQ(hits[0].at("terrain").get<std::string>(), "terrain|Probe");
}

TEST(ScriptTerrainCollision, ProbeGroundFindsTerrainBelowAScriptBody)
{
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions({
        Instance("terrain|Probe", Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f))});

    Json states = Json::array({StaticProbeState()});
    const Json declarations = StaticProbeDeclarations();
    physics.Step(states, declarations, 0.0f);

    const VL::Animation::FootGroundHit hit = physics.ProbeGround(
        "probe",
        Eigen::Vector3f(1000.0f, 6000.0f, -2000.0f),
        Eigen::Vector3f(0.0f, -3000.0f, 0.0f));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y(), 3500.0f, 0.5f);
    EXPECT_EQ(hit.supportIdentity, "terrain|Probe");
}

TEST(ScriptTerrainCollision, PlainWorldDoesNotCreateAPhysicsWorld)
{
    VL::ScriptRuntimeSubsystem runtime;
    auto world = std::make_shared<VL::World>(1, "scene.json");
    runtime.SynchronizeWorld(world);
    EXPECT_FALSE(runtime.HasPhysicsWorld());
    runtime.SynchronizeWorld(nullptr);
    EXPECT_FALSE(runtime.HasPhysicsWorld());
}
