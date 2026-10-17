#include <gtest/gtest.h>
#include "engine/modelPreviewCamera.h"
#include "engine/modelPreviewRequest.h"
#include "engine/blueprintPreviewRequest.h"
#include "script/scriptComponents.h"
#include "sceneNode.h"
#include "world/world.h"

namespace VL
{
TEST(BlueprintPreviewRequest, UsesAcceptedVisualDraftAndPreservesComponentHierarchy)
{
    const nlohmann::json draft{{"class", "Gameplay.Test"}, {"properties", {{"Secret", 42}}},
        {"scale", {2, 3, 4}}, {"rotation", {0, 45, 0}},
        {"components", nlohmann::json::array({{{"id", "pivot"}, {"type", "scene"}, {"scale", {2, 1, 1}}},
            {{"id", "mesh"}, {"parent", "pivot"}, {"type", "mesh"}, {"modelPath", "Common/SM_test.json"},
                {"rotation", {0, 30, 0}}}})}};
    const auto original = draft;
    const auto actor = BuildBlueprintPreviewObjects("Blueprints/BP_test.json", 12, 12, draft)[0];
    EXPECT_EQ(actor.at("components"), draft.at("components"));
    EXPECT_EQ(actor.at("scale"), draft.at("scale"));
    EXPECT_TRUE(actor.at("properties").empty());
    EXPECT_FALSE(actor.contains("__instanceEditable"));
    EXPECT_EQ(draft, original);
    EXPECT_THROW(BuildBlueprintPreviewObjects("Blueprints/BP_test.json", 11, 12, draft), std::invalid_argument);
    EXPECT_NE(BlueprintPreviewIdentity("Blueprints/BP_test.json", 11), actor.at("id"));
}

TEST(BlueprintPreviewCamera, FramesFullParentMatricesAcrossMeshComponents)
{
    World world{8, ModelPreviewScenePath};
    world.SetCamera(std::make_shared<Camera>());
    const nlohmann::json draft{{"class", "Gameplay.Test"},
        {"components", nlohmann::json::array({{{"id", "pivot"}, {"type", "scene"}, {"position", {100, 0, 0}},
            {"scale", {2, 1, 1}}}, {{"id", "a"}, {"parent", "pivot"}, {"type", "mesh"},
            {"modelPath", "SM_test.json"}, {"rotation", {0, 45, 0}}}, {{"id", "b"}, {"type", "mesh"},
            {"modelPath", "SM_test.json"}, {"position", {-100, 0, 0}}}})}};
    const auto actor = BuildBlueprintPreviewObjects("Blueprints/BP_test.json", 1, 1, draft)[0];
    Eigen::Vector3f lower = Eigen::Vector3f::Constant(1e6f), upper = -lower;
    for (size_t index : {1u, 2u})
    {
        WorldMeshObject mesh;
        mesh.sceneObjectIdentity = ScriptMeshIdentity(actor.at("id"), actor.at("components")[index]);
        mesh.localBoundsMin = Eigen::Vector3f::Constant(-10);
        mesh.localBoundsMax = Eigen::Vector3f::Constant(10);
        world.AddMeshObject(mesh.sceneObjectIdentity, mesh);
        const auto transform = ScriptComponentMatrix(actor, index);
        for (float x : {-10.f, 10.f}) for (float y : {-10.f, 10.f}) for (float z : {-10.f, 10.f})
        {
            const Eigen::Vector3f point = (transform * Eigen::Vector4f(x, y, z, 1)).head<3>();
            lower = lower.cwiseMin(point); upper = upper.cwiseMax(point);
        }
    }
    world.ApplyScriptBlueprintVisuals(actor);
    world.SetScriptBlueprints(nlohmann::json::array({actor}));
    ModelPreviewCamera camera;
    ASSERT_TRUE(camera.Synchronize(world, 1));
    const Eigen::Vector3f center = (lower + upper) * 0.5f;
    EXPECT_NEAR(world.GetCamera()->GetPosition().x(), center.x(), 1e-4f);
    EXPECT_TRUE(world.GetCamera()->GetForwardVector().isApprox(
        (center - world.GetCamera()->GetPosition()).normalized()));
}

TEST(BlueprintPreviewCamera, EmptyActorStillSupportsNavigation)
{
    World world{9, ModelPreviewScenePath};
    world.SetCamera(std::make_shared<Camera>());
    world.SetScriptBlueprints(BuildBlueprintPreviewObjects("Blueprints/BP_empty.json", 1, 1, {{"class", "Gameplay.Test"}}));
    ModelPreviewCamera camera;
    ASSERT_TRUE(camera.Synchronize(world, 1));
    EXPECT_TRUE(camera.Apply(world, {ModelPreviewNavigation::Orbit, 9, 20, 10}, 1, 300));
    EXPECT_TRUE(world.GetCamera()->GetViewMatrix().allFinite());
}

TEST(MaterialPreviewRequest, UsesFormalSphereWithInstanceOnlyMaterialBinding)
{
    const auto objects = BuildMaterialPreviewObjects("Common/Materials/MI_test.json");
    ASSERT_EQ(objects.size(), 1u);
    EXPECT_EQ(objects[0].at("modelPath"), "Common/Meshes/SM_material_preview_ball.json");
    EXPECT_EQ(objects[0].at("materialOverride"), "Common/Materials/MI_test.json");
    EXPECT_EQ(objects[0].at("name"), "preview_material");
    EXPECT_FALSE(BuildModelPreviewObjects("SM_test.json")[0].contains("materialOverride"));
}

TEST(MaterialPreviewRequest, SphereSupportsTheSharedOrbitCamera)
{
    World world{7, ModelPreviewScenePath};
    world.SetCamera(std::make_shared<Camera>());
    WorldMeshObject sphere;
    sphere.sceneObjectIdentity = "preview_material";
    sphere.localBoundsMin = Eigen::Vector3f::Constant(-50);
    sphere.localBoundsMax = Eigen::Vector3f::Constant(50);
    world.AddMeshObject("sphere", sphere);
    ModelPreviewCamera camera;
    ASSERT_TRUE(camera.Synchronize(world, 1.0f));
    EXPECT_TRUE(camera.Apply(world, {ModelPreviewNavigation::Orbit, 7, 80, 20}, 1.0f, 400));
    EXPECT_TRUE(world.GetCamera()->GetViewMatrix().allFinite());
}

class ModelPreviewCameraTest : public testing::Test
{
protected:
    World world{1, "Maps/SC_model_preview/SC_model_preview.json"};
    ModelPreviewCamera navigation;
    void SetUp() override
    {
        world.SetCamera(std::make_shared<Camera>());
        WorldMeshObject mesh;
        mesh.sceneObjectIdentity = "preview_model";
        mesh.localBoundsMin = Eigen::Vector3f::Constant(-1);
        mesh.localBoundsMax = Eigen::Vector3f::Constant(1);
        world.AddMeshObject("model", mesh);
        ASSERT_TRUE(navigation.Synchronize(world, 1.5f));
    }
    bool Apply(ModelPreviewNavigation mode, float x = 0, float y = 0, uint64_t generation = 1)
    {
        return navigation.Apply(world, {mode, generation, x, y}, 1.5f, 600);
    }
};

TEST_F(ModelPreviewCameraTest, OrbitPreservesDistanceAndLooksAtModel)
{
    const auto initial = world.GetCamera()->GetPosition();
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Orbit, 100, 50));
    const auto camera = world.GetCamera();
    EXPECT_FALSE(camera->GetPosition().isApprox(initial));
    EXPECT_NEAR(camera->GetPosition().norm(), initial.norm(), 1e-5f);
    EXPECT_TRUE(camera->GetForwardVector().isApprox(-camera->GetPosition().normalized(), 1e-5f));
}

TEST_F(ModelPreviewCameraTest, ZoomDirectionAndFrameReset)
{
    const auto initial = world.GetCamera()->GetPosition();
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Zoom, 0, -2));
    EXPECT_LT(world.GetCamera()->GetPosition().norm(), initial.norm());
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Zoom, 0, 4));
    EXPECT_GT(world.GetCamera()->GetPosition().norm(), initial.norm());
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Frame));
    EXPECT_TRUE(world.GetCamera()->GetPosition().isApprox(initial));
}

TEST_F(ModelPreviewCameraTest, PanMovesPositionWithoutChangingOrientation)
{
    const auto initial = world.GetCamera()->GetPosition();
    const auto forward = world.GetCamera()->GetForwardVector();
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Pan, 50, 20));
    EXPECT_FALSE(world.GetCamera()->GetPosition().isApprox(initial));
    EXPECT_TRUE(world.GetCamera()->GetForwardVector().isApprox(forward));
}

TEST_F(ModelPreviewCameraTest, RejectsStaleInputAndLevelWorld)
{
    const auto initial = world.GetCamera()->GetPosition();
    EXPECT_FALSE(Apply(ModelPreviewNavigation::Orbit, 100, 20, 2));
    EXPECT_TRUE(world.GetCamera()->GetPosition().isApprox(initial));
    World level{2, "Maps/SC_level/SC_level.json"};
    level.SetCamera(std::make_shared<Camera>());
    EXPECT_FALSE(navigation.Synchronize(level, 1.5f));
    EXPECT_FALSE(navigation.Apply(level, {ModelPreviewNavigation::Zoom, 2, 0, 1}, 1.5f, 600));
}

TEST_F(ModelPreviewCameraTest, ExtremeOrbitAndZoomRemainFinite)
{
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Orbit, 1e8f, 1e8f));
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Zoom, 0, -1e8f));
    EXPECT_TRUE(world.GetCamera()->GetViewMatrix().allFinite());
    EXPECT_GT(world.GetCamera()->GetPosition().norm(), world.GetCamera()->GetClipNear());
    ASSERT_TRUE(Apply(ModelPreviewNavigation::Zoom, 0, 1e8f));
    EXPECT_LT(world.GetCamera()->GetPosition().norm(), world.GetCamera()->GetClipFar());
}

TEST_F(ModelPreviewCameraTest, FramesTransformedSectionsAndExcludesBackdrop)
{
    World next{2, "Maps/SC_model_preview/SC_model_preview.json"};
    next.SetCamera(std::make_shared<Camera>());
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = "preview_model";
    mesh.localBoundsMin = Eigen::Vector3f::Constant(-1);
    mesh.localBoundsMax = Eigen::Vector3f::Constant(1);
    mesh.model(0, 3) = 10;
    next.AddMeshObject("a", mesh);
    mesh.model(0, 3) = 20;
    next.AddMeshObject("b", mesh);
    mesh.sceneObjectIdentity = "neutral_backdrop";
    mesh.model(0, 3) = 10000;
    next.AddMeshObject("backdrop", mesh);
    ASSERT_TRUE(navigation.Synchronize(next, 1.5f));
    EXPECT_NEAR(next.GetCamera()->GetPosition().x(), 15, 1e-5f);
    EXPECT_LT(next.GetCamera()->GetPosition().z(), 100);
    EXPECT_FALSE(next.IsMeshSceneObjectVisible("neutral_backdrop"));
    EXPECT_FALSE(navigation.Synchronize(next, 1.5f));
}
}
