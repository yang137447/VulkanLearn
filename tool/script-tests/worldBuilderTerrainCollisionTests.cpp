#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "scene/sceneAssetTypes.h"
#include "script/scriptPhysics.h"
#include "world/loading/worldBuilder.h"
#include "world/worldBuildPlan.h"
#include "world/world.h"

namespace
{

std::shared_ptr<const HostImage> FlatHeightmap()
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
        pixels[index * 4 + 0] = 1.0f;
        pixels[index * 4 + 1] = 1.0f;
        pixels[index * 4 + 2] = 1.0f;
        pixels[index * 4 + 3] = 1.0f;
    }
    return std::make_shared<const HostImage>(std::move(image));
}

VL::TerrainBuildPlan TerrainPlan(const std::string& placementId)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_world_builder.json";
    plan.desc.size = Eigen::Vector2f(10000.0f, 10000.0f);
    plan.desc.heightScale = 500.0f;
    plan.desc.gridResolution = 64;
    plan.desc.chunkQuads = 16;
    plan.heightmap = FlatHeightmap();
    plan.placementId = placementId;
    return plan;
}

nlohmann::json CameraObject()
{
    return {
        {"name", "Camera"},
        {"type", "camera"},
        {"fov", 70.0},
        {"near_clip", 10.0},
        {"far_clip", 100000.0},
        {"position", {0.0, 8000.0, 22000.0}},
        {"rotation", {-16.0, 0.0, 0.0}},
        {"scale", {1.0, 1.0, 1.0}},
        {"exposure", {
            {"mode", "ev100"},
            {"ev100", 0.0},
            {"aperture", 4.0},
            {"shutterSeconds", 0.008},
            {"iso", 100.0},
            {"compensation", 0.0},
            {"autoMinEv100", -10.0},
            {"autoMaxEv100", 20.0}}}};
}

nlohmann::json TerrainObject(
    const std::string& name,
    const nlohmann::json& position,
    const nlohmann::json& scale = {1.0, 1.0, 1.0})
{
    return {
        {"name", name},
        {"type", "terrain"},
        {"terrainPath", "Generated/Validation/TerrainTests/TR_world_builder.json"},
        {"position", position},
        {"rotation", {0.0, 45.0, 0.0}},
        {"scale", scale}};
}

SceneAssetObjectPlan ObjectPlan(
    size_t index,
    const std::string& name,
    const std::string& type,
    std::optional<VL::TerrainBuildPlan> terrainPlan = std::nullopt)
{
    SceneAssetObjectPlan plan;
    plan.objectIndex = index;
    plan.objectName = name;
    plan.objectType = type;
    plan.terrainBuildPlan = std::move(terrainPlan);
    return plan;
}

} // namespace

TEST(WorldBuilderTerrainCollision, BuildsOneInstancePerTerrainPlacement)
{
    VL::WorldBuildPlan buildPlan;
    buildPlan.scenePath = "Generated/Validation/TerrainTests/SC_world_builder.json";
    buildPlan.sceneJson = {
        {"objects", nlohmann::json::array({
            CameraObject(),
            TerrainObject("TerrainA", {-6000.0, 100.0, 0.0}),
            TerrainObject("TerrainB", {6000.0, -50.0, 500.0})})}};
    buildPlan.sceneAssetPlan.scenePath = buildPlan.scenePath;
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(0, "Camera", "camera"));
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(1, "TerrainA", "terrain", TerrainPlan("terrain|TerrainA")));
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(2, "TerrainB", "terrain", TerrainPlan("terrain|TerrainB")));

    const auto result = VL::WorldBuilder().BuildFromLoadedScene(
        1, buildPlan, VL::WorldAssemblyAssets{}, 4);
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const auto& instances = result.Value()->GetTerrainCollisionInstances();
    ASSERT_EQ(instances.size(), 2u);
    EXPECT_EQ(instances[0].placementId, "terrain|TerrainA");
    EXPECT_EQ(instances[1].placementId, "terrain|TerrainB");
    EXPECT_TRUE(instances[0].placement.position.isApprox(
        Eigen::Vector3f(-6000.0f, 100.0f, 0.0f)));
    EXPECT_TRUE(instances[0].placement.rotation.isApprox(
        Eigen::Vector3f(0.0f, 45.0f, 0.0f)));
    EXPECT_TRUE(instances[1].placement.position.isApprox(
        Eigen::Vector3f(6000.0f, -50.0f, 500.0f)));
}

TEST(WorldBuilderTerrainCollision, NonPositiveScaleFailsWorldBuild)
{
    VL::WorldBuildPlan buildPlan;
    buildPlan.scenePath = "scene.json";
    buildPlan.sceneJson = {
        {"objects", nlohmann::json::array({
            CameraObject(),
            TerrainObject("TerrainA", {0.0, 0.0, 0.0}, {1.0, -1.0, 1.0})})}};
    buildPlan.sceneAssetPlan.scenePath = buildPlan.scenePath;
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(0, "Camera", "camera"));
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(1, "TerrainA", "terrain", TerrainPlan("terrain|TerrainA")));

    const auto result = VL::WorldBuilder().BuildFromLoadedScene(
        1, buildPlan, VL::WorldAssemblyAssets{}, 4);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "TerrainCollision.NonPositiveScale");
}

TEST(WorldBuilderTerrainCollision, WorldInstancesFeedPlayPhysicsWorld)
{
    VL::WorldBuildPlan buildPlan;
    buildPlan.scenePath = "scene.json";
    buildPlan.sceneJson = {
        {"objects", nlohmann::json::array({
            CameraObject(),
            TerrainObject("TerrainA", {-6000.0, 100.0, 0.0})})}};
    buildPlan.sceneAssetPlan.scenePath = buildPlan.scenePath;
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(0, "Camera", "camera"));
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(1, "TerrainA", "terrain", TerrainPlan("terrain|TerrainA")));

    const auto result = VL::WorldBuilder().BuildFromLoadedScene(
        1, buildPlan, VL::WorldAssemblyAssets{}, 4);
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;

    // 与 ScriptRuntimeSubsystem::Control 的 Play 路径相同：World 的只读实例
    // 直接交给 ScriptPhysicsWorld，随后脚本查询即可命中地形。
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions(result.Value()->GetTerrainCollisionInstances());
    ASSERT_EQ(physics.TerrainCollisionBodyCount(), 1u);

    const nlohmann::json request = {
        {"id", "ground"},
        {"owner", "none"},
        {"from", {-6000.0, 3000.0, 0.0}},
        {"to", {-6000.0, 0.0, 0.0}}};
    const nlohmann::json hits = physics.Raycast(nlohmann::json::array({request}));
    ASSERT_EQ(hits.size(), 1u);
    ASSERT_TRUE(hits[0].contains("point")) << hits[0].dump();
    EXPECT_NEAR(hits[0].at("point")[1].get<float>(), 600.0f, 0.5f);
    EXPECT_EQ(hits[0].at("blueprint").get<std::string>(), "terrain|TerrainA");
}
