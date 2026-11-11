#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "world/loading/terrainCollisionInstances.h"

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

VL::TerrainBuildPlan MakePlan(const std::string& placementId)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_world_collision.json";
    plan.desc.name = "TR_world_collision";
    plan.desc.size = Eigen::Vector2f(10000.0f, 10000.0f);
    plan.desc.heightScale = 500.0f;
    plan.desc.gridResolution = 64;
    plan.desc.chunkQuads = 16;
    plan.heightmap = FlatHeightmap();
    plan.placementId = placementId;
    return plan;
}

nlohmann::json TerrainObject(const std::string& name, const nlohmann::json& position)
{
    return {
        {"name", name},
        {"type", "terrain"},
        {"terrainPath", "Generated/Validation/TerrainTests/TR_world_collision.json"},
        {"position", position},
        {"rotation", {0.0, 30.0, 0.0}},
        {"scale", {1.0, 2.0, 1.0}}};
}

SceneAssetObjectPlan ObjectPlan(
    size_t index,
    const std::string& name,
    std::optional<VL::TerrainBuildPlan> plan)
{
    SceneAssetObjectPlan objectPlan;
    objectPlan.objectIndex = index;
    objectPlan.objectName = name;
    objectPlan.objectType = "terrain";
    objectPlan.terrainBuildPlan = std::move(plan);
    return objectPlan;
}

} // namespace

TEST(TerrainCollisionInstances, CollectsEveryPlacementWithAuthorTransform)
{
    SceneAssetBuildPlan scenePlan;
    scenePlan.scenePath = "Generated/Validation/TerrainTests/SC_collision.json";
    scenePlan.objectPlans.push_back(ObjectPlan(
        0, "TerrainA", MakePlan("terrain|TerrainA")));
    scenePlan.objectPlans.push_back(ObjectPlan(
        1, "TerrainB", MakePlan("terrain|TerrainB")));

    const nlohmann::json sceneJson = {
        {"objects", nlohmann::json::array({
            TerrainObject("TerrainA", {-6000.0, 100.0, 0.0}),
            TerrainObject("TerrainB", {6000.0, -50.0, 200.0})})}};

    const auto result = VL::BuildTerrainCollisionInstances(scenePlan, sceneJson);
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const auto& instances = result.Value();
    ASSERT_EQ(instances.size(), 2u);
    EXPECT_EQ(instances[0].placementId, "terrain|TerrainA");
    EXPECT_EQ(instances[1].placementId, "terrain|TerrainB");
    EXPECT_TRUE(instances[0].placement.position.isApprox(
        Eigen::Vector3f(-6000.0f, 100.0f, 0.0f)));
    EXPECT_TRUE(instances[0].placement.rotation.isApprox(
        Eigen::Vector3f(0.0f, 30.0f, 0.0f)));
    EXPECT_TRUE(instances[0].placement.scale.isApprox(
        Eigen::Vector3f(1.0f, 2.0f, 1.0f)));
    EXPECT_TRUE(instances[1].placement.position.isApprox(
        Eigen::Vector3f(6000.0f, -50.0f, 200.0f)));
    EXPECT_EQ(instances[0].plan.desc.assetPath,
              "Generated/Validation/TerrainTests/TR_world_collision.json");
}

TEST(TerrainCollisionInstances, MissingPreparedTerrainPlanFailsAtLoad)
{
    SceneAssetBuildPlan scenePlan;
    scenePlan.scenePath = "scene.json";
    scenePlan.objectPlans.push_back(ObjectPlan(0, "TerrainA", std::nullopt));
    const nlohmann::json sceneJson = {
        {"objects", nlohmann::json::array({
            TerrainObject("TerrainA", {0.0, 0.0, 0.0})})}};

    const auto result = VL::BuildTerrainCollisionInstances(scenePlan, sceneJson);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "TerrainCollision.MissingPlan");
}

TEST(TerrainCollisionInstances, NonPositiveScaleFailsAtLoad)
{
    SceneAssetBuildPlan scenePlan;
    scenePlan.scenePath = "scene.json";
    scenePlan.objectPlans.push_back(ObjectPlan(
        0, "TerrainA", MakePlan("terrain|TerrainA")));
    nlohmann::json object = TerrainObject("TerrainA", {0.0, 0.0, 0.0});
    object["scale"] = {1.0, 0.0, 1.0};
    const nlohmann::json sceneJson = {
        {"objects", nlohmann::json::array({object})}};

    const auto result = VL::BuildTerrainCollisionInstances(scenePlan, sceneJson);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "TerrainCollision.NonPositiveScale");
}

TEST(TerrainCollisionInstances, DuplicatePlacementIdentityFailsAtLoad)
{
    SceneAssetBuildPlan scenePlan;
    scenePlan.scenePath = "scene.json";
    scenePlan.objectPlans.push_back(ObjectPlan(
        0, "TerrainA", MakePlan("terrain|Shared")));
    scenePlan.objectPlans.push_back(ObjectPlan(
        1, "TerrainB", MakePlan("terrain|Shared")));
    const nlohmann::json sceneJson = {
        {"objects", nlohmann::json::array({
            TerrainObject("TerrainA", {0.0, 0.0, 0.0}),
            TerrainObject("TerrainB", {1000.0, 0.0, 0.0})})}};

    const auto result = VL::BuildTerrainCollisionInstances(scenePlan, sceneJson);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "TerrainCollision.DuplicatePlacement");
}

TEST(TerrainCollisionInstances, NonTerrainObjectsAreIgnored)
{
    SceneAssetBuildPlan scenePlan;
    scenePlan.scenePath = "scene.json";
    SceneAssetObjectPlan camera;
    camera.objectIndex = 0;
    camera.objectName = "Camera";
    camera.objectType = "camera";
    scenePlan.objectPlans.push_back(camera);
    scenePlan.objectPlans.push_back(ObjectPlan(
        1, "TerrainA", MakePlan("terrain|TerrainA")));
    const nlohmann::json sceneJson = {
        {"objects", nlohmann::json::array({
            {{"name", "Camera"}, {"type", "camera"},
             {"position", {0.0, 0.0, 0.0}}, {"rotation", {0.0, 0.0, 0.0}},
             {"scale", {1.0, 1.0, 1.0}}},
            TerrainObject("TerrainA", {0.0, 0.0, 0.0})})}};

    const auto result = VL::BuildTerrainCollisionInstances(scenePlan, sceneJson);
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    ASSERT_EQ(result.Value().size(), 1u);
    EXPECT_EQ(result.Value()[0].placementId, "terrain|TerrainA");
}
