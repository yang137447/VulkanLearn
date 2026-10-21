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

nlohmann::json SpringArmDraft()
{
    return {{"components", nlohmann::json::array({
        {{"id", "arm"}, {"type", "springArm"}, {"rotation", {-12, 0, 0}},
            {"targetArmLength", 450}, {"usePawnControlRotation", true}},
        {{"id", "camera"}, {"type", "camera"}, {"parent", "arm"}}})}};
}

EditorSceneCameraActor SpringArmView()
{
    auto camera = Camera();
    camera.position = {900, 180, 900};
    camera.forward = (Eigen::Vector3f(0, 0, 200) - camera.position).normalized();
    camera.right = camera.forward.cross(Eigen::Vector3f::UnitY()).normalized();
    camera.up = camera.right.cross(camera.forward);
    camera.clipFar = 10000;
    return camera;
}

TEST(BlueprintViewportInteraction, SpringArmCameraHelperAndPickingUsePitchedRearEndpoint)
{
    const auto draft = SpringArmDraft();
    const auto baseline = draft;
    const auto view = SpringArmView();
    const Eigen::Vector3f endpoint(0, 450 * std::sin(12 * 0.01745329252f),
        450 * std::cos(12 * 0.01745329252f));
    const auto helpers = BuildBlueprintViewportHelpers(draft, view, 800, 600);
    ASSERT_EQ(helpers.size(), 2u);
    ASSERT_TRUE(helpers[1].markerVisible);
    Eigen::Vector2f pixel;
    ASSERT_TRUE(ProjectBlueprintViewportPoint(view, 800, 600, endpoint, pixel));
    EXPECT_TRUE(helpers[1].marker.isApprox(pixel));
    EXPECT_EQ(HitBlueprintViewportHelper(helpers, pixel), std::optional<std::string>("camera"));
    ASSERT_EQ(helpers[0].lines.size(), 5u);
    EXPECT_TRUE(helpers[0].lines[0].end.isApprox(pixel));
    EditorSceneActorSnapshot expected;
    expected.type = EditorSceneActorType::Camera;
    expected.camera.position = endpoint;
    expected.camera.forward = Eigen::Vector3f(0, -std::sin(12 * 0.01745329252f),
        -std::cos(12 * 0.01745329252f));
    expected.camera.up = Eigen::Vector3f(0, std::cos(12 * 0.01745329252f),
        -std::sin(12 * 0.01745329252f));
    const auto frustum = BuildActorVisualLines(expected, view, 800, 600);
    ASSERT_GE(helpers[1].lines.size(), frustum.size());
    for (size_t index = 0; index < frustum.size(); ++index)
    {
        EXPECT_TRUE(helpers[1].lines[index].begin.isApprox(frustum[index].begin));
        EXPECT_TRUE(helpers[1].lines[index].end.isApprox(frustum[index].end));
    }
    EXPECT_EQ(draft, baseline);
}

TEST(BlueprintViewportInteraction, SpringArmPreviewScalesMountButKeepsWorldAndSocketOffsetsInCentimeters)
{
    auto draft = SpringArmDraft();
    draft["position"] = {10, 20, 30};
    draft["rotation"] = {0, 90, 0};
    draft["scale"] = {2, 3, 4};
    auto& arm = draft["components"][0];
    arm["position"] = {1, 2, 3};
    arm["rotation"] = {0, 0, 0};
    arm["scale"] = {7, 8, 9};
    arm["targetOffset"] = {3, 4, 5};
    arm["socketOffset"] = {6, 7, 8};
    arm["enableCameraLag"] = true;
    arm["enableCameraRotationLag"] = true;
    const auto preview = EvaluateScriptComponentSpace(draft, 0);
    EXPECT_TRUE((preview.worldFromComponent.block<3, 1>(0, 3) + ScriptVector(arm, "targetOffset")).isApprox(
        Eigen::Vector3f(25, 30, 33), 1e-5f));
    EXPECT_TRUE((preview.worldFromAttachment.block<3, 1>(0, 3).isApprox(Eigen::Vector3f(483, 37, 27), 1e-5f)));
}

TEST(BlueprintViewportInteraction, SpringArmPreviewHonorsDisabledYawInheritanceAndRotationOffset)
{
    auto draft = SpringArmDraft();
    draft["rotation"] = {0, 90, 0};
    auto& arm = draft["components"][0];
    arm["rotation"] = {0, 10, 0};
    arm["inheritYaw"] = false;
    arm["rotationOffset"] = {0, 20, 0};
    const auto preview = EvaluateScriptComponentSpace(draft, 0);
    EXPECT_TRUE((preview.worldFromAttachment.block<3, 1>(0, 3).isApprox(
        Eigen::Vector3f(225, 0, 450 * std::cos(30 * 0.01745329252f)), 1e-5f)));
}

TEST(BlueprintViewportInteraction, SpringArmPreviewAbsoluteRotationUsesAuthorLocalAxes)
{
    auto draft = SpringArmDraft();
    draft["rotation"] = {20, 90, 30};
    draft["components"][0]["absoluteRotation"] = true;
    draft["components"][0]["inheritPitch"] = false;
    const auto preview = EvaluateScriptComponentSpace(draft, 0);
    EXPECT_TRUE((preview.attachmentRotation * Eigen::Vector3f::UnitZ()).isApprox(
        Eigen::Vector3f(0, std::sin(12 * 0.01745329252f), std::cos(12 * 0.01745329252f)), 1e-5f));
}

TEST(BlueprintViewportInteraction, NestedArmAndSceneSpacesPreserveChildLocalPositionAndRotation)
{
    auto draft = SpringArmDraft();
    draft["components"][0]["rotation"] = {0, 0, 0};
    draft["components"].insert(draft["components"].begin() + 1,
        nlohmann::json{{"id", "inner"}, {"type", "springArm"}, {"parent", "arm"},
            {"position", {50, 0, 0}}, {"targetArmLength", 100}, {"absoluteRotation", true}});
    draft["components"].insert(draft["components"].begin() + 2,
        nlohmann::json{{"id", "pivot"}, {"type", "scene"}, {"parent", "inner"},
            {"position", {100, 20, 0}}, {"rotation", {0, 30, 0}}});
    auto& camera = draft["components"][3];
    camera["parent"] = "pivot";
    camera["position"] = {9, 8, 7};
    camera["rotation"] = {0, 0, 15};
    const auto space = EvaluateScriptComponentSpace(draft, 3);
    const Eigen::Vector3f expected = Eigen::Vector3f(150, 20, 550) +
        ScriptRotation({{"rotation", {0, 30, 0}}}) * Eigen::Vector3f(9, 8, 7);
    EXPECT_TRUE((space.worldFromComponent.block<3, 1>(0, 3).isApprox(expected)));
    EXPECT_TRUE(space.rotation.isApprox(ScriptRotation({{"rotation", {0, 30, 15}}}), 1e-5f));
}

TEST(BlueprintViewportInteraction, NonCameraSpringArmChildrenUseTheSameSocketSpace)
{
    auto draft = SpringArmDraft();
    draft["components"][1]["type"] = "scene";
    draft["components"][1]["position"] = {10, 0, 0};
    const auto space = EvaluateScriptComponentSpace(draft, 1);
    const Eigen::Vector3f expected(10, 450 * std::sin(12 * 0.01745329252f),
        450 * std::cos(12 * 0.01745329252f));
    EXPECT_TRUE((space.worldFromComponent.block<3, 1>(0, 3).isApprox(expected)));
    EXPECT_TRUE(space.rotation.isApprox(ScriptComponentRotation(draft, 0)));
}

TEST(BlueprintViewportInteraction, SpringArmCameraGizmoEditsChildLocalSpaceAtSocket)
{
    auto draft = SpringArmDraft();
    draft["components"][0]["rotationOffset"] = {0, 30, 0};
    draft["components"][1]["position"] = {12, 34, 56};
    const auto view = SpringArmView();
    const auto gizmo = BuildBlueprintComponentGizmo(draft, 1, view, 800, 600, BlueprintGizmoMode::Rotate);
    ASSERT_TRUE(gizmo.valid);
    EXPECT_TRUE(gizmo.originWorld.isApprox(ScriptComponentMatrix(draft, 1).block<3, 1>(0, 3)));
    const Eigen::Vector3f y = EvaluateScriptComponentSpace(draft, 0).attachmentRotation * Eigen::Vector3f::UnitY();
    EXPECT_TRUE(gizmo.axes[1].worldAxis.isApprox(y, 1e-5f));
    const auto helpers = BuildBlueprintViewportHelpers(draft, view, 800, 600);
    EXPECT_TRUE(gizmo.originPixel.isApprox(helpers[1].marker));
    const auto move = BuildBlueprintComponentGizmo(draft, 1, view, 800, 600, BlueprintGizmoMode::Translate);
    ASSERT_TRUE(move.valid);
    EXPECT_TRUE(move.axes[1].worldAxis.isApprox(y, 1e-5f));
}

TEST(BlueprintViewportInteraction, ReparentingUsesNewAttachmentSpaceAndKeepsChildLocalTRS)
{
    auto draft = SpringArmDraft();
    draft["components"].push_back({{"id", "mesh"}, {"type", "mesh"}, {"parent", "camera"},
        {"position", {12, 34, 56}}, {"rotation", {20, 30, 40}}, {"scale", {-2, 3, 4}}});
    auto& mesh = draft["components"][2];
    const auto local = ScriptTransformMatrix(mesh);
    const auto camera = EvaluateScriptComponentSpace(draft, 1);
    EXPECT_TRUE(ScriptComponentMatrix(draft, 2).isApprox(camera.worldFromAttachment * local));
    mesh["parent"] = "arm";
    const auto arm = EvaluateScriptComponentSpace(draft, 0);
    EXPECT_TRUE(ScriptComponentMatrix(draft, 2).isApprox(arm.worldFromAttachment * local));
    mesh["parent"] = "";
    EXPECT_TRUE(ScriptComponentMatrix(draft, 2).isApprox(local));
    EXPECT_TRUE(ScriptTransformMatrix(mesh).isApprox(local));
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

TEST(BlueprintViewportInteraction, MeshSectionLocalSpaceRemainsBelowComponentSpaceDuringParentDrag)
{
    PreviewWorldFixture fixture;
    fixture.draft["scale"] = {-2, 3, 4};
    fixture.draft["components"].push_back({{"id", "pivot"}, {"type", "scene"},
        {"rotation", {15, 35, 20}}, {"position", {20, 30, 40}}});
    fixture.draft["components"][0]["parent"] = "pivot";
    fixture.draft["components"][0]["rotation"] = {25, 10, 5};
    fixture.draft["components"][0]["position"] = {8, 9, 10};
    const auto actor = BuildBlueprintPreviewObjects(fixture.input.assetPath, 11, 11, fixture.draft).at(0);
    fixture.world.SetScriptBlueprints(nlohmann::json::array({actor}));
    const Eigen::Matrix4f importedSection = ScriptTransformMatrix(
        {{"position", {2, 5, 7}}, {"rotation", {5, 10, 15}}, {"scale", {1, 2, 3}}});
    WorldMeshObject section = fixture.world.GetMeshObjects().at("body-section");
    section.model = importedSection;
    fixture.world.AddMeshObject("body-section", section);
    fixture.world.ApplyScriptBlueprintVisuals(actor);
    fixture.input.componentId = "pivot";
    fixture.input.value = {100, 200, 300};
    ASSERT_TRUE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    auto candidate = actor;
    candidate["components"][2]["position"] = fixture.input.value;
    const Eigen::Matrix4f expected = ScriptComponentMatrix(candidate, 0) * importedSection;
    EXPECT_TRUE(fixture.world.GetMeshObjects().at("body-section").model.isApprox(expected, 1e-4f));
    WorldSnapshotBuilder builder;
    WorldSnapshotBuildDesc description;
    const auto snapshot = builder.Build(fixture.world, description);
    ASSERT_TRUE(snapshot.IsSuccess());
    ASSERT_EQ(snapshot.Value().meshDraws.size(), 1u);
    EXPECT_TRUE(snapshot.Value().meshDraws[0].model.isApprox(expected, 1e-4f));
    fixture.input.reset = true;
    ASSERT_TRUE(ApplyBlueprintPreviewTransform(fixture.world, fixture.input, 11, fixture.draft));
    EXPECT_TRUE(fixture.world.GetMeshObjects().at("body-section").model.isApprox(
        ScriptComponentMatrix(actor, 0) * importedSection, 1e-4f));
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
