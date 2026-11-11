#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "world/loading/worldBuilder.h"
#include "world/loading/worldGameplayPlanBuilder.h"
#include "world/world.h"

namespace
{

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

nlohmann::json BalloonObject()
{
    return {
        {"name", "Balloon"},
        {"type", "balloon"},
        {"position", {1.0, 2.0, 3.0}},
        {"rotation", {0.0, 90.0, 0.0}},
        {"scale", {2.0, 2.0, 2.0}},
        {"radius", 20.0},
        {"thickness", 0.3},
        {"subdivisions", 1},
        {"siteCount", 8},
        {"pressure", 12.0},
        {"burstStrain", 0.4},
        {"restContraction", 0.7},
        {"seed", 7},
        {"materialInstance", "Maps/SC_test/Materials/MI_balloon.json"}};
}

nlohmann::json GlassObject()
{
    return {
        {"name", "Glass"},
        {"type", "glassPane"},
        {"position", {-4.0, 5.0, 6.0}},
        {"rotation", {0.0, 0.0, 0.0}},
        {"scale", {1.0, 1.0, 1.0}},
        {"width", 120.0},
        {"height", 80.0},
        {"thickness", 0.6},
        {"siteCount", 24},
        {"seed", 11},
        {"subfracture", false},
        {"materialInstance", "Maps/SC_test/Materials/MI_glass.json"}};
}

SceneAssetObjectPlan ObjectPlan(size_t index, const std::string& name, const std::string& type)
{
    SceneAssetObjectPlan plan;
    plan.objectIndex = index;
    plan.objectName = name;
    plan.objectType = type;
    return plan;
}

std::string ReadSourceFile(const std::string& relativePath)
{
    std::ifstream file(std::filesystem::path(VL_SOURCE_DIR) / relativePath);
    EXPECT_TRUE(file.is_open()) << relativePath;
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

} // namespace

TEST(WorldGameplayPlanBuilder, ParsesAuthoringDefaultsWithoutRendererKeys)
{
    SceneAssetBuildPlan scenePlan;
    scenePlan.scenePath = "SC_test.json";
    scenePlan.objectPlans.push_back(ObjectPlan(0, "Balloon", "balloon"));
    scenePlan.objectPlans.push_back(ObjectPlan(1, "Glass", "glassPane"));

    const nlohmann::json sceneJson = {
        {"objects", nlohmann::json::array({BalloonObject(), GlassObject()})},
        {"projectile", {
            {"materialInstance", "Maps/SC_test/Materials/MI_arrow.json"}}}};

    const auto result = VL::WorldGameplayPlanBuilder().Build(
        scenePlan, sceneJson, "SC_test.json");
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const VL::WorldGameplayPlans& plans = result.Value();

    ASSERT_EQ(plans.softBodyObjectPlans.size(), 1u);
    const SoftBodyObjectBuildPlan& balloon = plans.softBodyObjectPlans.front();
    EXPECT_EQ(balloon.objectName, "Balloon");
    EXPECT_EQ(balloon.materialInstancePath,
        "Maps/SC_test/Materials/MI_balloon.json");
    EXPECT_DOUBLE_EQ(balloon.radius, 20.0);
    EXPECT_DOUBLE_EQ(balloon.burstStrain, 0.4);
    EXPECT_EQ(balloon.seed, 7u);
    EXPECT_TRUE((balloon.model.block<3, 1>(0, 3).isApprox(
        Eigen::Vector3f(1.0f, 2.0f, 3.0f))));

    ASSERT_EQ(plans.glassObjectPlans.size(), 1u);
    const GlassObjectBuildPlan& glass = plans.glassObjectPlans.front();
    EXPECT_EQ(glass.materialInstancePath,
        "Maps/SC_test/Materials/MI_glass.json");
    EXPECT_EQ(glass.crackMaterialInstancePath,
        "Common/Materials/MI_glass_crack.json");
    EXPECT_FALSE(glass.subfracture);
    EXPECT_EQ(glass.seed, 11u);

    ASSERT_TRUE(plans.projectile.has_value());
    EXPECT_EQ(plans.projectile->materialInstancePath,
        "Maps/SC_test/Materials/MI_arrow.json");
    EXPECT_FLOAT_EQ(plans.projectile->speedCmPerSecond, 3500.0f);
    EXPECT_FLOAT_EQ(plans.projectile->radiusCm, 1.5f);
    EXPECT_FLOAT_EQ(plans.projectile->massKg, 0.08f);
    EXPECT_FLOAT_EQ(plans.projectile->cooldownSeconds, 0.15f);
}

TEST(WorldGameplayPlanBuilder, RejectsMissingObjectsArray)
{
    SceneAssetBuildPlan scenePlan;
    const auto result = VL::WorldGameplayPlanBuilder().Build(
        scenePlan, nlohmann::json::object(), "SC_bad.json");
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Scene.LoadFailed");
    EXPECT_EQ(result.Error().sourcePath, "SC_bad.json");
}

TEST(WorldBuilderMaterialBinding, ResolvesGameplayMaterialKeys)
{
    VL::WorldBuildPlan buildPlan;
    buildPlan.scenePath = "SC_test.json";
    buildPlan.sceneJson = {
        {"objects", nlohmann::json::array({
            CameraObject(), BalloonObject(), GlassObject()})}};
    buildPlan.sceneAssetPlan.scenePath = buildPlan.scenePath;
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(0, "Camera", "camera"));

    SoftBodyObjectBuildPlan balloon;
    balloon.objectName = "Balloon";
    balloon.sceneObjectIdentity = "Balloon";
    balloon.materialInstancePath = "Maps/SC_test/Materials/MI_balloon.json";
    buildPlan.gameplay.softBodyObjectPlans.push_back(balloon);

    GlassObjectBuildPlan glass;
    glass.objectName = "Glass";
    glass.sceneObjectIdentity = "Glass";
    glass.materialInstancePath = "Maps/SC_test/Materials/MI_glass.json";
    glass.crackMaterialInstancePath = "Common/Materials/MI_glass_crack.json";
    buildPlan.gameplay.glassObjectPlans.push_back(glass);

    VL::ProjectileBuildPlan projectile;
    projectile.materialInstancePath = "Maps/SC_test/Materials/MI_arrow.json";
    projectile.speedCmPerSecond = 1234.0f;
    buildPlan.gameplay.projectile = projectile;

    VL::WorldAssemblyAssets assets;
    assets.materialBindings.emplace(
        "Maps/SC_test/Materials/MI_balloon.json",
        VL::ResolvedMaterialBinding{
            "Maps/SC_test/Materials/MI_balloon.json", "M_balloon", "MI_balloon"});
    assets.materialBindings.emplace(
        "Maps/SC_test/Materials/MI_glass.json",
        VL::ResolvedMaterialBinding{
            "Maps/SC_test/Materials/MI_glass.json", "M_glass", "MI_glass"});
    assets.materialBindings.emplace(
        "Common/Materials/MI_glass_crack.json",
        VL::ResolvedMaterialBinding{
            "Common/Materials/MI_glass_crack.json", "M_crack", "MI_crack"});
    assets.materialBindings.emplace(
        "Maps/SC_test/Materials/MI_arrow.json",
        VL::ResolvedMaterialBinding{
            "Maps/SC_test/Materials/MI_arrow.json", "M_arrow", "MI_arrow"});

    const auto result = VL::WorldBuilder().BuildFromLoadedScene(
        1, buildPlan, std::move(assets), 4);
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const VL::World& world = *result.Value();

    const auto softBody = world.GetSoftBodyObjects().find("Balloon");
    ASSERT_NE(softBody, world.GetSoftBodyObjects().end());
    EXPECT_EQ(softBody->second.materialKey, "M_balloon");
    EXPECT_EQ(softBody->second.materialInstanceKey, "MI_balloon");

    const auto glassObject = world.GetGlassObjects().find("Glass");
    ASSERT_NE(glassObject, world.GetGlassObjects().end());
    EXPECT_EQ(glassObject->second.materialKey, "M_glass");
    EXPECT_EQ(glassObject->second.crackMaterialKey, "M_crack");
    EXPECT_EQ(glassObject->second.crackMaterialInstanceKey, "MI_crack");

    EXPECT_EQ(world.GetProjectileSettings().materialKey, "M_arrow");
    EXPECT_EQ(world.GetProjectileSettings().materialInstanceKey, "MI_arrow");
    EXPECT_FLOAT_EQ(world.GetProjectileSettings().speedCmPerSecond, 1234.0f);
}

TEST(WorldBuilderMaterialBinding, RejectsMissingGameplayBinding)
{
    VL::WorldBuildPlan buildPlan;
    buildPlan.scenePath = "SC_test.json";
    buildPlan.sceneJson = {
        {"objects", nlohmann::json::array({CameraObject(), BalloonObject()})}};
    buildPlan.sceneAssetPlan.scenePath = buildPlan.scenePath;
    buildPlan.sceneAssetPlan.objectPlans.push_back(
        ObjectPlan(0, "Camera", "camera"));

    SoftBodyObjectBuildPlan balloon;
    balloon.objectName = "Balloon";
    balloon.materialInstancePath = "Maps/SC_test/Materials/MI_balloon.json";
    buildPlan.gameplay.softBodyObjectPlans.push_back(balloon);

    const auto result = VL::WorldBuilder().BuildFromLoadedScene(
        1, buildPlan, VL::WorldAssemblyAssets{}, 4);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "WorldBuilder.MaterialBindingMissing");
    EXPECT_EQ(result.Error().sourceNode, "Balloon");
}

TEST(WorldGameplayPlanOwnership, RendererDoesNotParseGameplaySceneFields)
{
    const std::string coordinator = ReadSourceFile(
        "render/resource/rendererResourceLoadCoordinator.cpp");
    EXPECT_EQ(coordinator.find("#include \"world/loading/worldLoader.h\""),
        std::string::npos);
    EXPECT_EQ(coordinator.find("BuildRuntimeObjectModelMatrix"), std::string::npos);
    for (const char* field : {"\"burstStrain\"", "\"restContraction\"",
             "\"subfracture\"", "\"cooldown\"", "\"siteCount\""})
    {
        EXPECT_EQ(coordinator.find(field), std::string::npos) << field;
    }
}
