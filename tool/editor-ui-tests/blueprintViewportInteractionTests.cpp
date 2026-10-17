#include <gtest/gtest.h>

#include <limits>

#include "editor/ui/blueprintViewportInteraction.h"
#include "engine/blueprintPreviewTransform.h"
#include "sceneNode.h"
#include "world/worldSnapshotBuilder.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;

EditorSceneCameraActor Camera()
{
    EditorSceneCameraActor camera;
    camera.position = {0, 0, 10};
    camera.horizontalFovDegrees = 90;
    camera.clipNear = 1;
    camera.clipFar = 100;
    return camera;
}

BlueprintViewportBounds Box(const std::string& id, float z = 0)
{
    BlueprintViewportBounds bounds;
    bounds.componentId = id;
    bounds.minimum = Eigen::Vector3f::Constant(-1);
    bounds.maximum = Eigen::Vector3f::Constant(1);
    bounds.worldFromLocal(2, 3) = z;
    return bounds;
}

BlueprintViewportGizmo Gizmo(BlueprintGizmoMode mode,
    const Eigen::Matrix4f& parent = Eigen::Matrix4f::Identity(),
    const Eigen::Quaternionf& rotation = Eigen::Quaternionf::Identity(),
    const Eigen::Vector3f& localScale = Eigen::Vector3f::Ones())
{
    return BuildBlueprintViewportGizmo(Camera(), 800, 600, parent, rotation,
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Zero(), localScale, mode);
}

Eigen::Vector2f Pixel(const Eigen::Vector3f& world)
{
    Eigen::Vector2f pixel;
    EXPECT_TRUE(ProjectBlueprintViewportPoint(Camera(), 800, 600, world, pixel));
    return pixel;
}

TEST(BlueprintViewportInteraction, HorizontalFovProjectionAndInversePickAgree)
{
    Eigen::Vector2f pixel;
    ASSERT_TRUE(ProjectBlueprintViewportPoint(Camera(), 800, 600, {2, 1, 0}, pixel));
    EXPECT_TRUE(pixel.isApprox(Eigen::Vector2f(480, 260)));
    auto bounds = Box("offset");
    bounds.worldFromLocal(0, 3) = 2;
    bounds.worldFromLocal(1, 3) = 1;
    const auto hit = PickBlueprintViewportComponent({bounds}, Camera(), 800, 600, pixel);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->componentId, "offset");
}

TEST(BlueprintViewportInteraction, PickingReturnsNearestStableComponentAcrossInputOrder)
{
    const auto hit = PickBlueprintViewportComponent({Box("back", -4), Box("front", 3)},
        Camera(), 800, 600, {400, 300});
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->componentId, "front");
    EXPECT_FLOAT_EQ(hit->distance, 6);
}

TEST(BlueprintViewportInteraction, CompleteParentChainIncludingMirroringAndShearIsPreserved)
{
    const nlohmann::json blueprint{{"position", {3, 0, 0}}, {"scale", {-2, 3, 1}},
        {"components", nlohmann::json::array({
            {{"id", "parent"}, {"type", "scene"}, {"rotation", {0, 0, 45}}},
            {{"id", "child"}, {"type", "scene"}, {"parent", "parent"}, {"position", {1, 0, 0}}}})}};
    auto bounds = Box("child");
    bounds.worldFromLocal = ScriptComponentMatrix(blueprint, 1);
    const Eigen::Vector3f center = bounds.worldFromLocal.block<3, 1>(0, 3);
    const auto hit = PickBlueprintViewportComponent({bounds}, Camera(), 800, 600, Pixel(center));
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->componentId, "child");
    EXPECT_FALSE((bounds.worldFromLocal.block<3, 3>(0, 0).col(0).normalized().isOrthogonal(
        bounds.worldFromLocal.block<3, 3>(0, 0).col(1).normalized(), 1e-3f)));
}

TEST(BlueprintViewportInteraction, ShearedBoxDoesNotPickEmptyCornersOfItsWorldAabb)
{
    auto bounds = Box("sheared");
    bounds.worldFromLocal(0, 1) = 2;
    const auto hit = PickBlueprintViewportComponent({bounds}, Camera(), 800, 600, Pixel({2.5f, 0, 0}));
    EXPECT_FALSE(hit);
}

TEST(BlueprintViewportInteraction, PickingClipsAtNearAndFarAndAcceptsNearPlaneInsideBox)
{
    auto camera = Camera();
    camera.clipNear = 4;
    camera.clipFar = 12;
    EXPECT_FALSE(PickBlueprintViewportComponent({Box("before-near", 8)}, camera, 800, 600, {400, 300}));
    EXPECT_FALSE(PickBlueprintViewportComponent({Box("after-far", -4)}, camera, 800, 600, {400, 300}));
    const auto hit = PickBlueprintViewportComponent({Box("cross-near", 6)}, camera, 800, 600, {400, 300});
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit->distance, 4);
}

TEST(BlueprintViewportInteraction, PickingBoxAcrossEyeStartsAtNearPlaneInsteadOfBehindCamera)
{
    auto bounds = Box("across-eye");
    bounds.minimum = {-1, -1, 3};
    bounds.maximum = {1, 1, 12};
    auto camera = Camera();
    camera.clipNear = 4;
    const auto hit = PickBlueprintViewportComponent({bounds}, camera, 800, 600, {400, 300});
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit->distance, 4);
}

TEST(BlueprintViewportInteraction, PickingUsesHalfOpenViewportAndRejectsNonFiniteInput)
{
    EXPECT_FALSE(PickBlueprintViewportComponent({Box("box")}, Camera(), 800, 600, {800, 300}));
    EXPECT_FALSE(PickBlueprintViewportComponent({Box("box")}, Camera(), 800, 600, {400, 600}));
    EXPECT_FALSE(PickBlueprintViewportComponent({Box("box")}, Camera(), 800, 600,
        {std::numeric_limits<float>::quiet_NaN(), 0}));
    auto camera = Camera();
    camera.horizontalFovDegrees = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(PickBlueprintViewportComponent({Box("box")}, camera, 800, 600, {400, 300}));
}

TEST(BlueprintViewportInteraction, PickingSkipsSingularAndMalformedBounds)
{
    auto singular = Box("singular");
    singular.worldFromLocal(0, 0) = 0;
    auto malformed = Box("malformed");
    malformed.minimum.x() = 2;
    auto nonFinite = Box("non-finite");
    nonFinite.worldFromLocal(1, 1) = std::numeric_limits<float>::infinity();
    auto projective = Box("projective");
    projective.worldFromLocal(3, 0) = 1;
    const auto hit = PickBlueprintViewportComponent({singular, malformed, nonFinite, projective, Box("valid")},
        Camera(), 800, 600, {400, 300});
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->componentId, "valid");
}

TEST(BlueprintViewportInteraction, AxisHitSelectsVisibleAxisAndLeavesCenterForComponentSelection)
{
    const auto gizmo = Gizmo(BlueprintGizmoMode::Translate);
    ASSERT_TRUE(gizmo.valid);
    EXPECT_TRUE(gizmo.axes[0].visible);
    EXPECT_TRUE(gizmo.axes[1].visible);
    EXPECT_FALSE(gizmo.axes[2].visible);
    const auto hit = HitBlueprintViewportGizmo(gizmo, gizmo.axes[0].end);
    ASSERT_TRUE(hit);
    EXPECT_EQ(*hit, 0);
    EXPECT_FALSE(HitBlueprintViewportGizmo(gizmo, gizmo.originPixel));
    EXPECT_FALSE(HitBlueprintViewportGizmo(gizmo, {700, 500}));
}

TEST(BlueprintViewportInteraction, TranslationConvertsWorldDragToAuthoredParentCoordinates)
{
    const Eigen::Quaternionf rotation(Eigen::AngleAxisf(1.57079632679f, Eigen::Vector3f::UnitZ()));
    Eigen::Matrix4f parent = Eigen::Matrix4f::Identity();
    parent.block<3, 3>(0, 0) = rotation.toRotationMatrix() * Eigen::Vector3f(2, 3, 4).asDiagonal();
    const auto gizmo = Gizmo(BlueprintGizmoMode::Translate, parent, rotation);
    ASSERT_TRUE(gizmo.axes[0].visible);
    const auto& axis = gizmo.axes[0];
    const Eigen::Vector3f start = gizmo.originWorld + axis.worldAxis;
    Eigen::Vector3f value;
    ASSERT_TRUE(DragBlueprintViewportGizmo(gizmo, 0, Pixel(start), Pixel(start + axis.worldAxis * 3), value));
    EXPECT_TRUE(value.isApprox(Eigen::Vector3f(1.5f, 0, 0), 1e-5f));
}

TEST(BlueprintViewportInteraction, TranslationHandlesShearedParentWithoutDecomposition)
{
    Eigen::Matrix4f parent = Eigen::Matrix4f::Identity();
    parent(0, 1) = 2;
    parent(1, 1) = 3;
    const auto gizmo = Gizmo(BlueprintGizmoMode::Translate, parent);
    const auto& axis = gizmo.axes[1];
    ASSERT_TRUE(axis.visible);
    const Eigen::Vector3f start = gizmo.originWorld + axis.worldAxis * 0.5f;
    const Eigen::Vector3f end = start + parent.block<3, 1>(0, 1) * 0.25f;
    Eigen::Vector3f value;
    ASSERT_TRUE(DragBlueprintViewportGizmo(gizmo, 1, Pixel(start), Pixel(end), value));
    EXPECT_TRUE(value.isApprox(Eigen::Vector3f(0, 0.25f, 0), 1e-5f));
}

TEST(BlueprintViewportInteraction, RotationRingUsesSignedPlaneAngleAndKeepsOtherEulerComponents)
{
    const auto gizmo = Gizmo(BlueprintGizmoMode::Rotate);
    ASSERT_TRUE(gizmo.axes[2].visible);
    EXPECT_FALSE(gizmo.axes[0].visible);
    const float radius = gizmo.axes[2].worldLength;
    Eigen::Vector3f value;
    ASSERT_TRUE(DragBlueprintViewportGizmo(gizmo, 2, Pixel({radius, 0, 0}), Pixel({0, radius, 0}), value));
    EXPECT_TRUE(value.isApprox(Eigen::Vector3f(0, 0, 90), 1e-5f));
    const auto hit = HitBlueprintViewportGizmo(gizmo, Pixel({radius, 0, 0}));
    ASSERT_TRUE(hit);
    EXPECT_EQ(*hit, 2);
}

TEST(BlueprintViewportInteraction, RotationDragUnwrapsPastHalfTurnAndPreservesCompleteTurns)
{
    const auto gizmo = Gizmo(BlueprintGizmoMode::Rotate);
    const float radius = gizmo.axes[2].worldLength;
    const float angle = -170 * 0.017453292519943295f;
    const auto current = Pixel({radius * std::cos(angle), radius * std::sin(angle), 0});
    Eigen::Vector3f value;
    ASSERT_TRUE(DragBlueprintViewportGizmo(gizmo, 2, Pixel({radius, 0, 0}), current, value, 179));
    EXPECT_NEAR(value.z(), 190, 1e-4f);
    ASSERT_TRUE(DragBlueprintViewportGizmo(gizmo, 2, Pixel({radius, 0, 0}), current, value, 539));
    EXPECT_NEAR(value.z(), 550, 1e-4f);
}

TEST(BlueprintViewportInteraction, EulerEditAxesAgreeWithNativeRotationDerivative)
{
    auto camera = Camera();
    camera.position = {10, 12, 14};
    camera.forward = -camera.position.normalized();
    camera.right = camera.forward.cross(Eigen::Vector3f::UnitY()).normalized();
    camera.up = camera.right.cross(camera.forward);
    const Eigen::Vector3f degrees(30, 50, 70);
    const auto gizmo = BuildBlueprintViewportGizmo(camera, 800, 600, Eigen::Matrix4f::Identity(),
        Eigen::Quaternionf::Identity(), Eigen::Vector3f::Zero(), degrees, Eigen::Vector3f::Ones(),
        BlueprintGizmoMode::Rotate);
    const nlohmann::json original{{"rotation", {degrees.x(), degrees.y(), degrees.z()}}};
    const auto base = ScriptRotation(original);
    for (int axis = 0; axis < 3; ++axis)
    {
        auto changed = original;
        changed["rotation"][axis] = degrees[axis] + 0.1f;
        const Eigen::Quaternionf difference = ScriptRotation(changed) * base.conjugate();
        EXPECT_TRUE(gizmo.axes[axis].worldAxis.isApprox(difference.vec().normalized(), 1e-3f));
    }
}

TEST(BlueprintViewportInteraction, ScaleDragPreservesNegativeScaleAndUntouchedAxes)
{
    const auto gizmo = Gizmo(BlueprintGizmoMode::Scale, Eigen::Matrix4f::Identity(),
        Eigen::Quaternionf::Identity(), {-2, 3, 4});
    const float radius = gizmo.axes[0].worldLength;
    Eigen::Vector3f value;
    ASSERT_TRUE(DragBlueprintViewportGizmo(gizmo, 0, Pixel({0.5f, 0, 0}),
        Pixel({0.5f + radius * std::log(2.0f), 0, 0}), value));
    EXPECT_TRUE(value.isApprox(Eigen::Vector3f(-4, 3, 4), 1e-5f));
}

TEST(BlueprintViewportInteraction, FailedDragLeavesOutputUntouched)
{
    const auto gizmo = Gizmo(BlueprintGizmoMode::Rotate);
    Eigen::Vector3f value(7, 8, 9);
    EXPECT_FALSE(DragBlueprintViewportGizmo(gizmo, 2, gizmo.originPixel, {400, 200}, value));
    EXPECT_FALSE(DragBlueprintViewportGizmo(gizmo, 3, {450, 300}, {400, 200}, value));
    EXPECT_FALSE(DragBlueprintViewportGizmo(gizmo, 0, {450, 300}, {400, 200}, value));
    EXPECT_EQ(value, Eigen::Vector3f(7, 8, 9));
}

TEST(BlueprintViewportInteraction, InvalidCameraOrParentCannotPublishGizmo)
{
    auto camera = Camera();
    camera.clipNear = 0;
    EXPECT_FALSE(BuildBlueprintViewportGizmo(camera, 800, 600, Eigen::Matrix4f::Identity(),
        Eigen::Quaternionf::Identity(), Eigen::Vector3f::Zero(), Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Ones(), BlueprintGizmoMode::Translate).valid);
    Eigen::Matrix4f singular = Eigen::Matrix4f::Zero();
    EXPECT_FALSE(Gizmo(BlueprintGizmoMode::Translate, singular).valid);
    auto behind = Eigen::Matrix4f(Eigen::Matrix4f::Identity());
    behind(2, 3) = 20;
    EXPECT_FALSE(Gizmo(BlueprintGizmoMode::Translate, behind).valid);
}

TEST(BlueprintViewportInteraction, NonMeshHelpersUseParentTransformsAndOfferStablePicking)
{
    const nlohmann::json draft{{"components", nlohmann::json::array({
        {{"id", "parent"}, {"type", "scene"}, {"position", {2, 0, 0}}},
        {{"id", "camera"}, {"type", "camera"}, {"parent", "parent"}, {"position", {0, 1, 0}}}})}};
    const auto helpers = BuildBlueprintViewportHelpers(draft, Camera(), 800, 600);
    ASSERT_EQ(helpers.size(), 2u);
    EXPECT_FALSE(helpers[1].lines.empty());
    const auto hit = HitBlueprintViewportHelper(helpers, Pixel({2, 1, 0}));
    ASSERT_TRUE(hit);
    EXPECT_EQ(*hit, "camera");
}

TEST(BlueprintViewportInteraction, RootCollisionOutlineCanSelectBlueprintSelf)
{
    const nlohmann::json draft{{"physics", {{"shape", "box"}, {"halfExtents", {1, 1, 1}}}}};
    const auto helpers = BuildBlueprintViewportHelpers(draft, Camera(), 800, 600);
    ASSERT_EQ(helpers.size(), 1u);
    ASSERT_FALSE(helpers[0].lines.empty());
    const auto hit = HitBlueprintViewportHelper(helpers, helpers[0].lines[0].begin);
    ASSERT_TRUE(hit);
    EXPECT_TRUE(hit->empty());
}

TEST(BlueprintViewportInteraction, CompoundHelpersSelectTheirOwningMeshComponent)
{
    const nlohmann::json draft{{"physics", {{"shape", "compound"}}},
        {"components", nlohmann::json::array({{{"id", "body"}, {"type", "mesh"},
            {"position", {2, 0, 0}}, {"collision", {{"shape", "box"}, {"halfExtents", {1, 1, 1}}}}}})}};
    const auto helpers = BuildBlueprintViewportHelpers(draft, Camera(), 800, 600);
    ASSERT_EQ(helpers.size(), 1u);
    EXPECT_EQ(helpers[0].componentId, "body");
    ASSERT_FALSE(helpers[0].lines.empty());
    EXPECT_EQ(HitBlueprintViewportHelper(helpers, helpers[0].lines[0].begin), std::optional<std::string>("body"));
}

TEST(BlueprintViewportInteraction, MeshCollisionDoesNotPretendToBeSphereOrRequireRadius)
{
    const nlohmann::json draft{{"physics", {{"shape", "mesh"}, {"component", "body"}}}};
    EXPECT_NO_THROW(BuildBlueprintViewportHelpers(draft, Camera(), 800, 600));
    EXPECT_TRUE(BuildBlueprintViewportHelpers(draft, Camera(), 800, 600)[0].lines.empty());
}

TEST(BlueprintViewportInteraction, TranslationAndRotationSnappingUseGestureRelativeIncrements)
{
    Eigen::Vector3f result;
    ASSERT_TRUE(SnapBlueprintViewportValue(BlueprintGizmoMode::Translate, 0, {3, 4, 5}, {19, 4, 5}, 10, result));
    EXPECT_EQ(result, Eigen::Vector3f(23, 4, 5));
    ASSERT_TRUE(SnapBlueprintViewportValue(BlueprintGizmoMode::Rotate, 2, {0, 0, 170}, {0, 0, 190}, 15, result));
    EXPECT_EQ(result, Eigen::Vector3f(0, 0, 185));
}

TEST(BlueprintViewportInteraction, ScaleSnappingPreservesMirroringAndRejectsZero)
{
    Eigen::Vector3f result;
    ASSERT_TRUE(SnapBlueprintViewportValue(BlueprintGizmoMode::Scale, 0, {-2, 3, 4}, {-2.36f, 3, 4}, 0.1f, result));
    EXPECT_TRUE(result.isApprox(Eigen::Vector3f(-2.4f, 3, 4)));
    EXPECT_FALSE(SnapBlueprintViewportValue(BlueprintGizmoMode::Scale, 0, {-2, 3, 4}, {-0.02f, 3, 4}, 0.1f, result));
}

struct PreviewWorldFixture
{
    nlohmann::json draft{{"class", "Gameplay.Empty"}, {"components", nlohmann::json::array({
        {{"id", "body"}, {"type", "mesh"}, {"modelPath", "Common/Meshes/SM_test.json"}},
        {{"id", "lamp"}, {"type", "pointLight"}, {"intensityUnit", "cd"}}})}};
    World world{7, "Common/Scenes/SC_model_preview.json"};
    BlueprintPreviewTransformInput input;
    PreviewWorldFixture()
    {
        input.assetPath = "Blueprints/BP_test.json";
        input.documentRevision = 11;
        input.worldGeneration = 7;
        input.componentId = "body";
        input.field = "position";
        input.value = {40, 20, 0};
        const auto actor = BuildBlueprintPreviewObjects(input.assetPath, 11, 11, draft).at(0);
        world.SetScriptBlueprints(nlohmann::json::array({actor}));
        WorldMeshObject mesh;
        mesh.sceneObjectIdentity = ScriptMeshIdentity(actor.at("id"), actor["components"][0]);
        mesh.localBoundsMin = {-1, -1, -1};
        mesh.localBoundsMax = {1, 1, 1};
        world.AddMeshObject("body-section", mesh);
        world.ApplyScriptBlueprintVisuals(actor);
        world.SetCamera(std::make_shared<::Camera>());
    }
};

TEST(BlueprintViewportInteraction, TransientDragMovesRealMeshWithoutChangingWorldOrAuthorDeclaration)
{
    PreviewWorldFixture fixture;
    const auto baseline = fixture.world.GetScriptBlueprints();
    ASSERT_TRUE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    EXPECT_EQ(fixture.world.GetGeneration(), 7u);
    EXPECT_EQ(fixture.world.GetScriptBlueprints(), baseline);
    EXPECT_TRUE((fixture.world.GetMeshObjects().at("body-section").model.block<3, 1>(0, 3).isApprox(
        Eigen::Vector3f(40, 20, 0))));
    WorldSnapshotBuilder builder;
    WorldSnapshotBuildDesc description;
    description.worldGeneration = 7;
    const auto snapshot = builder.Build(fixture.world, description);
    ASSERT_TRUE(snapshot.IsSuccess());
    ASSERT_EQ(snapshot.Value().meshDraws.size(), 1u);
    EXPECT_TRUE((snapshot.Value().meshDraws[0].model.block<3, 1>(0, 3).isApprox(
        Eigen::Vector3f(40, 20, 0))));
    fixture.input.reset = true;
    ASSERT_TRUE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    EXPECT_TRUE(fixture.world.GetMeshObjects().at("body-section").model.isIdentity());
}

TEST(BlueprintViewportInteraction, RootTransientTransformMovesAllGeometryAndLights)
{
    PreviewWorldFixture fixture;
    fixture.input.componentId.clear();
    fixture.input.field = "scale";
    fixture.input.value = {2, 3, 4};
    ASSERT_TRUE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    const auto& model = fixture.world.GetMeshObjects().at("body-section").model;
    EXPECT_FLOAT_EQ(model(0, 0), 2);
    EXPECT_FLOAT_EQ(model(1, 1), 3);
    fixture.input.componentId = "lamp";
    fixture.input.field = "position";
    fixture.input.value = {50, 25, 0};
    ASSERT_TRUE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    ASSERT_EQ(fixture.world.GetScriptLights().size(), 1u);
    EXPECT_TRUE(fixture.world.GetScriptLights().begin()->second.position.isApprox(Eigen::Vector3f(50, 25, 0)));
}

TEST(BlueprintViewportInteraction, InvalidOrStaleTransientRequestsCannotMovePreviewGeometry)
{
    PreviewWorldFixture fixture;
    fixture.input.worldGeneration = 8;
    EXPECT_FALSE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    fixture.input.worldGeneration = 7;
    EXPECT_FALSE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 12, fixture.draft));
    fixture.input.componentId.clear();
    EXPECT_FALSE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    fixture.input.componentId = "body";
    fixture.input.field = "scale";
    fixture.input.value = {0, 1, 1};
    EXPECT_FALSE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    EXPECT_TRUE(fixture.world.GetMeshObjects().at("body-section").model.isIdentity());
}
} // namespace
