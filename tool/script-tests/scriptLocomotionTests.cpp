#include <gtest/gtest.h>
#include <cmath>
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "sceneNode.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
using namespace VL;
using Json = nlohmann::json;

// 与 script/VulkanLearn.Runtime/LocomotionStateMachine.cs 的显式枚举值保持一致。
enum : int { kIdle = 0, kWalk = 1, kJog = 2 };
enum : int { kGrounded = 0, kAirborne = 1 };
enum : int { kForward = 0, kLeft90 = 1, kLeft180 = 2, kRight90 = 3, kRight180 = 4 };
enum : int { kNone = 0, kStartWalk = 1, kStartJog = 2, kStopWalk = 3, kStopJog = 4, kFall = 5, kLand = 6 };

class ScriptLocomotionTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "locomotion");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }

    Json Owner(const Json& properties, const std::string& className = "ScriptFixtures.LocomotionProbeBlueprint")
    {
        return {{"type", "blueprintInstance"}, {"id", "owner"}, {"name", "owner"},
            {"class", className}, {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}},
            {"scale", {1, 1, 1}}, {"properties", properties}, {"components", Json::array()}};
    }

    void Prepare(const Json& properties, const std::string& className = "ScriptFixtures.LocomotionProbeBlueprint")
    {
        PrepareObject(Owner(properties, className));
    }
    void PrepareObject(const Json& blueprint)
    {
        if (!runtime.IsEditing())
        {
            std::string error;
            ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error)) << error;
        }
        runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}});
        runtime.SynchronizeWorld(world);
    }
    void PrepareAnimated(const std::string& driverClass)
    {
        PrepareObject({{"type", "blueprintInstance"}, {"id", "owner"}, {"name", "owner"},
            {"class", driverClass}, {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}},
            {"scale", {1, 1, 1}}, {"properties", Json::object()},
            {"components", Json::array({{{"id", "visual"}, {"type", "mesh"}, {"parent", ""},
                {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
                {"modelPath", "Common/Meshes/SM_material_preview_ball.json"}}})},
            {"scriptComponents", Json::array({{{"id", "anim"},
                {"class", "ScriptFixtures.LocomotionClockProbeBlueprint"},
                {"properties", {{"MeshComponentId", "visual"}}}}})}});
    }
    void PrepareAction(int triggerFrame, int cancelFrame)
    {
        PrepareObject({{"type", "blueprintInstance"}, {"id", "owner"}, {"name", "owner"},
            {"class", "ScriptFixtures.LocomotionActionDriverBlueprint"}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"properties", {{"TriggerFrame", triggerFrame}, {"CancelFrame", cancelFrame}}},
            {"components", Json::array({{{"id", "visual"}, {"type", "mesh"}, {"parent", ""},
                {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
                {"modelPath", "Common/Meshes/SM_material_preview_ball.json"}}})},
            {"scriptComponents", Json::array({{{"id", "anim"},
                {"class", "ScriptFixtures.LocomotionActionClockBlueprint"},
                {"properties", {{"MeshComponentId", "visual"}}}}})}});
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

    const Json& State() const { return world->GetScriptBlueprintStates()[0]; }
    int StateValue() const { return static_cast<int>(std::lround(State()["position"][0].get<float>())); }
    int ModeValue() const { return static_cast<int>(std::lround(State()["position"][1].get<float>())); }
    int TransitionValue() const { return static_cast<int>(std::lround(State()["position"][2].get<float>())); }
    int DirectionValue() const { return static_cast<int>(std::lround(State()["rotation"][0].get<float>())); }
    float TargetSpeedValue() const { return State()["rotation"][1].get<float>(); }

    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptLocomotionTest, IdleToWalkEmitsForwardStart)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", -1}, {"DesiredSpeed", 200}, {"PlanarSpeed", 200}});
    Play(); Tick();
    EXPECT_EQ(StateValue(), kWalk);
    EXPECT_EQ(ModeValue(), kGrounded);
    EXPECT_EQ(TransitionValue(), kStartWalk);
    EXPECT_EQ(DirectionValue(), kForward);
    EXPECT_FLOAT_EQ(TargetSpeedValue(), 200);
}

TEST_F(ScriptLocomotionTest, IdleToJogEmitsJogStart)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", -1}, {"DesiredSpeed", 450}, {"PlanarSpeed", 450}});
    Play(); Tick();
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(TransitionValue(), kStartJog);
    EXPECT_FLOAT_EQ(TargetSpeedValue(), 450);
}

TEST_F(ScriptLocomotionTest, DirectionBucketsUseSignedRelativeYaw)
{
    const struct { float x; float z; int direction; } cases[] = {
        {0, -1, kForward},
        {-1, 0, kLeft90},
        {1, 0, kRight90},
        {0, 1, kRight180}
    };
    for (const auto& item : cases)
    {
        Prepare({{"DesiredX", item.x}, {"DesiredZ", item.z},
            {"DesiredSpeed", 200}, {"PlanarSpeed", 200}});
        Play(); Tick();
        EXPECT_EQ(DirectionValue(), item.direction)
            << "direction for (" << item.x << ", " << item.z << ")";
    }
}

TEST_F(ScriptLocomotionTest, GaitSequenceStartsStopsAndHandlesAirborneLanding)
{
    Prepare({{"Scenario", 1}});
    Play();

    for (int frame = 0; frame < 3; ++frame) Tick();
    EXPECT_EQ(StateValue(), kWalk);
    EXPECT_EQ(TransitionValue(), kStartWalk);

    for (int frame = 0; frame < 2; ++frame) Tick();
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(TransitionValue(), kNone);

    for (int frame = 0; frame < 4; ++frame) Tick();
    EXPECT_EQ(StateValue(), kIdle);
    EXPECT_EQ(TransitionValue(), kStopWalk);
    EXPECT_EQ(ModeValue(), kGrounded);

    Tick();
    EXPECT_EQ(TransitionValue(), kNone);

    Tick();
    EXPECT_EQ(ModeValue(), kAirborne);
    EXPECT_EQ(TransitionValue(), kFall);

    Tick();
    EXPECT_EQ(ModeValue(), kGrounded);
    EXPECT_EQ(TransitionValue(), kLand);
    EXPECT_EQ(StateValue(), kWalk);
}

TEST_F(ScriptLocomotionTest, HysteresisKeepsJogThenFallsBackToWalk)
{
    Prepare({{"Scenario", 2}});
    Play();

    for (int frame = 0; frame < 5; ++frame) Tick();
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(TransitionValue(), kNone);

    for (int frame = 0; frame < 2; ++frame) Tick();
    EXPECT_EQ(StateValue(), kWalk);
    EXPECT_EQ(TransitionValue(), kNone);
}

TEST_F(ScriptLocomotionTest, InvalidSettingsAreRejected)
{
    Prepare({{"WalkSpeed", 450}, {"JogSpeed", 200}}, "ScriptFixtures.LocomotionValidationBlueprint");
    Play(); Tick();
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f, 0.001f);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, HysteresisBandMustSitBetweenWalkAndJog)
{
    Prepare({{"WalkToJogEnterSpeed", 100}}, "ScriptFixtures.LocomotionValidationBlueprint");
    Play(); Tick();
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f, 0.001f);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, ClipSetPinsValleyDirectionalNames)
{
    Prepare(Json::object(), "ScriptFixtures.LocomotionClipProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, AnimationBlueprintComponentOwnsGaitDecision)
{
    PrepareObject({{"type", "blueprintInstance"}, {"id", "owner"}, {"name", "owner"},
        {"class", "ScriptFixtures.LocomotionAnimationDriverBlueprint"}, {"position", {0, 0, 0}},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", {{"DesiredSpeed", 450}}},
        {"components", Json::array({{{"id", "visual"}, {"type", "mesh"}, {"parent", ""},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"modelPath", "Common/Meshes/SM_material_preview_ball.json"}}})},
        {"scriptComponents", Json::array({{{"id", "anim"},
            {"class", "ScriptFixtures.LocomotionAnimationProbeBlueprint"},
            {"properties", {{"MeshComponentId", "visual"}}}}})}});
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(TransitionValue(), kStartJog);
}

TEST_F(ScriptLocomotionTest, ReleaseDuringStartClipInterruptsToIdle)
{
    PrepareAnimated("ScriptFixtures.LocomotionStartReleaseDriverBlueprint");
    Play();
    // Valley 的 Idle_to_Walk_Fwd 有 2.07s；前两帧仍应停留在起步片段。
    Tick();
    Tick();
    Tick();
    EXPECT_EQ(StateValue(), 4);
    EXPECT_FLOAT_EQ(State()["position"][1].get<float>(), 0);

    // 松开输入：物理已停（PlanarSpeed=0），不能等 2.07s 起步片段播完。
    Tick();
    EXPECT_EQ(StateValue(), 1);
    EXPECT_FLOAT_EQ(State()["position"][1].get<float>(), 1);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, AirborneHoldsFallLastFrameInsteadOfDroppingToGroundIdle)
{
    PrepareAnimated("ScriptFixtures.LocomotionAirborneDriverBlueprint");
    Play();
    for (int frame = 0; frame < 30; ++frame) Tick();
    // UE 的 Jump_Idle_Fall 是 bLoopAnimation=False：0.3s 播完后保持末帧，
    // 仍停在 Airborne（code 8），不能切到 Idle（code 1）或重启 Fall。
    EXPECT_EQ(StateValue(), 8);
    EXPECT_FLOAT_EQ(State()["position"][1].get<float>(), 0);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, GaitChangeDuringStartClipKeepsAnimationAlive)
{
    // 起步片段被打断时 Walk→Jog 会带同步组、不携带相位；这必须是合法请求。
    // 曾经把这种组合判成非法，动画组件被禁用后角色会永久停在最后一帧。
    PrepareAnimated("ScriptFixtures.LocomotionStartToJogDriverBlueprint");
    Play();
    for (int frame = 0; frame < 20; ++frame) Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    // Walk 中途切 Jog 不再播 Jog 起步，直接进 Jog 循环。
    EXPECT_EQ(StateValue(), 3);
}

TEST_F(ScriptLocomotionTest, BackwardVelocityKeepsIdle)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", -1}, {"DesiredSpeed", 200}, {"PlanarSpeed", 200},
        {"VelocityZ", 1}});
    Play(); Tick();
    EXPECT_EQ(StateValue(), kIdle);
    EXPECT_EQ(TransitionValue(), kNone);
}

TEST_F(ScriptLocomotionTest, IdleEntryUsesSpeedThresholdsForStopClips)
{
    Prepare({{"Scenario", 3}});
    Play();
    Tick(); Tick();
    EXPECT_EQ(StateValue(), kWalk);
    Tick();
    EXPECT_EQ(StateValue(), kIdle);
    EXPECT_EQ(TransitionValue(), kStopJog);
}

TEST_F(ScriptLocomotionTest, StartAndCycleTransitionsUseEchoBlendTimes)
{
    PrepareAnimated("ScriptFixtures.LocomotionWalkHoldDriverBlueprint");
    Play();
    Tick();
    EXPECT_EQ(StateValue(), 4);
    // UE Echo 的 Locomotion Idle→Walk/Jog 是 0 秒硬切。
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 0.0f);
    // 假时钟的 Idle_to_Walk_Fwd 为 2.07s；130 帧足够播完并进入 Cycles。
    for (int frame = 0; frame < 130; ++frame) Tick();
    EXPECT_EQ(StateValue(), 2);
    EXPECT_FLOAT_EQ(State()["position"][1].get<float>(), 1);
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 0.5f);
}

TEST_F(ScriptLocomotionTest, StopTransitionBlendsBackToIdleAtHalfSecond)
{
    PrepareAnimated("ScriptFixtures.LocomotionStopDriverBlueprint");
    Play();
    Tick(); Tick(); Tick();
    EXPECT_EQ(StateValue(), 6);
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 0.3f);
    // Walk_Fwd_to_Idle 在假时钟里 5.67s；播完后 Stop→Idle 的 UE 自动规则为 0.5s。
    for (int frame = 0; frame < 360; ++frame) Tick();
    EXPECT_EQ(StateValue(), 1);
    EXPECT_FLOAT_EQ(State()["position"][1].get<float>(), 1);
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 0.5f);
}

TEST_F(ScriptLocomotionTest, SpeedCurveCanPromoteGaitWithoutReplacingExplicitInput)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", -1}, {"DesiredSpeed", 200}, {"PlanarSpeed", 200},
        {"SpeedCurve", 350}});
    Play(); Tick();
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(TransitionValue(), kStartJog);
}

TEST_F(ScriptLocomotionTest, SpeedCurveCannotDemoteExplicitJogIntent)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", -1}, {"DesiredSpeed", 450}, {"PlanarSpeed", 450},
        {"SpeedCurve", 100}});
    Play(); Tick();
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(TransitionValue(), kStartJog);
}

TEST_F(ScriptLocomotionTest, ActionLayerDoesNotOverwriteMontageOnTriggerFrame)
{
    PrepareAction(2, -1);
    Play();
    Tick();
    Tick();
    const int baseCalls = static_cast<int>(std::lround(State()["position"][1].get<float>()));
    Tick(); // trigger frame
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 1.0f);
    EXPECT_EQ(static_cast<int>(std::lround(State()["position"][1].get<float>())), baseCalls);
    EXPECT_FLOAT_EQ(State()["rotation"][2].get<float>(), 1.0f);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, ActionLayerRestoresBaseAfterNativeMontageCompletion)
{
    PrepareAction(0, -1);
    Play();
    Tick();
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 1.0f);
    for (int frame = 0; frame < 260; ++frame) Tick();
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 0.0f);
    EXPECT_EQ(StateValue(), 2); // Walk_Fwd base restored from the latest input.
    EXPECT_GT(State()["position"][1].get<float>(), 1.0f);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, ActionLayerCancelRestoresBaseImmediately)
{
    PrepareAction(0, 2);
    Play();
    Tick();
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 1.0f);
    for (int frame = 0; frame < 2; ++frame) Tick();
    EXPECT_FLOAT_EQ(State()["position"][2].get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(State()["rotation"][2].get<float>(), 0.0f);
    for (int frame = 0; frame < 180; ++frame) Tick();
    EXPECT_EQ(StateValue(), 2);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, WalkRotationCurveScalesStartAngleBeforeBucketing)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", 1}, {"DesiredSpeed", 200}, {"PlanarSpeed", 200},
        {"WalkRotationDeltaCurve", 0.5f}});
    Play(); Tick();
    // 无曲线时 180° 是 Right180；剩余转向比例 0.5 把起步角缩到 90°，应选 Right90。
    EXPECT_EQ(DirectionValue(), kRight90);
}

TEST_F(ScriptLocomotionTest, JogRotationCurveIsSelectedForJogGait)
{
    Prepare({{"DesiredX", 0}, {"DesiredZ", 1}, {"DesiredSpeed", 450}, {"PlanarSpeed", 450},
        {"WalkRotationDeltaCurve", 1}, {"JogRotationDeltaCurve", 0.5f}});
    Play(); Tick();
    EXPECT_EQ(StateValue(), kJog);
    EXPECT_EQ(DirectionValue(), kRight90);
}

TEST_F(ScriptLocomotionTest, InvalidOptionalCurveValuesAreRejected)
{
    Prepare(Json::object(), "ScriptFixtures.LocomotionCurveValidationBlueprint");
    Play(); Tick();
    EXPECT_NEAR(State()["position"][0].get<float>(), 2.0f, 0.001f);
    EXPECT_FALSE(State()["failed"].get<bool>());
}

TEST_F(ScriptLocomotionTest, AnimationGraphFakeCharacterChangesOnlyData)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphFakeCharacterBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["position"][1].get<float>(), 2.0f, 0.001f);
    EXPECT_NEAR(State()["position"][2].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["rotation"][0].get<float>(), 0.1f, 0.001f);
}

TEST_F(ScriptLocomotionTest, AnimationGraphValidationRejectsInvalidDefinitions)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphValidationBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    // 1 duplicate, 2 unknown target, 4 nested cycle, 8 unreachable, 16 empty clip,
    // 32 invalid threshold, 64 budget, 128 unknown state alias source,
    // 256 blend sample count, 512 unsorted blend thresholds, 1024 duplicate blend clip,
    // 2048 unsupported blend axis, 4096 conduit with clip, 8192 entry rule on
    // content state, 16384 entry conduit without AllowConduitEntryStates,
    // 32768 max transitions per frame out of range, 65536 additive on conduit,
    // 131072 additive alpha without clip, 262144 cached pose with clip,
    // 1048576 layer on conduit, 2097152 layer alpha without clip,
    // 4194304 duplicate layer mask bone.
    // 8388608 adds a slot mask without a slot name.
    EXPECT_NEAR(State()["position"][0].get<float>(), 16252927.0f, 0.001f);
}

TEST_F(ScriptLocomotionTest, PoseGraphReportsCachedPoseAdditiveAndSlot)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphPoseGraphProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["position"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["rotation"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["scale"][axis].get<float>(), 1.0f, 0.001f);
    }
}

TEST_F(ScriptLocomotionTest, LayeredPoseDecisionFieldsAndValidation)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphLayerProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["position"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["rotation"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["scale"][axis].get<float>(), 1.0f, 0.001f);
    }
}

TEST_F(ScriptLocomotionTest, StateMachineSettingsBudgetAndFirstUpdateBlend)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphSettingsProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["position"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["rotation"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["scale"][axis].get<float>(), 1.0f, 0.001f);
    }
}

TEST_F(ScriptLocomotionTest, ConduitResolvesSameFrameAndFallsBackWhenBlocked)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphConduitProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["position"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["rotation"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["scale"][axis].get<float>(), 1.0f, 0.001f);
    }
}

TEST_F(ScriptLocomotionTest, BlendSpaceAssetResolvesToRuntimeSamples)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphBlendSpaceAssetProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["position"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["rotation"][axis].get<float>(), 1.0f, 0.001f);
        EXPECT_NEAR(State()["scale"][axis].get<float>(), 1.0f, 0.001f);
    }
}

TEST_F(ScriptLocomotionTest, StateAliasAndVerticalSpeedDriveSharedTransition)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphAliasProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["position"][1].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["position"][2].get<float>(), 1.0f, 0.001f);
}

TEST_F(ScriptLocomotionTest, BlendSpaceSelectsClipsAndWeightsOnSpeedAxis)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphBlendSpaceProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["position"][axis].get<float>(), 1.0f, 0.001f);
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["rotation"][axis].get<float>(), 1.0f, 0.001f);
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        EXPECT_NEAR(State()["scale"][axis].get<float>(), 1.0f, 0.001f);
    }
}

TEST_F(ScriptLocomotionTest, EchoResourceGraphDefinesClipsWithoutCharacterCode)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphEchoProbeBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    EXPECT_NEAR(State()["position"][0].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["position"][1].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["position"][2].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["rotation"][0].get<float>(), 1.0f, 0.001f);
    EXPECT_NEAR(State()["rotation"][1].get<float>(), 1.0f, 0.001f);
    // 180° 起步片段入口时间来自 sidecar（UE StartPosition=0.20）。
    EXPECT_NEAR(State()["rotation"][2].get<float>(), 1.0f, 0.001f);
}

TEST_F(ScriptLocomotionTest, AnimationGraphMatchesLegacyLocomotionDecisions)
{
    Prepare(Json::object(), "ScriptFixtures.AnimationGraphParityBlueprint");
    Play(); Tick();
    EXPECT_FALSE(State()["failed"].get<bool>());
    EXPECT_NEAR(State()["position"][0].get<float>(), 0.0f, 0.001f);
    EXPECT_NEAR(State()["position"][1].get<float>(), -1.0f, 0.001f);
}

class ScriptLocomotionCurveRateTest : public ScriptLocomotionTest,
    public testing::WithParamInterface<int>
{
};

TEST_P(ScriptLocomotionCurveRateTest, CurveDrivenGaitDoesNotOscillate)
{
    const int rate = GetParam();
    RecordProperty("frameRate", rate);
    Prepare(Json::object(), "ScriptFixtures.LocomotionCurveRateProbeBlueprint");
    Play();
    const float dt = 1.0f / rate;
    for (int frame = 0; frame < rate; ++frame) Tick(dt);

    EXPECT_EQ(StateValue(), kJog);
    EXPECT_LT(State()["position"][2].get<float>(), 1.0f);
    // 第二个整秒只允许稳定 Jog/None；速度曲线单调上升不能反复越过阈值。
    for (int frame = 0; frame < rate; ++frame)
    {
        Tick(dt);
        ASSERT_EQ(StateValue(), kJog) << "frame=" << frame;
        ASSERT_EQ(static_cast<int>(State()["position"][1].get<float>()), kNone) << "frame=" << frame;
    }
}

INSTANTIATE_TEST_SUITE_P(FormalFrameRates, ScriptLocomotionCurveRateTest,
    testing::Values(30, 60, 120), testing::PrintToStringParamName());

class ScriptAnimationGraphRateTest : public ScriptLocomotionTest,
    public testing::WithParamInterface<int>
{
};

TEST_P(ScriptAnimationGraphRateTest, GraphStartStopAirborneSettlesAtSameState)
{
    const int rate = GetParam();
    RecordProperty("frameRate", rate);
    Prepare({{"FrameRate", rate}}, "ScriptFixtures.AnimationGraphRateProbeBlueprint");
    Play();
    const float dt = 1.0f / rate;
    for (int frame = 0; frame < rate * 9; ++frame) Tick(dt);
    EXPECT_EQ(StateValue(), kIdle) << "frameRate=" << rate;
    EXPECT_EQ(TransitionValue(), kNone) << "frameRate=" << rate << " state=" << State().dump();
    EXPECT_NEAR(State()["rotation"][0].get<float>(), 1.0f, 0.001f) << "clip should be Idle";
}

INSTANTIATE_TEST_SUITE_P(FormalFrameRates, ScriptAnimationGraphRateTest,
    testing::Values(30, 60, 120), testing::PrintToStringParamName());
}
#endif
