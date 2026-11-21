#include "animation/motionBricks/motionBricksBakedRuntime.h"
#include "animation/motionBricks/motionBricksSkeleton.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
using namespace VL::Animation;
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

json MakeTargetSkeleton()
{
    struct Entry
    {
        const char* name;
        int parent;
        float z;
    };
    const std::array<Entry, 24> entries = {{
        {"root", -1, 0},
        {"pelvis", 0, 0},
        {"spine_01", 1, 10},
        {"spine_03", 2, 10},
        {"spine_05", 3, 10},
        {"neck_01", 4, 10},
        {"neck_02", 5, 5},
        {"head", 6, 5},
        {"clavicle_l", 4, 5},
        {"upperarm_l", 8, 10},
        {"lowerarm_l", 9, 10},
        {"hand_l", 10, 5},
        {"clavicle_r", 4, 5},
        {"upperarm_r", 12, 10},
        {"lowerarm_r", 13, 10},
        {"hand_r", 14, 5},
        {"thigh_l", 1, 10},
        {"calf_l", 16, 10},
        {"foot_l", 17, 5},
        {"ball_l", 18, 5},
        {"thigh_r", 1, 10},
        {"calf_r", 20, 10},
        {"foot_r", 21, 5},
        {"ball_r", 22, 5},
    }};
    json joints = json::array();
    for (const Entry& entry : entries)
    {
        std::array<float, 16> rest = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, entry.z,
            0, 0, 0, 1,
        };
        joints.push_back({{"name", entry.name}, {"parent", entry.parent},
            {"restLocal", rest}});
    }
    return {
        {"format", "vl.skeleton.v1"},
        {"skeleton", "SK_UEFN_Mannequin"},
        {"root", "root"},
        {"joints", joints},
        {"slots", json::array({"Default"})},
        {"curves", json::object()},
    };
}

SkeletonMesh MakeTargetMesh()
{
    const json json = MakeTargetSkeleton();
    const SkeletonAsset asset = ParseSkeleton(json, "test-target");
    return BuildSkeletonMesh(asset);
}

float LaneValue(const ozz::math::SimdFloat4& vector, size_t lane)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    return values[lane];
}

TEST(MotionBricksRuntime, LoadsSomaSkeletonAndBindsThirtyModelJoints)
{
    const auto source = LoadSomaSkeleton(FixturePath("SK_soma.json"),
        FixturePath("soma_n1.json"));
    ASSERT_TRUE(source);
    EXPECT_EQ(source->mesh.joints.size(), 77u);
    EXPECT_EQ(source->binding.jointNames.size(), 77u);
    EXPECT_EQ(source->binding.modelToPose.size(), 30u);
    EXPECT_EQ(source->binding.modelToPose[0], 0);
    EXPECT_EQ(source->skeleton.root, "Hips");
    EXPECT_FALSE(source->mesh.runtime == nullptr);
    EXPECT_TRUE(source->mesh.sections.empty());
}

TEST(MotionBricksRuntime, RetargetProfileBindsAndAppliesToTarget)
{
    const auto source = LoadSomaSkeleton(FixturePath("SK_soma.json"),
        FixturePath("soma_n1.json"));
    SkeletonMesh target = MakeTargetMesh();
    const json profileJson = LoadJson(FixturePath("RT_UEFN_From_SOMA.json"));
    const RetargetProfile profile = ParseRetargetProfile(profileJson, "fixture");
    CompiledRetarget retarget(source->mesh, target, profile);

    BakedMotion baked = LoadBakedMotion(FixturePath("soma_probe_motion.json"));
    const DecodedMotion decoded = DecodeSomaOutput(source->binding, baked.alignment,
        Transform{}, baked.prediction, baked.tokenCount);
    ASSERT_GE(decoded.frames.size(), 18u);
    const LocalPose sourceLocals = PoseToLocalPoseCm(decoded.frames.front());
    const LocalPose targetLocals = retarget.Apply(sourceLocals);
    EXPECT_EQ(targetLocals.size(), (target.joints.size() + 3) / 4);
    EXPECT_TRUE(std::isfinite(LaneValue(targetLocals[0].translation.x, 0)));
}

TEST(MotionBricksRuntime, BakedRuntimeProducesInPlaceExternalPose)
{
    const auto source = LoadSomaSkeleton(FixturePath("SK_soma.json"),
        FixturePath("soma_n1.json"));
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = ParseRetargetProfile(
        LoadJson(FixturePath("RT_UEFN_From_SOMA.json")), "fixture");
    BakedMotionRuntime runtime(source, target, profile,
        LoadBakedMotion(FixturePath("soma_probe_motion.json")));

    for (int frame = 0; frame < 10; ++frame)
    {
        const ExternalPoseFrame pose = runtime.Tick(1.0f / 60.0f);
        EXPECT_EQ(pose.locals.size(), (target.joints.size() + 3) / 4);
        EXPECT_TRUE(pose.rootMotionDelta.allFinite());
        EXPECT_TRUE(std::isfinite(pose.rootMotionYawDelta));
        EXPECT_FLOAT_EQ(pose.deltaSeconds, 1.0f / 60.0f);
        // The probe submits an in-place target root.
        EXPECT_NEAR(LaneValue(pose.locals[0].translation.x, 0), 0.0f, 1e-5f);
        EXPECT_NEAR(LaneValue(pose.locals[0].translation.y, 0), 0.0f, 1e-5f);
        EXPECT_NEAR(LaneValue(pose.locals[0].translation.z, 0), 0.0f, 1e-5f);
    }
    EXPECT_GT(runtime.PlaybackTime(), 0.0f);
}

TEST(MotionBricksRuntime, BakedRuntimeLoopsAndRejectsNegativeDelta)
{
    const auto source = LoadSomaSkeleton(FixturePath("SK_soma.json"),
        FixturePath("soma_n1.json"));
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = ParseRetargetProfile(
        LoadJson(FixturePath("RT_UEFN_From_SOMA.json")), "fixture");
    BakedMotionRuntime runtime(source, target, profile,
        LoadBakedMotion(FixturePath("soma_probe_motion.json")));
    EXPECT_THROW(runtime.Tick(-0.01f), std::invalid_argument);
    const ExternalPoseFrame looped = runtime.Tick(10.0f);
    EXPECT_EQ(looped.locals.size(), (target.joints.size() + 3) / 4);
    EXPECT_TRUE(std::isfinite(looped.sourceTime));
}

TEST(MotionBricksRuntime, RejectsMalformedBakedMotion)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vl_baked_motion_bad.json";
    {
        std::ofstream output(path);
        output << R"({"token_count":5,"alignment":[0,0,0,0,0,0,1],"pred_global_motions":[]})";
    }
    EXPECT_THROW(LoadBakedMotion(path), std::invalid_argument);
    std::filesystem::remove(path);
}
}
