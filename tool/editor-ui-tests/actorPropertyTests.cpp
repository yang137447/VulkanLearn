#include <gtest/gtest.h>

#include <limits>

#include "editor/scene/editorActorProperty.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "editor/ui/editorActorVisualization.h"
#include "sceneNode.h"
#include "world/world.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;
using P = EditorActorProperty;

EditorActorPropertyEdit Edit(const std::string& identity, P property, uint32_t component, float value)
{
    return {7, identity, property, component, value};
}

TEST(EditorActorVisualization, BlueprintPreviewPublishesVisibleSectionBoundsAsFrozenValues)
{
    World world(7, "Maps/SC_model_preview/SC_model_preview.json");
    world.SetScriptBlueprints(nlohmann::json::array({{
        {"id", "~preview.test"}, {"name", "Preview"}, {"class", "Gameplay.Test"},
        {"properties", nlohmann::json::object()}, {"position", {0, 0, 0}},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"components", nlohmann::json::array({{{"id", "body"}, {"type", "mesh"}}})}}}));
    WorldMeshObject section;
    section.sceneObjectIdentity = "__blueprint/~preview.test/body";
    section.localBoundsMin = Eigen::Vector3f(-2, -3, -4);
    section.localBoundsMax = Eigen::Vector3f(5, 6, 7);
    section.model(0, 1) = 0.5f;
    section.model(0, 3) = 30;
    world.AddMeshObject("Preview::Visible", section);
    section.visible = false;
    world.AddMeshObject("Preview::Hidden", section);
    const auto frozen = BuildEditorSceneActors(world);
    const EditorSceneActorSnapshot* preview = nullptr;
    for (const auto& actor : frozen)
        if (actor.identity == "blueprintInstance|~preview.test") preview = &actor;
    ASSERT_NE(preview, nullptr);
    ASSERT_EQ(preview->scriptComponentBounds.size(), 1u);
    const auto& bounds = preview->scriptComponentBounds[0];
    EXPECT_EQ(bounds.componentId, "body");
    EXPECT_EQ(bounds.minimum, Eigen::Vector3f(-2, -3, -4));
    EXPECT_TRUE(bounds.worldFromLocal.isApprox(section.model));
    section.model(0, 3) = 90;
    world.AddMeshObject("Preview::Visible", section);
    EXPECT_FLOAT_EQ(bounds.worldFromLocal(0, 3), 30);
}

TEST(EditorActorProperty, StaleWorldEditCannotTouchANewSameNamedLight)
{
    World world(8, "scene.json");
    auto light = std::make_shared<PointLight>();
    world.AddPointLight("Key", light);
    std::string error;
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Intensity, 0, 42), error));
    EXPECT_FLOAT_EQ(light->GetIntensity(), 1.0f);
    EXPECT_NE(error.find("World changed"), std::string::npos);
}

TEST(EditorActorProperty, SequentialComponentEditsPreserveEachOther)
{
    World world(7, "scene.json");
    auto light = std::make_shared<PointLight>();
    world.AddPointLight("Key", light);
    std::string error;
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Position, 0, 100), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Position, 1, 200), error));
    EXPECT_EQ(light->GetPosition(), Eigen::Vector3f(100, 200, 0));
}

TEST(EditorActorProperty, InvalidNumbersTypesAndComponentsLeaveLightUntouched)
{
    World world(7, "scene.json");
    auto light = std::make_shared<PointLight>();
    world.AddPointLight("Key", light);
    std::string error;
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Intensity, 0, -1), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Position, 0,
        std::numeric_limits<float>::infinity()), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Position, 3, 4), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("spotLight|Key", P::Intensity, 0, 4), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::CameraFov, 0, 45), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("pointLight|Key", P::Color, 0, 2), error));
    EXPECT_EQ(light->GetPosition(), Eigen::Vector3f::Zero());
    EXPECT_FLOAT_EQ(light->GetIntensity(), 1);
}

TEST(EditorActorProperty, MeshEditMovesAllSectionsAndPreservesTheirRelativeMatrices)
{
    World world(7, "scene.json");
    WorldMeshObject section;
    section.sceneObjectIdentity = "Model";
    section.sceneScale = Eigen::Vector3f(-2, 3, 4);
    SceneNode root;
    root.SetTransform(section.scenePosition, section.sceneRotation, section.sceneScale);
    section.model = root.GetModelMatrix();
    world.AddMeshObject("Model::A", section);
    Eigen::Matrix4f local = Eigen::Matrix4f::Identity();
    local(0, 3) = 12;
    section.model *= local;
    world.AddMeshObject("Model::B", section);
    std::string error;
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("mesh|Model", P::Position, 0, 100), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("mesh|Model", P::Rotation, 1, 45), error));
    const auto& a = world.GetMeshObjects().at("Model::A");
    const auto& b = world.GetMeshObjects().at("Model::B");
    EXPECT_TRUE((a.model.inverse() * b.model).isApprox(local, 1.0e-5f));
    EXPECT_EQ(a.sceneScale, Eigen::Vector3f(-2, 3, 4));
    EXPECT_EQ(a.scenePosition, b.scenePosition);
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("mesh|Model", P::Scale, 0, 0), error));
    EXPECT_EQ(world.GetMeshObjects().at("Model::A").sceneScale, Eigen::Vector3f(-2, 3, 4));
}

TEST(EditorActorProperty, SpotHalfAnglesAreValidatedTogetherAndConvertedToRadians)
{
    World world(7, "scene.json");
    auto light = std::make_shared<SpotLight>();
    light->SetConeAngleInner(0.2f);
    light->SetConeAngleOuter(0.7f);
    world.AddSpotLight("Spot", light);
    std::string error;
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("spotLight|Spot", P::InnerCone, 0, 60), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("spotLight|Spot", P::OuterCone, 0, 60), error));
    EXPECT_NEAR(light->GetConeAngleOuter(), 1.04719755f, 1.0e-6f);
}

TEST(EditorActorProperty, PrimaryLightRotationAlsoUpdatesTheSkyDirection)
{
    World world(7, "scene.json");
    auto light = std::make_shared<DirectionalLight>();
    world.AddDirectionalLight("Sun", light);
    std::string error;
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("directionalLight|Sun", P::Rotation, 1, 90), error));
    EXPECT_TRUE(world.GetEnvironment().skyParameters.sunDirectionIntensity.head<3>().isApprox(
        -light->GetForwardVector()));
}

TEST(EditorActorProperty, CameraLensEditsUpdateProjectionAndRejectCrossedClipPlanes)
{
    World world(7, "scene.json");
    auto camera = std::make_shared<Camera>();
    camera->SetName("Shot");
    camera->SetProjection(80, 1.5f, 10, 10000);
    world.SetCamera(camera);
    const float initial = camera->GetProjectionMatrix()(0, 0);
    std::string error;
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraFov, 0, 40), error));
    EXPECT_GT(camera->GetProjectionMatrix()(0, 0), initial);
    EXPECT_NEAR(std::abs(camera->GetProjectionMatrix()(1, 1) / camera->GetProjectionMatrix()(0, 0)), 1.5f, 1.0e-6f);
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraNear, 0, 20000), error));
    EXPECT_FLOAT_EQ(camera->GetClipNear(), 10);
}

TEST(EditorActorProperty, EditorNavigationDoesNotMoveTheSceneCamera)
{
    World world(7, "scene.json");
    auto camera = std::make_shared<Camera>();
    camera->SetName("Shot");
    camera->SetPosition(Eigen::Vector3f(100, 200, 300));
    world.SetCamera(camera);
    world.SetEditorViewEnabled(true);
    ASSERT_NE(world.GetRenderCamera(), world.GetCamera());
    world.GetRenderCamera()->SetPosition(Eigen::Vector3f(400, 500, 600));
    EXPECT_EQ(camera->GetPosition(), Eigen::Vector3f(100, 200, 300));
    const auto actors = BuildEditorSceneActors(world);
    ASSERT_EQ(actors.front().type, EditorSceneActorType::Camera);
    EXPECT_EQ(actors.front().camera.position, camera->GetPosition());
    EXPECT_EQ(actors.front().viewportCamera.position, Eigen::Vector3f(400, 500, 600));
    world.SetEditorViewEnabled(false);
    EXPECT_EQ(world.GetRenderCamera(), camera);
}

TEST(EditorActorProperty, CameraExposureEditsPreservePhysicalFieldsAndSequentialValues)
{
    World world(7, "scene.json");
    auto camera = std::make_shared<Camera>();
    camera->SetName("Shot");
    CameraExposure exposure;
    exposure.mode = ExposureMode::PhysicalCamera;
    exposure.aperture = 2.8;
    exposure.iso = 640;
    camera->SetExposure(exposure);
    world.SetCamera(camera);
    std::string error;
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraExposureCompensation, 0, 2), error));
    EXPECT_EQ(camera->GetExposure().mode, ExposureMode::PhysicalCamera);
    EXPECT_DOUBLE_EQ(camera->GetExposure().aperture, 2.8);
    EXPECT_DOUBLE_EQ(camera->GetExposure().iso, 640);
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraExposureMode, 0, 2), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoMinEv100, 0, -8), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoMaxEv100, 0, 20), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoSpeedUp, 0, 4), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoSpeedDown, 0, 2), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoTargetGray, 0, 0.25f), error));
    ASSERT_TRUE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraEv100, 0, 11), error));
    world.SetEditorViewEnabled(true);
    const auto actors = BuildEditorSceneActors(world);
    const auto& frozen = actors.front().camera.exposure;
    EXPECT_EQ(frozen.mode, ExposureMode::Auto);
    EXPECT_DOUBLE_EQ(frozen.compensation, 2);
    EXPECT_DOUBLE_EQ(frozen.ev100, 11);
    EXPECT_DOUBLE_EQ(frozen.autoMinEv100, -8);
    EXPECT_DOUBLE_EQ(frozen.autoMaxEv100, 20);
    EXPECT_DOUBLE_EQ(frozen.autoSpeedUp, 4);
    EXPECT_DOUBLE_EQ(frozen.autoSpeedDown, 2);
    EXPECT_DOUBLE_EQ(frozen.autoTargetGray, 0.25);
    camera->SetExposure(CameraExposure{});
    EXPECT_EQ(frozen.mode, ExposureMode::Auto);
}

TEST(EditorActorProperty, InvalidOrStaleCameraExposureEditsAreAtomic)
{
    World world(7, "scene.json");
    auto camera = std::make_shared<Camera>();
    camera->SetName("Shot");
    world.SetCamera(camera);
    std::string error;
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraExposureMode, 0, 3), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoMinEv100, 0, 20), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoMaxEv100, 0, -10), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoSpeedUp, 0, 0), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraAutoTargetGray, 0, 1.1f), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Shot", P::CameraEv100, 1, 10), error));
    EXPECT_FALSE(ApplyEditorActorProperty(world, Edit("camera|Other", P::CameraEv100, 0, 10), error));
    auto stale = Edit("camera|Shot", P::CameraEv100, 0, 10);
    stale.worldGeneration = 6;
    EXPECT_FALSE(ApplyEditorActorProperty(world, stale, error));
    EXPECT_EQ(DescribeCameraExposure(camera->GetExposure()), DescribeCameraExposure(CameraExposure{}));
}

TEST(EditorActorVisualization, ProjectionUsesHorizontalFovAndDownwardScreenY)
{
    EditorSceneCameraActor view;
    view.horizontalFovDegrees = 90;
    Eigen::Vector2f pixel;
    ASSERT_TRUE(ProjectActorVisualPoint(view, {0, 0, -100}, 800, 400, pixel));
    EXPECT_TRUE(pixel.isApprox(Eigen::Vector2f(400, 200)));
    ASSERT_TRUE(ProjectActorVisualPoint(view, {20, 20, -100}, 800, 400, pixel));
    EXPECT_NEAR(pixel.x(), 480, 0.001f);
    EXPECT_NEAR(pixel.y(), 120, 0.001f);
    EXPECT_FALSE(ProjectActorVisualPoint(view, {0, 0, 100}, 800, 400, pixel));
}

TEST(EditorActorProperty, FramingUnionsOffsetSectionsAndPreservesTheAuthoredCamera)
{
    World world(7, "scene.json");
    auto camera = std::make_shared<Camera>();
    camera->SetProjection(30, 2.0f, 10, 200);
    const Eigen::Vector3f initial = camera->GetPosition();
    world.SetCamera(camera);
    world.SetEditorViewEnabled(true);
    WorldMeshObject object;
    object.sceneObjectIdentity = "Model";
    object.localBoundsMin = Eigen::Vector3f(-100, -100, -100);
    object.localBoundsMax = Eigen::Vector3f(100, 100, 100);
    object.model(0, 3) = 10000;
    world.AddMeshObject("Model::A", object);
    object.model(0, 3) = 12000;
    world.AddMeshObject("Model::B", object);
    ASSERT_TRUE(FrameEditorActor(world, 7, "mesh|Model"));
    const auto actors = BuildEditorSceneActors(world);
    ASSERT_EQ(actors[1].type, EditorSceneActorType::Camera);
    const auto& view = actors[1].viewportCamera;
    Eigen::Vector2f pixel;
    ASSERT_TRUE(ProjectActorVisualPoint(view, {11000, 0, 0}, 800, 400, pixel));
    EXPECT_TRUE(pixel.isApprox(Eigen::Vector2f(400, 200), 0.001f));
    for (float x : {9900.0f, 12100.0f})
        for (float y : {-100.0f, 100.0f})
            for (float z : {-100.0f, 100.0f})
                EXPECT_TRUE(ProjectActorVisualPoint(view, {x, y, z}, 800, 400, pixel));
    EXPECT_EQ(camera->GetPosition(), initial);
    EXPECT_FLOAT_EQ(camera->GetClipFar(), 200);
}

TEST(EditorActorProperty, FramingRejectsStaleOrUnknownActorsWithoutMovingTheObserver)
{
    World world(7, "scene.json");
    world.SetCamera(std::make_shared<Camera>());
    world.SetEditorViewEnabled(true);
    const Eigen::Vector3f initial = world.GetRenderCamera()->GetPosition();
    EXPECT_FALSE(FrameEditorActor(world, 6, "camera|Camera"));
    EXPECT_FALSE(FrameEditorActor(world, 7, "mesh|missing"));
    EXPECT_EQ(world.GetRenderCamera()->GetPosition(), initial);
}

TEST(EditorActorVisualization, LinesCrossingTheNearPlaneAreClippedBeforeProjection)
{
    EditorSceneCameraActor view;
    view.clipNear = 10;
    std::vector<EditorActorVisualLine> lines;
    AddActorVisualLine(lines, view, {0, 0, 50}, {20, 0, -100}, 800, 400);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(lines.front().begin.allFinite());
    EXPECT_TRUE(lines.front().end.allFinite());
    for (const auto point : {lines.front().begin, lines.front().end})
    {
        EXPECT_GE(point.x(), 0);
        EXPECT_LE(point.x(), 800);
        EXPECT_GE(point.y(), 0);
        EXPECT_LE(point.y(), 400);
    }
}

TEST(EditorActorVisualization, ExternalObserverCanSeeTheSceneCameraFrustum)
{
    EditorSceneCameraActor view;
    view.position = Eigen::Vector3f(400, 300, 600);
    const Eigen::Vector3f direction = -view.position.normalized();
    view.forward = direction;
    view.right = direction.cross(Eigen::Vector3f::UnitY()).normalized();
    view.up = view.right.cross(direction);
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Camera;
    const auto lines = BuildActorVisualLines(actor, view, 800, 400);
    EXPECT_EQ(lines.size(), 8u);
}

TEST(EditorActorVisualization, ScriptBoxOutlineUsesActorRotationScaleAndCentimeterExtents)
{
    EditorSceneCameraActor view;
    view.position = {0, 0, 1000}; view.clipFar = 5000;
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Script;
    actor.rotation = {0, 30, 0}; actor.scale = {2, 2, 2};
    actor.scriptPhysics = {{"shape", "box"}, {"halfExtents", {50, 100, 50}}};
    const auto lines = BuildActorVisualLines(actor, view, 800, 400);
    ASSERT_EQ(lines.size(), 12u);
    for (const auto& line : lines)
    {
        EXPECT_TRUE(line.begin.allFinite());
        EXPECT_TRUE(line.end.allFinite());
        EXPECT_GE(line.begin.x(), 0); EXPECT_LE(line.end.x(), 800);
    }
}

TEST(EditorActorVisualization, ScriptCapsuleOutlineHasOnlyVerticalStraightSeams)
{
    EditorSceneCameraActor view;
    view.position = {0, 0, 1000}; view.clipFar = 5000;
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Script;
    actor.scriptPhysics = {{"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    const auto lines = BuildActorVisualLines(actor, view, 800, 400);
    ASSERT_EQ(lines.size(), 100u);
    for (const auto& line : lines)
    {
        ASSERT_TRUE(line.begin.allFinite());
        ASSERT_TRUE(line.end.allFinite());
        // 半球/圆环的小段不会越过整个直段；跨直段的线必须竖直。
        if (std::abs(line.end.y() - line.begin.y()) > 20)
            EXPECT_NEAR(line.begin.x(), line.end.x(), 1e-4f);
    }
}
} // namespace
