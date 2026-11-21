#include "animation/motionBricks/motionBricksContract.h"
#include "animation/motionBricks/motionBricksMath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
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

PoseFrames ReadFrames(const json& value)
{
    PoseFrames frames;
    for (const json& frame : value)
        frames.push_back(ReadPose(frame));
    return frames;
}

void ExpectRotationNear(const Eigen::Quaternionf& actual, const Eigen::Quaternionf& expected,
    float tolerance = 1e-5f)
{
    const float dot = std::abs(actual.normalized().dot(expected.normalized()));
    EXPECT_NEAR(dot, 1.0f, tolerance);
}

void ExpectPoseNear(const Pose& actual, const Pose& expected, float positionTolerance = 1e-5f,
    float rotationTolerance = 1e-5f)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t joint = 0; joint < actual.size(); ++joint)
    {
        EXPECT_NEAR(actual[joint].position.x(), expected[joint].position.x(), positionTolerance)
            << "joint " << joint;
        EXPECT_NEAR(actual[joint].position.y(), expected[joint].position.y(), positionTolerance)
            << "joint " << joint;
        EXPECT_NEAR(actual[joint].position.z(), expected[joint].position.z(), positionTolerance)
            << "joint " << joint;
        ExpectRotationNear(actual[joint].rotation, expected[joint].rotation, rotationTolerance);
    }
}

ModelContract LoadContract()
{
    return ParseModelManifest(FixturePath("soma_n1.json"));
}

SkeletonBinding MakeBinding(const json& golden, const ModelContract& contract,
    bool addUnmappedJoint = false)
{
    std::vector<std::string> names = golden["joint_names"].get<std::vector<std::string>>();
    std::vector<int> parents = golden["parents"].get<std::vector<int>>();
    Pose reference = ReadPose(golden["reference"]);
    if (addUnmappedJoint)
    {
        Transform extra;
        extra.position = Eigen::Vector3f(0.25f, 0.5f, 0.75f);
        extra.rotation = Eigen::Quaternionf(Eigen::AngleAxisf(0.3f, Eigen::Vector3f::UnitY()));
        names.push_back("Extra");
        parents.push_back(0);
        reference.push_back(extra);
    }
    return BindSomaSkeleton(contract, std::move(names), std::move(parents),
        std::move(reference));
}

std::array<Pose, kContextPoses> ReadContext(const json& golden)
{
    const json& context = golden["context"];
    if (context.size() != kContextPoses)
        throw std::runtime_error("Fixture context must contain four poses");
    std::array<Pose, kContextPoses> poses;
    for (size_t index = 0; index < poses.size(); ++index)
        poses[index] = ReadPose(context[index]);
    return poses;
}

std::vector<Pose> ReadTarget(const json& golden)
{
    std::vector<Pose> poses;
    for (const json& pose : golden["target"])
        poses.push_back(ReadPose(pose));
    return poses;
}

EncodeConfig ReadConfig(const json& golden)
{
    EncodeConfig config;
    config.allowedTokenCounts = golden["allowed_token_counts"].get<std::vector<int>>();
    const std::vector<float> heading = golden["heading_alignment"].get<std::vector<float>>();
    if (heading.size() != 2)
        throw std::runtime_error("Fixture heading alignment must have 2 values");
    config.headingAlignment = Eigen::Vector2f(heading[0], heading[1]);
    config.modelToRuntime = ReadTransform(golden["model_to_runtime"]);
    return config;
}

std::vector<std::array<float, 366>> ReadPrediction(const json& golden)
{
    const json& features = golden["prediction"]["features"];
    if (features.size() > 64)
        throw std::runtime_error("Fixture prediction has too many rows");
    std::vector<std::array<float, 366>> prediction(64);
    for (auto& row : prediction)
        row.fill(0.0f);
    for (size_t row = 0; row < features.size(); ++row)
    {
        const std::vector<float> values = features[row].get<std::vector<float>>();
        if (values.size() != 366)
            throw std::runtime_error("Fixture prediction row must have 366 values");
        std::copy(values.begin(), values.end(), prediction[row].begin());
    }
    return prediction;
}

struct GoldenFixture
{
    json root;
    ModelContract contract;
    SkeletonBinding binding;
    std::array<Pose, kContextPoses> context;
    std::vector<Pose> target;
    EncodeConfig config;
    std::vector<std::array<float, 366>> prediction;
};

GoldenFixture LoadGolden()
{
    GoldenFixture golden;
    golden.root = LoadJson(FixturePath("soma_golden.json"));
    golden.contract = LoadContract();
    golden.binding = MakeBinding(golden.root, golden.contract);
    golden.context = ReadContext(golden.root);
    golden.target = ReadTarget(golden.root);
    golden.config = ReadConfig(golden.root);
    golden.prediction = ReadPrediction(golden.root);
    return golden;
}

void ExpectEncodedNear(const EncodedInputs& actual, const json& expected)
{
    const Transform expectedAlignment = ReadTransform(expected["alignment"]);
    EXPECT_NEAR(actual.alignment.position.x(), expectedAlignment.position.x(), 1e-5f);
    EXPECT_NEAR(actual.alignment.position.y(), expectedAlignment.position.y(), 1e-5f);
    EXPECT_NEAR(actual.alignment.position.z(), expectedAlignment.position.z(), 1e-5f);
    ExpectRotationNear(actual.alignment.rotation, expectedAlignment.rotation);

    const auto globalRoot = expected["global_root_values"].get<std::vector<std::vector<float>>>();
    const auto localRoot = expected["local_root_values"].get<std::vector<std::vector<float>>>();
    const auto localPoses = expected["local_poses"].get<std::vector<std::vector<float>>>();
    ASSERT_EQ(globalRoot.size(), 8u);
    ASSERT_EQ(localRoot.size(), 8u);
    ASSERT_EQ(localPoses.size(), 8u);
    for (size_t frame = 0; frame < 8; ++frame)
    {
        ASSERT_EQ(globalRoot[frame].size(), 5u);
        ASSERT_EQ(localRoot[frame].size(), 4u);
        ASSERT_EQ(localPoses[frame].size(), 267u);
        for (size_t value = 0; value < 5; ++value)
            EXPECT_NEAR(actual.globalRootValues[frame][value], globalRoot[frame][value], 1e-5f)
                << "global root frame " << frame << " value " << value;
        for (size_t value = 0; value < 4; ++value)
            EXPECT_NEAR(actual.localRootValues[frame][value], localRoot[frame][value], 1e-4f)
                << "local root frame " << frame << " value " << value;
        for (size_t value = 0; value < 267; ++value)
            EXPECT_NEAR(actual.localPoses[frame][value], localPoses[frame][value], 1e-5f)
                << "local pose frame " << frame << " value " << value;
    }

    const auto hasGlobal = expected["has_global_root_values"].get<std::vector<int32_t>>();
    const auto hasLocalRoot = expected["has_local_root_values"].get<std::vector<int32_t>>();
    const auto hasLocalPoses = expected["has_local_poses"].get<std::vector<int32_t>>();
    const auto allowed = expected["allowed_pred_num_tokens"].get<std::vector<int32_t>>();
    ASSERT_EQ(hasGlobal.size(), 8u);
    ASSERT_EQ(hasLocalRoot.size(), 8u);
    ASSERT_EQ(hasLocalPoses.size(), 8u);
    ASSERT_EQ(allowed.size(), 11u);
    for (size_t index = 0; index < 8; ++index)
    {
        EXPECT_EQ(actual.hasGlobalRootValues[index], hasGlobal[index]);
        EXPECT_EQ(actual.hasLocalRootValues[index], hasLocalRoot[index]);
        EXPECT_EQ(actual.hasLocalPoses[index], hasLocalPoses[index]);
    }
    for (size_t index = 0; index < 11; ++index)
        EXPECT_EQ(actual.allowedPredNumTokens[index], allowed[index]);
}

void ExpectMotionNear(const DecodedMotion& actual, const json& expected)
{
    const int frameCount = expected["frame_count"].get<int>();
    const PoseFrames frames = ReadFrames(expected["frames"]);
    const auto contacts = expected["foot_contacts"].get<std::vector<std::vector<float>>>();
    ASSERT_EQ(static_cast<int>(actual.frames.size()), frameCount);
    ASSERT_EQ(frames.size(), actual.frames.size());
    ASSERT_EQ(contacts.size(), actual.footContacts.size());
    for (size_t frame = 0; frame < actual.frames.size(); ++frame)
    {
        ExpectPoseNear(actual.frames[frame], frames[frame]);
        for (size_t contact = 0; contact < 4; ++contact)
            EXPECT_NEAR(actual.footContacts[frame][contact], contacts[frame][contact], 1e-5f)
                << "frame " << frame << " contact " << contact;
    }
}

TEST(MotionBricksContract, ParsesSomaManifestAndRejectsBadTiming)
{
    const ModelContract contract = LoadContract();
    EXPECT_EQ(contract.name, "mmm_v6_soma_fp32");
    EXPECT_EQ(contract.jointNames.size(), 30u);
    EXPECT_EQ(contract.tensorSpecs.size(), 9u);
    EXPECT_NO_THROW(ValidateSomaContract(contract));
    EXPECT_EQ(ResolveModelPath(FixturePath("soma_n1.json"), contract.onnxFilename).filename(),
        std::filesystem::path(contract.onnxFilename));

    EXPECT_EQ(ValidFrameCount(6), 18);
    EXPECT_EQ(ValidFrameCount(16), 58);
    EXPECT_THROW(ValidFrameCount(5), std::invalid_argument);
    EXPECT_THROW(ValidFrameCount(17), std::invalid_argument);

    json bad = LoadJson(FixturePath("soma_n1.json"));
    bad["sample_rate"] = 60.0;
    const auto temp = std::filesystem::temp_directory_path() / "vl_motionbricks_bad_timing.json";
    {
        std::ofstream output(temp);
        output << bad.dump(1);
    }
    EXPECT_THROW(ParseModelManifest(temp), std::invalid_argument);
    std::filesystem::remove(temp);

    json duplicate = LoadJson(FixturePath("soma_n1.json"));
    duplicate["joint_names"][1] = duplicate["joint_names"][0];
    {
        std::ofstream output(temp);
        output << duplicate.dump(1);
    }
    EXPECT_THROW(ParseModelManifest(temp), std::invalid_argument);
    std::filesystem::remove(temp);
}

TEST(MotionBricksMath, BindRejectsMissingJointAndKeepsUnmappedJoints)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));

    std::vector<std::string> names = golden["joint_names"].get<std::vector<std::string>>();
    std::vector<int> parents = golden["parents"].get<std::vector<int>>();
    Pose reference = ReadPose(golden["reference"]);
    names.pop_back();
    parents.pop_back();
    reference.pop_back();
    EXPECT_THROW(BindSomaSkeleton(contract, names, parents, reference), std::invalid_argument);

    const SkeletonBinding binding = MakeBinding(golden, contract, true);
    EXPECT_EQ(binding.jointNames.size(), 31u);
    EXPECT_EQ(binding.poseToModel.back(), -1);
    EXPECT_TRUE(binding.referenceLocalPose.back().position.isApprox(
        Eigen::Vector3f(0.25f, 0.5f, 0.75f)));
}

TEST(MotionBricksMath, EncodedInputsMatchOfficialSomaKernels)
{
    GoldenFixture golden = LoadGolden();
    const EncodedInputs encoded = EncodeSomaInputs(golden.binding, golden.context,
        golden.target, golden.config);
    ExpectEncodedNear(encoded, golden.root["encoded"]);
}

TEST(MotionBricksMath, DecodedFramesMatchOfficialSomaKernels)
{
    GoldenFixture golden = LoadGolden();
    const EncodedInputs encoded = EncodeSomaInputs(golden.binding, golden.context,
        golden.target, golden.config);
    const int tokenCount = golden.root["prediction"]["token_count"].get<int>();
    const DecodedMotion decoded = DecodeSomaOutput(golden.binding, encoded.alignment,
        golden.config.modelToRuntime, golden.prediction, tokenCount);
    ExpectMotionNear(decoded, golden.root["decoded"]);
}

TEST(MotionBricksMath, EndpointCorrectionMatchesOfficialSomaKernels)
{
    GoldenFixture golden = LoadGolden();
    const EncodedInputs encoded = EncodeSomaInputs(golden.binding, golden.context,
        golden.target, golden.config);
    const int tokenCount = golden.root["prediction"]["token_count"].get<int>();
    DecodedMotion decoded = DecodeSomaOutput(golden.binding, encoded.alignment,
        golden.config.modelToRuntime, golden.prediction, tokenCount);
    CorrectEndpoints(golden.binding, golden.context.back(), golden.target.front(),
        decoded.frames);

    const json& corrected = golden.root["corrected"];
    ExpectPoseNear(decoded.frames.front(), ReadPose(corrected["first"]));
    ExpectPoseNear(decoded.frames[decoded.frames.size() / 2], ReadPose(corrected["middle"]));
    ExpectPoseNear(decoded.frames.back(), ReadPose(corrected["last"]));
    ExpectPoseNear(decoded.frames.front(), golden.context.back());
    ExpectPoseNear(decoded.frames.back(), golden.target.front());
}

TEST(MotionBricksMath, SamplingMatchesOfficialSomaKernels)
{
    GoldenFixture golden = LoadGolden();
    const EncodedInputs encoded = EncodeSomaInputs(golden.binding, golden.context,
        golden.target, golden.config);
    const int tokenCount = golden.root["prediction"]["token_count"].get<int>();
    DecodedMotion decoded = DecodeSomaOutput(golden.binding, encoded.alignment,
        golden.config.modelToRuntime, golden.prediction, tokenCount);
    CorrectEndpoints(golden.binding, golden.context.back(), golden.target.front(),
        decoded.frames);

    const json& sampled = golden.root["sampled"];
    const float time = sampled["time"].get<float>();
    ExpectPoseNear(SamplePose(decoded.frames, time), ReadPose(sampled["pose"]));

    const auto contacts = SampleFootContacts(decoded.footContacts, time);
    for (float contact : contacts)
        EXPECT_GE(contact, 0.0f);
}

TEST(MotionBricksMath, UnmappedJointKeepsReferencePose)
{
    const ModelContract contract = LoadContract();
    const json golden = LoadJson(FixturePath("soma_golden.json"));
    const SkeletonBinding binding = MakeBinding(golden, contract, true);
    auto context = ReadContext(golden);
    auto target = ReadTarget(golden);
    for (Pose& pose : context)
        pose.push_back(binding.referenceLocalPose.back());
    for (Pose& pose : target)
        pose.push_back(binding.referenceLocalPose.back());
    const EncodeConfig config = ReadConfig(golden);
    const auto prediction = ReadPrediction(golden);
    const EncodedInputs encoded = EncodeSomaInputs(binding, context, target, config);
    const int tokenCount = golden["prediction"]["token_count"].get<int>();
    const DecodedMotion decoded = DecodeSomaOutput(binding, encoded.alignment,
        config.modelToRuntime, prediction, tokenCount);
    ExpectPoseNear(Pose{decoded.frames[0].back()}, Pose{binding.referenceLocalPose.back()});
}

TEST(MotionBricksMath, RejectsInvalidEncodeAndDecodeInputs)
{
    GoldenFixture golden = LoadGolden();
    EXPECT_THROW(EncodeSomaInputs(golden.binding, golden.context, {}, golden.config),
        std::invalid_argument);

    EncodeConfig badCounts = golden.config;
    badCounts.allowedTokenCounts = {5};
    EXPECT_THROW(EncodeSomaInputs(golden.binding, golden.context, golden.target, badCounts),
        std::invalid_argument);

    EncodeConfig badHeading = golden.config;
    badHeading.headingAlignment = Eigen::Vector2f::Zero();
    EXPECT_THROW(EncodeSomaInputs(golden.binding, golden.context, golden.target, badHeading),
        std::invalid_argument);

    const EncodedInputs encoded = EncodeSomaInputs(golden.binding, golden.context,
        golden.target, golden.config);
    EXPECT_THROW(DecodeSomaOutput(golden.binding, encoded.alignment,
        golden.config.modelToRuntime, golden.prediction, 5), std::invalid_argument);

    std::vector<std::array<float, 366>> zeroPrediction(64);
    for (auto& row : zeroPrediction)
        row.fill(0.0f);
    EXPECT_THROW(DecodeSomaOutput(golden.binding, encoded.alignment,
        golden.config.modelToRuntime, zeroPrediction, 6), std::invalid_argument);
}
}
