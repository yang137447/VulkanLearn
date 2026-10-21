#include "animation/motionHistory.h"
#include "animation/motionRuntime.h"
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

MotionObservation Observation(double time, uint64_t epoch = 0)
{
    MotionObservation value;
    value.time = time;
    value.poseEpoch = epoch;
    value.root(2, 3) = static_cast<float>(time * 100);
    value.joints.push_back({0, 90, value.root(2, 3)});
    return value;
}

MotionTrack StationaryQueryTrack()
{
    MotionTrack track;
    track.schema.joints = {"root"};
    track.schema.trajectoryTimes = {0, 0.5f};
    const double step = track.schema.velocityStep;
    MotionObservation first;
    first.joints = {{0, 90, 0}};
    track.history.Record(first);
    first.time = step;
    track.history.Record(first);
    track.time = step;
    track.walkVelocity = {0, 0, 200};
    return track;
}

TEST(MotionHistory, ResamplesTimeConsistentlyAcrossObservationRates)
{
    for (int rate : {30, 60, 120})
    {
        MotionHistory history;
        for (int frame = 0; frame <= rate; ++frame) history.Record(Observation(double(frame) / rate));
        EXPECT_EQ(history.Size(), 61u);
        MotionObservation sampled;
        ASSERT_TRUE(history.Sample(0.375, sampled));
        EXPECT_NEAR(sampled.root(2, 3), 37.5, 0.0001);
        EXPECT_NEAR(sampled.joints[0].z(), 37.5, 0.0001);
    }
}

TEST(MotionHistory, CutInvalidatesInterpolatedPoseWithoutLosingActualRoot)
{
    MotionHistory history;
    history.Record(Observation(0));
    auto cut = Observation(1.0 / 30, 1);
    cut.joints[0].y() = 150;
    history.Record(cut);
    MotionObservation before, current;
    ASSERT_TRUE(history.Sample(1.0 / 60, before));
    EXPECT_FALSE(before.poseValid);
    EXPECT_NEAR(before.root(2, 3), 100.0 / 60, 0.0001);
    ASSERT_TRUE(history.Sample(1.0 / 30, current));
    EXPECT_TRUE(current.poseValid);
    EXPECT_EQ(current.poseEpoch, 1u);
    history.Record(Observation(2.0 / 30, 1));
    ASSERT_TRUE(history.Sample(3.0 / 60, current));
    EXPECT_TRUE(current.poseValid);
}

TEST(MotionHistory, PauseDoesNotGrowAndTeleportOrGapClearsOldSamples)
{
    MotionHistory history;
    history.Record(Observation(0));
    history.Record(Observation(0));
    EXPECT_EQ(history.Size(), 1u);
    history.Record(Observation(0.2));
    auto teleport = Observation(0.3);
    teleport.root(0, 3) = 3000;
    history.Record(teleport);
    MotionObservation value;
    EXPECT_EQ(history.Size(), 1u);
    EXPECT_FALSE(history.Sample(0.2, value));
    history.Record(Observation(1));
    EXPECT_EQ(history.Size(), 1u);
    EXPECT_FALSE(history.Sample(0.3, value));
}

TEST(MotionHistory, HistoryRemainsBoundedAndRejectsBackwardsTime)
{
    MotionHistory history;
    for (int frame = 0; frame <= 600; ++frame) history.Record(Observation(frame / 60.0));
    EXPECT_LE(history.Size(), 68u);
    EXPECT_THROW(history.Record(Observation(9)), std::invalid_argument);
    history.Reset();
    MotionObservation result;
    EXPECT_FALSE(history.Sample(10, result));
}

TEST(MotionPrediction, UsesInstantWalkAndSameSemiImplicitGravity)
{
    const auto root = Eigen::Matrix4f::Identity().eval();
    const auto grounded = PredictCharacterRoot(root, {100, 500, 0}, {0, 0, 200}, true, 0.5f);
    EXPECT_NEAR(grounded(0, 3), 0, 0.0001f);
    EXPECT_NEAR(grounded(2, 3), 100, 0.0001f);
    EXPECT_NEAR(grounded(1, 3), 0, 0.0001f);
    const auto airborne = PredictCharacterRoot(root, {0, 500, 0}, {0, 0, 200}, false, 1.0f / 60);
    EXPECT_NEAR(airborne(1, 3), (500 - 981.0f / 60) / 60, 0.0001f);
}

TEST(MotionFeatures, RotatesWorldVelocityWithoutSubtractingRootDisplacement)
{
    FeatureSchema schema;
    schema.joints = {"root"};
    schema.trajectoryTimes = {0.5f};
    Eigen::Matrix4f root = Eigen::Matrix4f::Identity();
    root.block<3, 3>(0, 0) = Eigen::AngleAxisf(1.57079632679f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    root(0, 3) = 100;
    auto future = root;
    future(0, 3) += 50;
    const auto result = EncodeMotionFeatures(schema, root, {future}, {{110, 90, 0}}, {{100, 0, 0}});
    ASSERT_EQ(result.size(), 10u);
    EXPECT_NEAR(result[0], 0, 0.0001f);
    EXPECT_NEAR(result[1], 50, 0.0001f);
    EXPECT_NEAR(result[5], 90, 0.0001f);
    EXPECT_NEAR(result[6], 10, 0.0001f);
    EXPECT_NEAR(result[9], 100, 0.0001f);
}

TEST(MotionFeatures, SchemaRejectsAmbiguousOrderAndInvalidWeights)
{
    FeatureSchema schema;
    schema.joints = {"pelvis", "foot_l", "foot_r"};
    auto json = FeatureSchemaJson(schema);
    EXPECT_EQ(FeatureChannelNames(ParseFeatureSchema(json)).size(), 42u);
    json["trajectoryTimes"] = {0.2, -0.2};
    EXPECT_THROW(ParseFeatureSchema(json), std::invalid_argument);
    json = FeatureSchemaJson(schema);
    json["weights"]["poseVelocity"] = 0;
    EXPECT_THROW(ParseFeatureSchema(json), std::invalid_argument);
    json = FeatureSchemaJson(schema);
    json["version"] = 2;
    EXPECT_THROW(ParseFeatureSchema(json), std::invalid_argument);
}

TEST(MotionRuntimeQuery, TransportsDisplayedJointVelocityIntoNewHeadingAndActualRootVelocity)
{
    MotionTrack displayed;
    displayed.schema.joints = {"root"};
    displayed.schema.trajectoryTimes = {0};
    displayed.schema.velocityStep = 0.1f;
    const double step = displayed.schema.velocityStep;
    MotionObservation first;
    first.joints = {{10, 50, 0}};
    displayed.history.Record(first);
    MotionObservation second;
    second.time = step;
    second.root(2, 3) = 10;
    second.joints = {{12, 50, 10}};
    displayed.history.Record(second);
    displayed.time = step;
    auto query = displayed;
    query.achievedVelocity = {200, 0, 0};
    query.time = 2 * step;
    MotionObservation current;
    current.time = 2 * step;
    current.root.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(1.57079632679f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    current.root.block<3, 1>(0, 3) = Eigen::Vector3f(20, 0, 10);
    current.joints = {{20, 50, -2}};
    query.history.Record(current);
    std::vector<float> raw;
    ASSERT_TRUE(BuildRuntimeMotionQuery(query, raw, &displayed));
    ASSERT_EQ(raw.size(), 10u);
    // old root=(0,0,100), relative joint=(20,0,0); yaw90、新根=(200,0,0)。
    // 搬运后世界速度=(200,0,-20)，投到新 heading 得到=(20,0,200)。
    EXPECT_NEAR(raw[4], 12, 0.0001f);
    EXPECT_NEAR(raw[5], 50, 0.0001f);
    EXPECT_NEAR(raw[7], 20, 0.0001f);
    EXPECT_NEAR(raw[8], 0, 0.0001f);
    EXPECT_NEAR(raw[9], 200, 0.0001f);
}

TEST(MotionRuntimeQuery, PreservesLocalAnimationVelocityWithoutCountingOldRootTwice)
{
    MotionTrack displayed;
    displayed.schema.joints = {"root"};
    displayed.schema.trajectoryTimes = {0};
    displayed.schema.velocityStep = 0.1f;
    const double step = displayed.schema.velocityStep;
    auto first = Observation(0);
    first.joints[0].x() = 10;
    displayed.history.Record(first);
    auto second = Observation(step);
    second.joints[0].x() = 12;
    displayed.history.Record(second);
    displayed.time = step;
    auto query = displayed;
    query.achievedVelocity = {0, 0, 100};
    query.time = 2 * step;
    auto current = Observation(2 * step);
    current.joints[0].x() = 12; // 当前显示仍为上一帧已求值姿态。
    query.history.Record(current);
    std::vector<float> raw;
    ASSERT_TRUE(BuildRuntimeMotionQuery(query, raw, &displayed));
    EXPECT_NEAR(raw[7], 20, 0.0001f);
    EXPECT_NEAR(raw[9], 100, 0.0001f);
}

TEST(MotionRuntimeQuery, RejectsDisplayedVelocityAcrossPoseDiscontinuity)
{
    MotionTrack displayed;
    displayed.schema.joints = {"root"};
    displayed.schema.trajectoryTimes = {0};
    displayed.schema.velocityStep = 0.1f;
    const double step = displayed.schema.velocityStep;
    displayed.history.Record(Observation(0));
    displayed.history.Record(Observation(step, 1));
    displayed.time = step;
    auto query = displayed;
    query.time = 2 * step;
    query.history.Record(Observation(2 * step, 1));
    std::vector<float> raw;
    EXPECT_FALSE(BuildRuntimeMotionQuery(query, raw, &displayed));
}

TEST(MotionRuntimeQuery, UsesSamePhysicsVelocityAcrossNoSubstepAndDoubleDisplacementRenderObservations)
{
    MotionTrack displayed;
    displayed.schema.joints = {"root"};
    displayed.schema.trajectoryTimes = {0};
    const double step = displayed.schema.velocityStep;
    MotionObservation first;
    first.joints = {{0, 90, 0}};
    displayed.history.Record(first);
    auto second = first;
    second.time = step;
    second.root(2, 3) = static_cast<float>(200 * step);
    second.joints[0].z() = second.root(2, 3);
    displayed.history.Record(second);
    displayed.time = step;
    for (float displacement : {0.0f, 4.0f})
    {
        auto query = displayed;
        query.time += 0.01;
        query.achievedVelocity = {0, 0, 200};
        auto current = second;
        current.time = query.time;
        current.root(2, 3) += displacement;
        current.joints[0].z() += displacement;
        query.history.Record(current);
        std::vector<float> raw;
        ASSERT_TRUE(BuildRuntimeMotionQuery(query, raw, &displayed));
        EXPECT_NEAR(raw[9], 200, 0.001f) << "render displacement=" << displacement;
    }
}

TEST(MotionRuntimeQuery, GroundedBlockedMovementPredictsAchievedMotionInsteadOfWalkingThroughWall)
{
    auto track = StationaryQueryTrack();
    track.achievedVelocity = {1, 0, 0};
    std::vector<float> raw;
    ASSERT_TRUE(BuildRuntimeMotionQuery(track, raw));
    // 半秒按已达到的 1 cm/s 漂移 0.5 cm，不能继续按输入前进 100 cm。
    EXPECT_NEAR(raw[4], 0.5f, 0.0001f);
    EXPECT_NEAR(raw[5], 0, 0.0001f);
}

TEST(MotionRuntimeQuery, FreelyMovingGroundedCharacterRetainsInstantDesiredPrediction)
{
    auto track = StationaryQueryTrack();
    track.achievedVelocity = {0, 0, 200};
    track.walkVelocity = {200, 0, 0};
    std::vector<float> raw;
    ASSERT_TRUE(BuildRuntimeMotionQuery(track, raw));
    EXPECT_NEAR(raw[4], 100, 0.0001f);
    EXPECT_NEAR(raw[5], 0, 0.0001f);
}

TEST(MotionRuntimeQuery, AirborneCharacterRetainsDesiredPredictionWhenHorizontalSpeedIsZero)
{
    auto track = StationaryQueryTrack();
    track.grounded = false;
    std::vector<float> raw;
    ASSERT_TRUE(BuildRuntimeMotionQuery(track, raw));
    EXPECT_NEAR(raw[4], 0, 0.0001f);
    EXPECT_NEAR(raw[5], 100, 0.0001f);
}
}
