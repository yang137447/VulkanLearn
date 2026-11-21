#include "animation/motionBricks/motionBricksContract.h"
#include "animation/motionBricks/motionBricksMath.h"
#include "animation/motionBricks/motionBricksPlayer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace VL::Animation::MotionBricks;
using json = nlohmann::json;

std::filesystem::path FixturePath(const char* name)
{
    return std::filesystem::path(VL_MOTIONBRICKS_FIXTURE_DIR) / name;
}

json LoadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open fixture " + path.string());
    json value;
    input >> value;
    return value;
}

Transform ReadTransform(const json& value)
{
    const std::vector<float> values = value.get<std::vector<float>>();
    if (values.size() != 7)
        throw std::runtime_error("Transform fixture must have 7 values");
    Transform transform;
    transform.position = Eigen::Vector3f(values[0], values[1], values[2]);
    transform.rotation = Eigen::Quaternionf(values[6], values[3], values[4], values[5]);
    transform.rotation.normalize();
    return transform;
}

Pose ReadPose(const json& value)
{
    Pose pose;
    for (const json& joint : value)
        pose.push_back(ReadTransform(joint));
    return pose;
}

ModelContract LoadContract()
{
    return ParseModelManifest(FixturePath("soma_n1.json"));
}

SkeletonBinding MakeBinding(const json& golden, const ModelContract& contract)
{
    return BindSomaSkeleton(contract,
        golden["joint_names"].get<std::vector<std::string>>(),
        golden["parents"].get<std::vector<int>>(),
        ReadPose(golden["reference"]));
}

EncodeConfig ReadConfig(const json& golden)
{
    EncodeConfig config;
    config.allowedTokenCounts = golden["allowed_token_counts"].get<std::vector<int>>();
    const std::vector<float> heading = golden["heading_alignment"].get<std::vector<float>>();
    config.headingAlignment = Eigen::Vector2f(heading[0], heading[1]);
    config.modelToRuntime = ReadTransform(golden["model_to_runtime"]);
    return config;
}

Pose MakePose(const Pose& reference, float rootX, float yaw)
{
    Pose pose = reference;
    pose[0].position.x() = rootX;
    pose[0].rotation = Eigen::Quaternionf(
        Eigen::AngleAxisf(yaw, Eigen::Vector3f::UnitY()));
    return pose;
}

GenerationParams CommitParams(int earlyFrames = 0)
{
    GenerationParams params;
    params.transitionEarlyByFrames = earlyFrames;
    return params;
}

DecodedMotion MakeMotion(const Pose& reference, int frameCount, float rootStep = 1.0f,
    float yawStep = 0.1f, float contactMaximum = 1.0f)
{
    DecodedMotion motion;
    for (int frame = 0; frame < frameCount; ++frame)
    {
        motion.frames.push_back(MakePose(reference, rootStep * frame, yawStep * frame));
        const float contact = frameCount > 1 ?
            contactMaximum * static_cast<float>(frame) /
                static_cast<float>(frameCount - 1) : 0.0f;
        motion.footContacts.push_back({contact, contact, contact, contact});
    }
    return motion;
}

void ExpectRootNear(const Pose& pose, float expectedX, float tolerance = 1e-5f)
{
    ASSERT_FALSE(pose.empty());
    EXPECT_NEAR(pose[0].position.x(), expectedX, tolerance);
}

void ExpectRotationNear(const Eigen::Quaternionf& actual,
    const Eigen::Quaternionf& expected, float tolerance = 1e-5f)
{
    EXPECT_NEAR(std::abs(actual.normalized().dot(expected.normalized())), 1.0f, tolerance);
}

void ExpectEncodedNear(const EncodedInputs& actual, const EncodedInputs& expected)
{
    EXPECT_NEAR(actual.alignment.position.x(), expected.alignment.position.x(), 1e-5f);
    EXPECT_NEAR(actual.alignment.position.y(), expected.alignment.position.y(), 1e-5f);
    EXPECT_NEAR(actual.alignment.position.z(), expected.alignment.position.z(), 1e-5f);
    ExpectRotationNear(actual.alignment.rotation, expected.alignment.rotation);
    for (size_t frame = 0; frame < kContextAndTargetPoses; ++frame)
    {
        for (size_t value = 0; value < 5; ++value)
            EXPECT_NEAR(actual.globalRootValues[frame][value],
                expected.globalRootValues[frame][value], 1e-5f);
        for (size_t value = 0; value < 4; ++value)
            EXPECT_NEAR(actual.localRootValues[frame][value],
                expected.localRootValues[frame][value], 1e-4f);
        for (size_t value = 0; value < 267; ++value)
            EXPECT_NEAR(actual.localPoses[frame][value],
                expected.localPoses[frame][value], 1e-5f);
        EXPECT_EQ(actual.hasGlobalRootValues[frame], expected.hasGlobalRootValues[frame]);
        EXPECT_EQ(actual.hasLocalRootValues[frame], expected.hasLocalRootValues[frame]);
        EXPECT_EQ(actual.hasLocalPoses[frame], expected.hasLocalPoses[frame]);
    }
    EXPECT_EQ(actual.allowedPredNumTokens, expected.allowedPredNumTokens);
}

TEST(MotionBricksPlayer, PoseHistorySamplesRecordedFramesAtThirtyHertz)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    PoseHistory history(32);
    history.Fill(MakePose(reference, 0.0f, 0.0f));
    for (int frame = 1; frame <= 31; ++frame)
        history.Record(1.0f / 60.0f, MakePose(reference, static_cast<float>(frame), 0.0f));
    ASSERT_TRUE(history.HasPose());
    ExpectRootNear(history.Latest(), 31.0f);

    const auto samples = history.SampleFour();
    ExpectRootNear(samples[0], 25.0f);
    ExpectRootNear(samples[1], 27.0f);
    ExpectRootNear(samples[2], 29.0f);
    ExpectRootNear(samples[3], 31.0f);
    EXPECT_TRUE(samples[3][1].position.isApprox(reference[1].position));
}

TEST(MotionBricksPlayer, PoseHistoryClampsSamplesBeforeRecordedRange)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    PoseHistory history(4);
    history.Fill(MakePose(reference, 0.0f, 0.0f));
    history.Record(1.0f / 30.0f, MakePose(reference, 10.0f, 0.0f));
    const auto samples = history.SampleFour();
    ExpectRootNear(samples[0], 0.0f);
    ExpectRootNear(samples[1], 0.0f);
    ExpectRootNear(samples[2], 0.0f);
    ExpectRootNear(samples[3], 10.0f);

    EXPECT_THROW(history.Record(0.0f, reference), std::invalid_argument);
    Pose wrong = reference;
    wrong.pop_back();
    EXPECT_THROW(history.Record(1.0f / 30.0f, wrong), std::invalid_argument);
}

TEST(MotionBricksPlayer, GenerationInputUsesHistoryContextAndSelectedTargets)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const std::array<Pose, kContextPoses> context = {
        ReadPose(golden["context"][0]),
        ReadPose(golden["context"][1]),
        ReadPose(golden["context"][2]),
        ReadPose(golden["context"][3])};
    const std::vector<Pose> target = {
        ReadPose(golden["target"][0]),
        ReadPose(golden["target"][1])};

    GeneratedMotionPlayer player(binding, 8);
    player.SeedHistory(context[0]);
    player.RecordPose(1.0f / 30.0f, context[1]);
    player.RecordPose(1.0f / 30.0f, context[2]);
    player.RecordPose(1.0f / 30.0f, context[3]);

    GenerationParams params;
    params.numTargetPoses = 2;
    params.transitionEarlyByFrames = 2;
    params.encode = ReadConfig(golden);
    const EncodedInputs actual = player.BuildGenerationInput(target, params);
    const EncodedInputs expected = EncodeSomaInputs(binding, context,
        {target[0], target[1]}, params.encode);
    ExpectEncodedNear(actual, expected);
    EXPECT_EQ(actual.hasLocalRootValues[3], 0);
    EXPECT_EQ(actual.hasLocalRootValues[5], 0);
    EXPECT_EQ(actual.hasLocalRootValues[6], 0);
    EXPECT_EQ(actual.hasLocalRootValues[7], 0);
}

TEST(MotionBricksPlayer, CommitAndSampleAdvancePlaybackAndRootMotion)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding, 8);
    player.SeedHistory(MakePose(reference, 0.0f, 0.0f));

    player.CommitGeneration(MakeMotion(reference, 18, 1.0f, 0.1f, 1.0f),
        CommitParams(2));

    EXPECT_FLOAT_EQ(player.PlaybackDuration(), 15.0f / 30.0f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 15.0f / 30.0f);

    PlaybackFrame frame = player.Sample(1.0f / 60.0f);
    ExpectRootNear(frame.pose, 0.5f);
    EXPECT_NEAR(frame.footContacts[0], 0.5f / 17.0f, 1e-5f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 0.5f, 1e-5f);
    EXPECT_NEAR(frame.rootMotionYawDelta, 0.05f, 1e-5f);
    EXPECT_FALSE(frame.hardCut);
    EXPECT_FLOAT_EQ(frame.sourceTime, 1.0f / 60.0f);

    frame = player.Sample(1.0f / 60.0f);
    ExpectRootNear(frame.pose, 1.0f);
    EXPECT_NEAR(frame.footContacts[0], 1.0f / 17.0f, 1e-5f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 0.5f, 1e-5f);
    EXPECT_NEAR(frame.rootMotionYawDelta, 0.05f, 1e-5f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 14.0f / 30.0f);

    frame = player.Sample(1.0f / 60.0f);
    ExpectRootNear(frame.pose, 1.5f);
    EXPECT_NEAR(frame.footContacts[0], 1.5f / 17.0f, 1e-5f);

    frame = player.Sample(1.0f / 60.0f);
    ExpectRootNear(frame.pose, 2.0f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 13.0f / 30.0f);

    frame = player.Sample(1.0f);
    ExpectRootNear(frame.pose, 17.0f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 15.0f, 1e-5f);
    EXPECT_NEAR(frame.rootMotionYawDelta, 1.5f, 1e-5f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 0.0f);
    EXPECT_NEAR(player.CurrentRootPosition().x(), 17.0f, 1e-5f);
}

TEST(MotionBricksPlayer, HardCutIsReportedOnlyOnFirstSample)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding, 4);
    player.SeedHistory(reference);
    player.CommitGeneration(MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f),
        CommitParams(), true);

    EXPECT_TRUE(player.Sample(0.0f).hardCut);
    EXPECT_FALSE(player.Sample(1.0f / 60.0f).hardCut);
}

TEST(MotionBricksPlayer, TimeToTransitionUsesLeadFramesAndClamps)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding, 4);
    player.SeedHistory(MakePose(reference, 0.0f, 0.0f));
    DecodedMotion motion;
    for (int frame = 0; frame < 18; ++frame)
    {
        motion.frames.push_back(MakePose(reference, static_cast<float>(frame), 0.0f));
        motion.footContacts.push_back({0, 0, 0, 0});
    }
    player.CommitGeneration(std::move(motion), CommitParams(2));
    EXPECT_FLOAT_EQ(player.PlaybackDuration(), 15.0f / 30.0f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 15.0f / 30.0f);

    PlaybackFrame frame = player.Sample(0.25f);
    ExpectRootNear(frame.pose, 7.5f, 1e-4f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 0.25f);

    frame = player.Sample(1.0f);
    ExpectRootNear(frame.pose, 17.0f, 1e-4f);
    EXPECT_FLOAT_EQ(player.TimeToTransition(), 0.0f);
}

TEST(MotionBricksPlayer, RejectsInvalidLifecycleAndInputs)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding);
    EXPECT_THROW(player.Sample(0.0f), std::logic_error);
    EXPECT_THROW(player.BuildGenerationInput({reference}, GenerationParams{}),
        std::logic_error);
    player.SeedHistory(reference);
    EXPECT_THROW(player.RecordPose(0.0f, reference), std::invalid_argument);

    DecodedMotion empty;
    EXPECT_THROW(player.CommitGeneration(empty, CommitParams()), std::invalid_argument);

    DecodedMotion mismatch;
    mismatch = MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f);
    mismatch.footContacts.clear();
    EXPECT_THROW(player.CommitGeneration(std::move(mismatch), CommitParams()),
        std::invalid_argument);

    DecodedMotion tooMany;
    tooMany = MakeMotion(reference, kMaxValidFrames + 1, 0.0f, 0.0f, 0.0f);
    EXPECT_THROW(player.CommitGeneration(std::move(tooMany), CommitParams()),
        std::invalid_argument);

    DecodedMotion valid;
    valid = MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f);
    EXPECT_THROW(player.CommitGeneration(std::move(valid), CommitParams(-1)),
        std::invalid_argument);

    GenerationParams params;
    params.numTargetPoses = 0;
    EXPECT_THROW(player.BuildGenerationInput({reference}, params), std::invalid_argument);
    params.numTargetPoses = 5;
    EXPECT_THROW(player.BuildGenerationInput({reference, reference, reference,
        reference, reference}, params), std::invalid_argument);
    params.numTargetPoses = 2;
    EXPECT_THROW(player.BuildGenerationInput({reference}, params), std::invalid_argument);
    params.transitionEarlyByFrames = -1;
    EXPECT_THROW(player.BuildGenerationInput({reference, reference}, params),
        std::invalid_argument);
}

TEST(MotionBricksPlayer, CommitBaselineIsFrameZero)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer sampled(binding, 8);
    sampled.SeedHistory(MakePose(reference, 100.0f, 0.0f));
    sampled.CommitGeneration(MakeMotion(reference, 18, 1.0f, 0.0f, 0.0f),
        CommitParams());
    PlaybackFrame frame = sampled.Sample(0.0f);
    ExpectRootNear(frame.pose, 0.0f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 0.0f, 1e-5f);
    frame = sampled.Sample(1.0f / 30.0f);
    ExpectRootNear(frame.pose, 1.0f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 1.0f, 1e-5f);

    GeneratedMotionPlayer direct(binding, 8);
    direct.SeedHistory(MakePose(reference, 100.0f, 0.0f));
    direct.CommitGeneration(MakeMotion(reference, 18, 1.0f, 0.0f, 0.0f),
        CommitParams());
    frame = direct.Sample(1.0f / 30.0f);
    ExpectRootNear(frame.pose, 1.0f);
    EXPECT_NEAR(frame.rootMotionDeltaMeters.x(), 1.0f, 1e-5f);
}

TEST(MotionBricksPlayer, StepSamplesThenRecords)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding, 8);
    player.SeedHistory(MakePose(reference, 0.0f, 0.0f));
    player.CommitGeneration(MakeMotion(reference, 18, 1.0f, 0.0f, 0.0f),
        CommitParams());

    const PlaybackFrame frame = player.Step(1.0f / 30.0f);
    ExpectRootNear(frame.pose, 1.0f);
    ExpectRootNear(player.LatestRecordedPose(), 1.0f);
    EXPECT_THROW(player.Step(0.0f), std::invalid_argument);
}

TEST(MotionBricksPlayer, RejectsShortGeneration)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding, 8);
    player.SeedHistory(reference);
    EXPECT_THROW(player.CommitGeneration(MakeMotion(reference, 17, 0.0f, 0.0f, 0.0f),
        CommitParams()), std::invalid_argument);
    EXPECT_NO_THROW(player.CommitGeneration(
        MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f), CommitParams()));
}

TEST(MotionBricksPlayer, RejectsInvalidFootContacts)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    const Pose reference = binding.referenceLocalPose;

    GeneratedMotionPlayer player(binding, 8);
    player.SeedHistory(reference);

    DecodedMotion nanContacts = MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f);
    nanContacts.footContacts[4][0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(player.CommitGeneration(std::move(nanContacts), CommitParams()),
        std::invalid_argument);

    DecodedMotion aboveOne = MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f);
    aboveOne.footContacts[4][1] = 1.01f;
    EXPECT_THROW(player.CommitGeneration(std::move(aboveOne), CommitParams()),
        std::invalid_argument);

    DecodedMotion belowZero = MakeMotion(reference, 18, 0.0f, 0.0f, 0.0f);
    belowZero.footContacts[4][2] = -0.01f;
    EXPECT_THROW(player.CommitGeneration(std::move(belowZero), CommitParams()),
        std::invalid_argument);
}

TEST(MotionBricksPlayer, SeedHistoryRejectsWrongJointCount)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    Pose wrong = binding.referenceLocalPose;
    wrong.pop_back();

    GeneratedMotionPlayer player(binding, 8);
    EXPECT_THROW(player.SeedHistory(wrong), std::invalid_argument);
    EXPECT_THROW(player.RecordPose(1.0f / 30.0f, wrong), std::invalid_argument);
}

TEST(MotionBricksPlayer, FillRejectsShapeChange)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract);
    Pose shorter = binding.referenceLocalPose;
    shorter.pop_back();

    PoseHistory history(4);
    history.Fill(binding.referenceLocalPose);
    EXPECT_THROW(history.Fill(shorter), std::invalid_argument);
    EXPECT_NO_THROW(history.Fill(binding.referenceLocalPose));
}
}
