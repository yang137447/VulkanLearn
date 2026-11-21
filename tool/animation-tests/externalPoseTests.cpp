#include "animation/skeletalPlayback.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <gtest/gtest.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

namespace
{
using namespace VL::Animation;

Eigen::Matrix4f Translation(float x, float y, float z)
{
    Eigen::Matrix4f value = Eigen::Matrix4f::Identity();
    value.block<3, 1>(0, 3) = Eigen::Vector3f(x, y, z);
    return value;
}

Eigen::Vector3f T(float x, float y, float z)
{
    return Eigen::Vector3f(x, y, z);
}

Clip MakeClip(const std::string& id)
{
    Clip clip;
    clip.id = id;
    clip.raw.duration = 4;
    clip.raw.tracks.resize(3);
    clip.raw.tracks[0].translations = {{0, {3, 2, 1}}, {4, {3, 2, 1}}};
    clip.raw.tracks[0].rotations = {{0, ozz::math::Quaternion::identity()}};
    clip.raw.tracks[0].scales = {{0, ozz::math::Float3::one()}};
    clip.raw.tracks[1].translations = {{0, {0, 10, 0}}, {4, {0, 10, 0}}};
    clip.raw.tracks[1].rotations = {{0, ozz::math::Quaternion::identity()}};
    clip.raw.tracks[1].scales = {{0, ozz::math::Float3::one()}};
    clip.raw.tracks[2].translations = {{0, {0, 0, 4}}, {4, {0, 0, 4}}};
    clip.raw.tracks[2].rotations = {{0, ozz::math::Quaternion::identity()}};
    clip.raw.tracks[2].scales = {{0, ozz::math::Float3::one()}};
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    return clip;
}

std::shared_ptr<SkeletalAsset> MakeAsset()
{
    auto asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    raw.roots[0].transform.translation = {3, 2, 1};
    raw.roots[0].children.resize(1);
    auto& hip = raw.roots[0].children[0];
    hip.name = "hip";
    hip.transform = ozz::math::Transform::identity();
    hip.transform.translation = {0, 10, 0};
    hip.children.resize(1);
    hip.children[0].name = "foot";
    hip.children[0].transform = ozz::math::Transform::identity();
    hip.children[0].transform.translation = {0, 0, 4};
    ozz::animation::offline::SkeletonBuilder skeletonBuilder;
    asset->mesh.runtime = skeletonBuilder(raw);
    asset->mesh.joints = {
        {"root", -1, Translation(3, 2, 1), Translation(3, 2, 1)},
        {"hip", 0, Translation(0, 10, 0), Translation(3, 12, 1)},
        {"foot", 1, Translation(0, 0, 4), Translation(3, 12, 5)}};
    SkinSection section;
    for (uint32_t joint = 0; joint < 3; ++joint)
    {
        Vertex vertex{};
        vertex.position = asset->mesh.joints[joint].restModel.block<3, 1>(0, 3);
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(joint);
        section.influences.push_back({joint, 1});
        section.inverseBinds.push_back(asset->mesh.joints[joint].restModel.inverse());
    }
    section.influenceOffsets.push_back(3);
    ComputeJointBindBounds(section);
    asset->mesh.sections.push_back(std::move(section));
    asset->clips.push_back(MakeClip("idle"));
    return asset;
}

void SetLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

LocalPose MakeLocals(const std::array<Eigen::Vector3f, 3>& translations)
{
    LocalPose pose(1, ozz::math::SoaTransform::identity());
    for (size_t joint = 0; joint < translations.size(); ++joint)
    {
        SetLane(pose[0].translation.x, joint, translations[joint].x());
        SetLane(pose[0].translation.y, joint, translations[joint].y());
        SetLane(pose[0].translation.z, joint, translations[joint].z());
    }
    return pose;
}

ExternalPoseFrame MakeFrame(const std::array<Eigen::Vector3f, 3>& translations)
{
    ExternalPoseFrame frame;
    frame.locals = MakeLocals(translations);
    return frame;
}

TEST(ExternalPose, ReplacesClipSamplingAndPublishesRootMotion)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("idle", true, 1, true);
    player.Tick(1.0f / 30);
    const auto before = player.Pose(0);
    ASSERT_TRUE(before);
    EXPECT_NEAR(before->matrices[1](0, 3), 0.0f, 1e-3f);
    const float clipTime = player.Time();

    player.SetExternalPoseStreamEnabled(true);
    auto frame = MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)});
    frame.rootMotionDelta = Eigen::Vector3f(1, 0, 2);
    frame.rootMotionYawDelta = 0.25f;
    frame.deltaSeconds = 1.0f / 15;
    player.SubmitExternalPose(std::move(frame));
    player.Tick(1.0f / 30);

    const auto after = player.Pose(0);
    ASSERT_TRUE(after);
    EXPECT_NEAR(after->matrices[1](0, 3), 40.0f, 1e-4f);
    EXPECT_NEAR(after->matrices[1](1, 3), 0.0f, 1e-4f);
    EXPECT_TRUE(player.RootMotionDelta().isApprox(Eigen::Vector3f(1, 0, 2)));
    EXPECT_FLOAT_EQ(player.RootMotionYawDelta(), 0.25f);
    // Root motion is reported with the delta that generated it, not the
    // later consumption tick.
    EXPECT_FLOAT_EQ(player.LastTickDeltaSeconds(), 1.0f / 15);
    EXPECT_FLOAT_EQ(player.Time(), clipTime);
}

TEST(ExternalPose, FreezesWithoutNewFrameAndDoesNotFallBackToClip)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("idle", true, 1, true);
    player.Tick(1.0f / 30);

    player.SetExternalPoseStreamEnabled(true);
    player.SubmitExternalPose(MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)}));
    player.Tick(1.0f / 30);
    const auto sampled = player.Pose(0);
    ASSERT_TRUE(sampled);

    player.Tick(0.5f);
    const auto frozen = player.Pose(0);
    ASSERT_TRUE(frozen);
    EXPECT_NEAR(frozen->matrices[1](0, 3), 40.0f, 1e-4f);
    EXPECT_EQ(frozen->playbackIdentity, sampled->playbackIdentity);
    EXPECT_EQ(frozen->discontinuity, sampled->discontinuity);
    EXPECT_TRUE(player.RootMotionDelta().isZero());
    EXPECT_FLOAT_EQ(player.RootMotionYawDelta(), 0.0f);
}

TEST(ExternalPose, HardCutAdvancesDiscontinuity)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("idle", true, 1, true);
    player.Tick(1.0f / 30);
    player.SetExternalPoseStreamEnabled(true);

    auto first = MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)});
    first.hardCut = true;
    player.SubmitExternalPose(std::move(first));
    player.Tick(1.0f / 30);
    const uint64_t cut = player.Pose(0)->discontinuity;

    player.SubmitExternalPose(MakeFrame({T(3, 2, 1), T(41, 10, 0), T(0, 0, 4)}));
    player.Tick(1.0f / 30);
    EXPECT_EQ(player.Pose(0)->discontinuity, cut);

    auto second = MakeFrame({T(3, 2, 1), T(42, 10, 0), T(0, 0, 4)});
    second.hardCut = true;
    player.SubmitExternalPose(std::move(second));
    player.Tick(1.0f / 30);
    EXPECT_EQ(player.Pose(0)->discontinuity, cut + 1);
}

TEST(ExternalPose, ExplicitPauseDropsSubmittedFrame)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("idle", true, 1, true);
    player.Tick(1.0f / 30);
    player.SetExternalPoseStreamEnabled(true);
    player.SubmitExternalPose(MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)}));
    player.Tick(1.0f / 30);
    const auto beforePause = player.Pose(0);
    ASSERT_TRUE(beforePause);

    player.SetPlaying(false);
    player.SubmitExternalPose(MakeFrame({T(3, 2, 1), T(80, 10, 0), T(0, 0, 4)}));
    player.Tick(1.0f / 30);
    EXPECT_EQ(player.Pose(0), beforePause);

    player.SetPlaying(true);
    player.SubmitExternalPose(MakeFrame({T(3, 2, 1), T(50, 10, 0), T(0, 0, 4)}));
    player.Tick(1.0f / 30);
    EXPECT_NEAR(player.Pose(0)->matrices[1](0, 3), 50.0f, 1e-4f);
}

TEST(ExternalPose, DisableResumesClipSampling)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("idle", true, 1, true);
    player.Tick(1.0f / 30);
    player.SetExternalPoseStreamEnabled(true);
    player.SubmitExternalPose(MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)}));
    player.Tick(1.0f / 30);
    ASSERT_NEAR(player.Pose(0)->matrices[1](0, 3), 40.0f, 1e-4f);

    player.SetExternalPoseStreamEnabled(false);
    player.Tick(1.0f / 30);
    EXPECT_NEAR(player.Pose(0)->matrices[1](0, 3), 0.0f, 1e-3f);
}

TEST(ExternalPose, RejectsSubmitWhenDisabledOrJointCountMismatch)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    auto frame = MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)});
    EXPECT_THROW(player.SubmitExternalPose(frame), std::logic_error);

    player.SetExternalPoseStreamEnabled(true);
    ExternalPoseFrame empty;
    EXPECT_THROW(player.SubmitExternalPose(empty), std::invalid_argument);

    ExternalPoseFrame nonFinite = MakeFrame({T(3, 2, 1), T(40, 10, 0), T(0, 0, 4)});
    nonFinite.rootMotionDelta.x() = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(player.SubmitExternalPose(nonFinite), std::invalid_argument);
}
}
