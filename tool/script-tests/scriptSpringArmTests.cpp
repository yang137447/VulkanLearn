#include <gtest/gtest.h>
#include "script/scriptRuntime.h"
#include "script/scriptComponents.h"
#include "world/world.h"
#include "sceneNode.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
using namespace VL;
using Json = nlohmann::json;

class ScriptSpringArmTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "spring-arm");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }

    Json CameraNode(bool autoActivate = true, std::string parent = "arm")
    {
        Json camera{{"id", "camera"}, {"type", "camera"}, {"parent", std::move(parent)},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"fov", 60}, {"near_clip", 1}, {"far_clip", 10000}};
        if (autoActivate) camera["autoActivate"] = true;
        return camera;
    }

    Json Owner(float armLength = 300, bool collision = true, Json socket = Json::array({0, 0, 0}),
        Json armRotation = Json::array({0, 0, 0}))
    {
        return {{"type", "blueprintInstance"}, {"id", "owner"}, {"name", "owner"},
            {"class", "ScriptFixtures.PassiveBlueprint"}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"properties", Json::object()},
            // 相机 owner 故意不带刚体：相机刷新走 Apply 的状态路径，探测查询走物理世界。
            {"components", Json::array({
                {{"id", "arm"}, {"type", "springArm"}, {"parent", ""},
                    {"position", {0, 0, 0}}, {"rotation", armRotation}, {"scale", {1, 1, 1}},
                    {"targetArmLength", armLength}, {"socketOffset", socket}, {"probeRadius", 12},
                    {"doCollisionTest", collision}, {"collisionMask", 65535}},
                CameraNode()})}};
    }

    Json Wall(float z, Json halfExtents = Json::array({100, 100, 10}))
    {
        return {{"type", "blueprintInstance"}, {"id", "wall"}, {"name", "wall"},
            {"class", "ScriptFixtures.PassiveBlueprint"}, {"position", {0, 0, z}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"properties", Json::object()},
            {"physics", {{"motion", "static"}, {"shape", "box"}, {"halfExtents", halfExtents}}},
            {"components", Json::array()}};
    }

    void Prepare(const Json& objects)
    {
        if (!runtime.IsEditing())
        {
            std::string error;
            ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error)) << error;
        }
        runtime.PrepareWorld(*world, {{"objects", objects}});
        runtime.SynchronizeWorld(world);
    }
    void Play()
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    void Tick(float dt = 1.0f / 60)
    {
        runtime.Tick(world, dt, {}, diagnostics);
        ASSERT_FALSE(runtime.IsEditing()) << runtime.View(world->GetGeneration()).feedback;
    }
    Eigen::Vector3f CameraPosition() const { return world->GetRenderCamera()->GetPosition(); }
    Eigen::Vector3f CameraRotation() const { return world->GetRenderCamera()->GetRotation(); }

    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptSpringArmTest, PlacesChildCameraAtTargetArmLengthInArmSpace)
{
    Prepare(Json::array({Owner()}));
    Play();
    Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 0, 300), 0.01f));
    // 再次 Tick 保持同一落点，说明无碰撞时不会累积偏移或抖动。
    Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 0, 300), 0.01f));
}

TEST_F(ScriptSpringArmTest, SocketOffsetAndArmRotationComposeIntoCameraWorldPosition)
{
    Prepare(Json::array({Owner(300, true, Json::array({0, 50, 0}), Json::array({0, 90, 0}))}));
    Play();
    Tick();
    // arm 绕 Y 旋转 90°，局部 +Z 映射到世界 +X；SocketOffset 的 +Y 不受 yaw 影响。
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(300, 50, 0), 0.05f));
}

TEST_F(ScriptSpringArmTest, OwnerTransformComposesIntoCameraWorldPosition)
{
    auto owner = Owner();
    owner["position"] = {100, 20, 0};
    owner["rotation"] = {0, 90, 0};
    Prepare(Json::array({owner}));
    Play();
    Tick();
    // owner 的 yaw 90° 把 arm 局部 +Z 映射到世界 +X；相机落点必须叠加上 owner 平移。
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(400, 20, 0), 0.05f));
}

TEST_F(ScriptSpringArmTest, CameraInheritsArmYawUnlessDisabled)
{
    auto owner = Owner();
    owner["position"] = {100, 20, 0};
    owner["rotation"] = {0, 90, 0};
    Prepare(Json::array({owner}));
    Play();
    Tick();
    EXPECT_NEAR(CameraRotation().y(), 90.0f, 0.5f);

    owner = Owner();
    owner["position"] = {100, 20, 0};
    owner["rotation"] = {0, 90, 0};
    owner["components"][0]["inheritYaw"] = false;
    Prepare(Json::array({owner}));
    Play();
    Tick();
    // 关闭 yaw 继承后 target 的父贡献为 0；相机局部旋转由 inverse(实际 arm 旋转) 补偿，
    // 相机世界 yaw 仍为 0，而不是把 arm 的 90° 原样带出去。
    // 原生 Euler 在零角附近可以返回等价的 (180,180,180)，直接核对镜头基向量。
    EXPECT_TRUE(world->GetRenderCamera()->GetForwardVector().isApprox(Eigen::Vector3f(0, 0, -1), 0.001f));
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(100, 20, 300), 0.001f));
}

TEST_F(ScriptSpringArmTest, RotationOffsetAndAbsoluteRotationAreApplied)
{
    auto owner = Owner();
    owner["components"][0]["rotationOffset"] = {0, 45, 0};
    Prepare(Json::array({owner}));
    Play();
    Tick();
    EXPECT_NEAR(CameraRotation().y(), 45.0f, 0.5f);

    owner = Owner();
    owner["rotation"] = {0, 90, 0};
    owner["components"][0]["absoluteRotation"] = true;
    owner["components"][1]["rotation"] = {0, 30, 0};
    Prepare(Json::array({owner}));
    Play();
    Tick();
    // absolute 使用作者相机局部旋转（30°），不叠 owner/arm 的 90°。
    EXPECT_NEAR(CameraRotation().y(), 30.0f, 0.5f);
}

TEST_F(ScriptSpringArmTest, UsePawnControlRotationReadsBlueprintControlRotation)
{
    auto owner = Owner();
    owner["class"] = "ScriptFixtures.SpringArmControlBlueprint";
    owner["components"][0]["usePawnControlRotation"] = true;
    Prepare(Json::array({owner}));
    Play();
    Tick();
    EXPECT_NEAR(CameraRotation().y(), 60.0f, 0.5f);
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(259.8076f, 0, 150), 0.001f));
}

TEST_F(ScriptSpringArmTest, NestedCameraUnderSceneChildUsesArmTarget)
{
    auto owner = Owner();
    owner["components"] = Json::array({
        {{"id", "arm"}, {"type", "springArm"}, {"parent", ""}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"targetArmLength", 300},
            {"socketOffset", {0, 0, 0}}, {"probeRadius", 12}, {"doCollisionTest", false}},
        {{"id", "socket"}, {"type", "scene"}, {"parent", "arm"}, {"position", {0, 0, 10}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
        CameraNode(true, "socket")});
    Prepare(Json::array({owner}));
    Play();
    Tick();
    // Scene 子组件在臂端空间偏移 10 cm；相机继承该偏移，不再被强行拉回同一端点。
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 0, 310), 0.05f));
}

TEST_F(ScriptSpringArmTest, ChildCameraKeepsAuthoredLocalTransformAcrossArmUpdates)
{
    auto owner = Owner(300, false, Json::array({0, 0, 0}), Json::array({0, 90, 0}));
    owner["components"][1]["position"] = {10, 20, 30};
    owner["components"][1]["rotation"] = {15, 20, 5};
    Prepare(Json::array({owner}));
    Play(); Tick(); Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(330, 20, -10), 0.001f));
    const auto& camera = world->GetScriptBlueprintStates()[0]["components"][1];
    EXPECT_EQ(camera["position"], Json::array({10, 20, 30}));
    EXPECT_EQ(camera["rotation"], Json::array({15, 20, 5}));
}

TEST_F(ScriptSpringArmTest, CollisionMovesSocketAndPreservesCameraRelativeOffset)
{
    auto owner = Owner();
    owner["components"][1]["position"] = {10, 0, 5};
    Prepare(Json::array({owner, Wall(150)}));
    Play(); Tick();
    const auto& nodes = world->GetScriptBlueprintStates()[0]["components"];
    EXPECT_NEAR(nodes[0]["__springArmSocket"]["position"][2].get<float>(), 128, 0.1f);
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(10, 0, 133), 0.001f));
    EXPECT_EQ(nodes[1]["position"], Json::array({10, 0, 5}));
}

TEST_F(ScriptSpringArmTest, SocketSpaceMovesMeshAndLightChildrenWithTheirLocalOffsets)
{
    auto owner = Owner(300, false);
    owner["components"].push_back({{"id", "mesh"}, {"type", "mesh"}, {"parent", "arm"},
        {"position", {10, 20, 30}}, {"modelPath", "Common/Meshes/SM_test.json"}});
    owner["components"].push_back({{"id", "light"}, {"type", "pointLight"}, {"parent", "arm"},
        {"position", {-10, 20, 30}}, {"intensityUnit", "cd"}});
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = "__blueprint/owner/mesh";
    // 导入 section 的 Mesh local 偏移应在组件空间中再乘一次，不能丢失或重复应用。
    mesh.model(0, 3) = 5;
    world->AddMeshObject("socket-mesh", mesh);
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_TRUE((world->GetMeshObjects().at("socket-mesh").model.block<3, 1>(0, 3).isApprox(
        Eigen::Vector3f(15, 20, 330), 0.001f)));
    EXPECT_TRUE(world->GetScriptLights().at("__blueprint/owner/light").position.isApprox(
        Eigen::Vector3f(-10, 20, 330), 0.001f));
}

TEST_F(ScriptSpringArmTest, SocketLocalOffsetInheritsScaleWithoutStretchingArmLength)
{
    auto owner = Owner(300, false);
    owner["scale"] = {2, 3, 4};
    owner["components"][1]["position"] = {10, 20, 30};
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(20, 60, 420), 0.001f));
}

TEST_F(ScriptSpringArmTest, ManagedAndNativeSocketMatricesAgreeWithMirroringAndShearedParentChain)
{
    auto owner = Owner(300, false, Json::array({7, 11, 13}), Json::array({15, 25, 35}));
    owner["class"] = "ScriptFixtures.SpringArmSpaceProbeBlueprint";
    owner["position"] = {10, 20, 30};
    owner["rotation"] = {20, 30, 40};
    owner["scale"] = {-2, 3, 4};
    owner["components"][0]["parent"] = "pivot";
    owner["components"][0]["rotationOffset"] = {5, 10, 15};
    owner["components"][1]["position"] = {12, 34, 56};
    owner["components"][1]["rotation"] = {15, 20, 25};
    owner["components"].push_back({{"id", "pivot"}, {"type", "scene"}, {"rotation", {10, 15, 20}},
        {"scale", {3, 2, 1}}, {"position", {30, 20, 10}}});
    owner["components"].push_back({{"id", "probe"}, {"type", "scene"}});
    const Eigen::Vector3f expected = (ScriptComponentMatrix(owner, 1) * Eigen::Vector4f(3, 5, 7, 1)).head<3>();
    Prepare(Json::array({owner}));
    Play(); Tick();
    const auto& probe = world->GetScriptBlueprintStates()[0]["components"][3];
    EXPECT_TRUE(ScriptVector(probe, "position").isApprox(expected, 1e-4f));
    EXPECT_TRUE(CameraPosition().isApprox(ScriptComponentMatrix(owner, 1).block<3, 1>(0, 3), 1e-4f));
}

TEST_F(ScriptSpringArmTest, NestedCollisionArmsResolveParentBeforeChildRegardlessOfDeclarationOrder)
{
    auto owner = Owner();
    owner["components"][1]["parent"] = "inner";
    owner["components"].insert(owner["components"].begin(),
        Json{{"id", "inner"}, {"type", "springArm"}, {"parent", "arm"},
            {"targetArmLength", 100}, {"doCollisionTest", true}, {"probeRadius", 12}});
    Prepare(Json::array({owner, Wall(150)}));
    Play(); Tick();
    // 两条臂使用同一面墙；子臂从父臂本帧球心 128 开始，不能从未裁剪的 300 开始。
    EXPECT_NEAR(CameraPosition().z(), 128, 0.2f);
    const auto& nodes = world->GetScriptBlueprintStates()[0]["components"];
    EXPECT_NEAR(nodes[0]["__springArmSocket"]["position"][2].get<float>(), 128, 0.2f);
    EXPECT_NEAR(nodes[1]["__springArmSocket"]["position"][2].get<float>(), 128, 0.2f);
}

TEST_F(ScriptSpringArmTest, CameraLagSmoothsPositionAcrossFrames)
{
    auto owner = Owner();
    owner["class"] = "ScriptFixtures.SpringArmMoverBlueprint";
    auto& arm = owner["components"][0];
    arm["doCollisionTest"] = false;
    arm["enableCameraLag"] = true;
    arm["cameraLagSpeed"] = 10;
    Prepare(Json::array({owner}));
    Play();
    Tick();
    const float first = CameraPosition().z();
    // 第一帧 owner 前移 200/60≈3.33 cm；VInterpTo 的系数为 1/6。
    EXPECT_GT(first, 300.05f);
    EXPECT_LT(first, 302.5f);
    for (int frame = 0; frame < 60; ++frame) Tick();
    // 稳态滞后≈速度/滞后速度=200/10=20 cm；owner 到 z≈203.3、臂目标≈503.3，
    // 相机应在 483 左右，而不是贴到 503。
    EXPECT_GT(CameraPosition().z(), 480.0f);
    EXPECT_LT(CameraPosition().z(), 495.0f);
}

TEST_F(ScriptSpringArmTest, HigherPriorityAutoActivateCameraWins)
{
    auto owner = Owner();
    owner["components"] = Json::array({
        {{"id", "arm"}, {"type", "springArm"}, {"parent", ""}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"targetArmLength", 300},
            {"socketOffset", {0, 0, 0}}, {"probeRadius", 12}, {"doCollisionTest", false}},
        {{"id", "cameraA"}, {"type", "camera"}, {"parent", "arm"}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60}, {"near_clip", 1},
            {"far_clip", 10000}, {"autoActivate", true}, {"priority", 1}},
        {{"id", "cameraB"}, {"type", "camera"}, {"parent", "arm"}, {"position", {0, 0, 0}},
            {"rotation", {0, 45, 0}}, {"scale", {1, 1, 1}}, {"fov", 60}, {"near_clip", 1},
            {"far_clip", 10000}, {"autoActivate", true}, {"priority", 5}}});
    Prepare(Json::array({owner}));
    Play();
    Tick();
    // 两个相机位置相同，用朝向区分谁被选中；高优先级 cameraB 的局部 yaw=45。
    EXPECT_NEAR(CameraRotation().y(), 45.0f, 0.5f);
}

TEST_F(ScriptSpringArmTest, SphereSweepShortensArmBeforeWallAndKeepsCameraInFront)
{
    Prepare(Json::array({Owner(), Wall(150)}));
    Play();
    Tick();
    const auto position = CameraPosition();
    // 墙面 z=150、半厚 10（前表面 z=140）；半径 12 的球扫在中心 z=128 处接触，
    // Jolt 返回接触时的球心 z≈128，不再重复减一次半径。
    EXPECT_NEAR(position.z(), 128.0f, 0.1f);
    EXPECT_LT(position.z(), 140.0f);
    EXPECT_NEAR(position.x(), 0, 0.05f);
    EXPECT_NEAR(position.y(), 0, 0.05f);
}

TEST_F(ScriptSpringArmTest, DisabledCollisionKeepsFullArmLength)
{
    Prepare(Json::array({Owner(300, false), Wall(150)}));
    Play();
    Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 0, 300), 0.01f));
}

TEST_F(ScriptSpringArmTest, ChildCameraWithoutSpringArmKeepsAuthoredLocalPosition)
{
    auto owner = Owner();
    owner["components"] = Json::array({
        {{"id", "arm"}, {"type", "scene"}, {"parent", ""}, {"position", {0, 0, 40}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
        CameraNode(true, "arm")});
    Prepare(Json::array({owner}));
    Play();
    Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 0, 40), 0.01f));
}

TEST_F(ScriptSpringArmTest, SpringArmSchemaRejectsNegativeLengthAndOutOfRangeMask)
{
    auto owner = Owner();
    auto arm = owner["components"][0];
    arm["targetArmLength"] = -1;
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
    arm = owner["components"][0];
    arm["probeRadius"] = -1;
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
    arm = owner["components"][0];
    arm["collisionMask"] = 70000;
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
    arm = owner["components"][0];
    arm["doCollisionTest"] = 1;
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
}

TEST_F(ScriptSpringArmTest, TargetOffsetUsesWorldSpaceAndSocketOffsetUsesTargetRotation)
{
    auto owner = Owner(300, false, Json::array({10, 0, 0}));
    owner["rotation"] = {0, 90, 0};
    owner["components"][0]["targetOffset"] = {0, 0, 40};
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(300, 0, 30), 0.001f));
}

TEST_F(ScriptSpringArmTest, InheritDisabledKeepsArmLocalYawForPositionAndRotation)
{
    auto owner = Owner(300, false, Json::array({0, 0, 0}), Json::array({0, 30, 0}));
    owner["rotation"] = {0, 90, 0};
    owner["components"][0]["inheritYaw"] = false;
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_NEAR(CameraRotation().y(), 30, 0.01f);
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(150, 0, 259.8076f), 0.001f));
}

TEST_F(ScriptSpringArmTest, AbsoluteRotationPreservesArmLocalRotation)
{
    auto owner = Owner(300, false, Json::array({0, 0, 0}), Json::array({0, 30, 0}));
    owner["rotation"] = {0, 90, 0};
    owner["components"][0]["absoluteRotation"] = true;
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_NEAR(CameraRotation().y(), 30, 0.01f);
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(150, 0, 259.8076f), 0.001f));
}

TEST_F(ScriptSpringArmTest, MixedPitchYawRollRoundTripsThroughCamera)
{
    auto owner = Owner(300, false, Json::array({0, 0, 0}), Json::array({25, 40, 15}));
    Prepare(Json::array({owner}));
    Play(); Tick();
    const Eigen::Matrix3f expected = (Eigen::AngleAxisf(40 * 3.14159265f / 180, Eigen::Vector3f::UnitY()) *
        Eigen::AngleAxisf(25 * 3.14159265f / 180, Eigen::Vector3f::UnitX()) *
        Eigen::AngleAxisf(15 * 3.14159265f / 180, Eigen::Vector3f::UnitZ())).toRotationMatrix();
    EXPECT_TRUE(CameraPosition().isApprox(expected * Eigen::Vector3f(0, 0, 300), 0.001f));
    EXPECT_TRUE(world->GetRenderCamera()->GetForwardVector().isApprox(-expected.col(2), 0.001f));
    EXPECT_TRUE(world->GetRenderCamera()->GetUpVector().isApprox(expected.col(1), 0.001f));
}

TEST_F(ScriptSpringArmTest, ParentScaleDoesNotStretchArmLength)
{
    auto owner = Owner(300, false);
    owner["scale"] = {2, 3, 4};
    owner["components"][0]["position"] = {0, 10, 0};
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 30, 300), 0.001f));
}

TEST_F(ScriptSpringArmTest, CameraFollowsSameFrameLateTickMovement)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmLateMoverBlueprint";
    Prepare(Json::array({owner}));
    Play(); Tick(0.05f);
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(10, 0, 300), 0.001f));
}

TEST_F(ScriptSpringArmTest, SweepUsesSameFrameRigidBodyPosition)
{
    auto owner = Owner();
    owner["class"] = "ScriptFixtures.SpringArmBodyMoverBlueprint";
    owner["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 5}};
    // 只覆盖移动后的臂；旧 x=0 的查询不会命中。
    auto wall = Wall(150, Json::array({3, 100, 10}));
    wall["position"] = {40, 0, 150};
    owner["components"][0]["probeRadius"] = 1;
    Prepare(Json::array({owner, wall}));
    Play(); Tick(0.2f);
    const auto& state = world->GetScriptBlueprintStates()[0];
    EXPECT_NEAR(CameraPosition().x(), state["position"][0].get<float>(), 0.01f);
    EXPECT_GT(CameraPosition().x(), 35);
    EXPECT_NEAR(CameraPosition().z(), 139, 0.1f);
}

TEST_F(ScriptSpringArmTest, MovingObstacleInvalidatesStationaryArmCollision)
{
    auto wall = Wall(150);
    wall["class"] = "ScriptFixtures.SpringArmMovingWallBlueprint";
    Prepare(Json::array({Owner(), wall}));
    Play(); Tick();
    EXPECT_NEAR(CameraPosition().z(), 128, 0.1f);
    Tick();
    EXPECT_NEAR(CameraPosition().z(), 300, 0.1f);
}

TEST_F(ScriptSpringArmTest, LagCannotSmoothCameraThroughBlockingWall)
{
    auto owner = Owner();
    owner["class"] = "ScriptFixtures.SpringArmMoverBlueprint";
    owner["components"][0]["enableCameraLag"] = true;
    owner["components"][0]["cameraLagSpeed"] = 1;
    Prepare(Json::array({owner, Wall(150)}));
    Play(); Tick();
    EXPECT_NEAR(CameraPosition().z(), 128, 0.1f);
    for (int frame = 0; frame < 10; ++frame) Tick();
    EXPECT_NEAR(CameraPosition().z(), 128, 0.1f);
}

TEST_F(ScriptSpringArmTest, ZeroLagSpeedSnapsToTarget)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmMoverBlueprint";
    owner["components"][0]["enableCameraLag"] = true;
    owner["components"][0]["cameraLagSpeed"] = 0;
    Prepare(Json::array({owner}));
    Play(); Tick(0.05f);
    EXPECT_NEAR(CameraPosition().z(), 310, 0.01f);
}

TEST_F(ScriptSpringArmTest, CameraLagMaxDistanceClampsPivotLag)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmMoverBlueprint";
    owner["components"][0]["enableCameraLag"] = true;
    owner["components"][0]["cameraLagSpeed"] = 0.1f;
    owner["components"][0]["cameraLagMaxDistance"] = 5;
    Prepare(Json::array({owner}));
    Play(); Tick(0.1f);
    EXPECT_NEAR(CameraPosition().z(), 315, 0.01f);
}

TEST_F(ScriptSpringArmTest, RotationLagDrivesBothBoomPositionAndCameraOrientation)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmRotatorBlueprint";
    owner["components"][0]["usePawnControlRotation"] = true;
    owner["components"][0]["enableCameraRotationLag"] = true;
    owner["components"][0]["cameraRotationLagSpeed"] = 10;
    owner["components"][0]["useCameraLagSubstepping"] = false;
    Prepare(Json::array({owner}));
    Play(); Tick(0.05f);
    EXPECT_NEAR(CameraRotation().y(), 2.25f, 0.01f);
    EXPECT_NEAR(CameraPosition().x(), 300 * std::sin(2.25f * 3.14159265f / 180), 0.01f);
}

TEST_F(ScriptSpringArmTest, SubsteppingKeepsConstantVelocityLagConsistentAcrossFrameRates)
{
    float positions[3]{};
    const int rates[3]{30, 60, 120};
    for (int index = 0; index < 3; ++index)
    {
        auto owner = Owner(300, false);
        owner["class"] = "ScriptFixtures.SpringArmMoverBlueprint";
        owner["components"][0]["enableCameraLag"] = true;
        owner["components"][0]["cameraLagMaxTimeStep"] = 1.0f / 120;
        Prepare(Json::array({owner}));
        Play();
        for (int frame = 0; frame < rates[index]; ++frame) Tick(1.0f / rates[index]);
        positions[index] = CameraPosition().z();
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error)) << error;
    }
    EXPECT_NEAR(positions[0], positions[1], 0.02f);
    EXPECT_NEAR(positions[1], positions[2], 0.02f);
}

TEST_F(ScriptSpringArmTest, RuntimeSettingsApplyWithoutRecreatingArm)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmMutationBlueprint";
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_TRUE(CameraPosition().isApprox(Eigen::Vector3f(0, 20, 180), 0.001f));
    const auto& arm = world->GetScriptBlueprintStates()[0]["components"][0];
    EXPECT_EQ(arm["targetArmLength"], 180);
    EXPECT_EQ(arm["targetOffset"], Json::array({0, 20, 0}));
}

TEST_F(ScriptSpringArmTest, FailedCallbackRestoresArmSettings)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmMutationBlueprint";
    owner["properties"] = {{"Fail", true}};
    Prepare(Json::array({owner}));
    Play(); Tick();
    const auto& state = world->GetScriptBlueprintStates()[0];
    EXPECT_TRUE(state["failed"].get<bool>());
    EXPECT_EQ(state["components"][0]["targetArmLength"], 300);
    EXPECT_EQ(state["components"][0]["targetOffset"], Json::array({0, 0, 0}));
}

TEST_F(ScriptSpringArmTest, LiveReloadPreservesLagHistoryAndAuthorCameraRotation)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmMoverBlueprint";
    owner["components"][0]["enableCameraLag"] = true;
    owner["components"][0]["rotationOffset"] = {0, 25, 0};
    Prepare(Json::array({owner}));
    Play();
    for (int frame = 0; frame < 10; ++frame) Tick();
    const auto before = CameraPosition();
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_TRUE(CameraPosition().isApprox(before, 0.001f));
    Tick();
    EXPECT_GT(CameraPosition().z(), before.z());
    EXPECT_LT(CameraPosition().z() - before.z(), 4);
    EXPECT_NEAR(CameraRotation().y(), 25, 0.01f);
}

TEST_F(ScriptSpringArmTest, DrawDebugKeepsSeparateLinesForMultipleArms)
{
    auto owner = Owner(300, false);
    owner["components"][0]["drawDebug"] = true;
    auto arm = owner["components"][0];
    arm["id"] = "second";
    arm["position"] = {100, 0, 0};
    auto camera = CameraNode(false, "second");
    camera["id"] = "secondCamera";
    owner["components"].push_back(arm);
    owner["components"].push_back(camera);
    Prepare(Json::array({owner}));
    Play(); Tick();
    EXPECT_EQ(world->GetDebugDraw().Count(), 2u);
}

TEST_F(ScriptSpringArmTest, ExtendedSchemaRejectsInvalidLagStepAndOffsets)
{
    auto arm = Owner()["components"][0];
    arm["cameraLagMaxTimeStep"] = 0;
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
    arm["cameraLagMaxTimeStep"] = 0.1f;
    arm["targetOffset"] = {1, 2};
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
    arm["targetOffset"] = {1, 2, 3};
    arm["useCameraLagSubstepping"] = 1;
    EXPECT_THROW(ValidateScriptComponents(Json::array({arm})), std::invalid_argument);
}

TEST_F(ScriptSpringArmTest, CollisionDiagnosticsKeepUnfixedPositionSeparateFromHit)
{
    auto owner = Owner();
    owner["class"] = "ScriptFixtures.SpringArmDiagnosticsBlueprint";
    owner["components"].push_back({{"id", "probe"}, {"type", "scene"}});
    Prepare(Json::array({owner, Wall(150)}));
    Play(); Tick(); Tick();
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["components"][2]["position"], Json::array({300, 1, 0}));
    EXPECT_NEAR(CameraPosition().z(), 128, 0.1f);
}

TEST_F(ScriptSpringArmTest, ResetCameraLagSnapsAfterExplicitTeleport)
{
    auto owner = Owner(300, false);
    owner["class"] = "ScriptFixtures.SpringArmTeleportBlueprint";
    owner["components"][0]["enableCameraLag"] = true;
    Prepare(Json::array({owner}));
    Play(); Tick(); Tick();
    EXPECT_NEAR(CameraPosition().z(), 400, 0.01f);
}

TEST_F(ScriptSpringArmTest, CollisionMaskCanExcludeWall)
{
    auto owner = Owner();
    owner["components"][0]["collisionMask"] = 0;
    Prepare(Json::array({owner, Wall(150)}));
    Play(); Tick();
    EXPECT_NEAR(CameraPosition().z(), 300, 0.01f);
}
}
#endif

TEST(ScriptComponentSpaceTest, AuthoredSpringArmCannotInjectRuntimeSocketTransform)
{
    const nlohmann::json arm{{"id", "arm"}, {"type", "springArm"},
        {"__springArmSocket", {{"position", {0, 0, 999}}, {"rotation", {0, 0, 0}}}}};
    EXPECT_THROW(VL::ValidateScriptComponents(nlohmann::json::array({arm})), std::invalid_argument);
}
