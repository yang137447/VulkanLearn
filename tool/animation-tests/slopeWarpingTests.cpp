#include "animation/skeletalPlayback.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

constexpr float Pi = 3.14159265358979323846f;

Eigen::Matrix4f Translation(float x, float y, float z)
{
    Eigen::Matrix4f value = Eigen::Matrix4f::Identity();
    value.block<3, 1>(0, 3) = Eigen::Vector3f(x, y, z);
    return value;
}

struct TestAsset
{
    std::shared_ptr<SkeletalAsset> asset;
    size_t root = 0, pelvis = 0, footLeft = 0, footRight = 0;
};

TestAsset MakeAsset(bool withFeet, bool movingRoot = false)
{
    TestAsset result;
    result.asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    auto& pelvis = raw.roots[0].children.emplace_back();
    pelvis.name = "pelvis";
    pelvis.transform = ozz::math::Transform::identity();
    pelvis.transform.translation = {0, 90, 0};
    if (withFeet)
    {
        auto& left = pelvis.children.emplace_back();
        left.name = "foot_l";
        left.transform = ozz::math::Transform::identity();
        left.transform.translation = {-15, -85, 0};
        auto& right = pelvis.children.emplace_back();
        right.name = "foot_r";
        right.transform = ozz::math::Transform::identity();
        right.transform.translation = {15, -85, 0};
    }
    result.asset->mesh.runtime = ozz::animation::offline::SkeletonBuilder()(raw);
    result.asset->mesh.joints = {
        {"root", -1, Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()},
        {"pelvis", 0, Translation(0, 90, 0), Translation(0, 90, 0)}};
    if (withFeet)
    {
        result.asset->mesh.joints.push_back(
            {"foot_l", 1, Translation(-15, -85, 0), Translation(-15, 5, 0)});
        result.asset->mesh.joints.push_back(
            {"foot_r", 1, Translation(15, -85, 0), Translation(15, 5, 0)});
        result.footLeft = 2;
        result.footRight = 3;
    }
    SkinSection section;
    for (uint32_t joint = 0; joint < result.asset->mesh.joints.size(); ++joint)
    {
        Vertex vertex{};
        vertex.position = result.asset->mesh.joints[joint].restModel.block<3, 1>(0, 3);
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(joint);
        section.influences.push_back({joint, 1});
        section.inverseBinds.push_back(result.asset->mesh.joints[joint].restModel.inverse());
    }
    section.influenceOffsets.push_back(
        static_cast<uint32_t>(result.asset->mesh.joints.size()));
    ComputeJointBindBounds(section);
    result.asset->mesh.sections.push_back(std::move(section));

    Clip clip;
    clip.id = movingRoot ? "moving" : "idle";
    clip.raw.duration = 2;
    clip.raw.tracks.resize(result.asset->mesh.joints.size());
    for (auto& track : clip.raw.tracks)
    {
        track.translations = {{0, {0, 0, 0}}, {2, {0, 0, 0}}};
        track.rotations = {{0, ozz::math::Quaternion::identity()},
            {2, ozz::math::Quaternion::identity()}};
        track.scales = {{0, {1, 1, 1}}, {2, {1, 1, 1}}};
    }
    if (movingRoot)
        clip.raw.tracks[0].translations = {{0, {0, 0, 0}}, {2, {100, 0, 0}}};
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    result.asset->clips.push_back(std::move(clip));
    result.root = 0;
    result.pelvis = 1;
    return result;
}

class FixedGroundProbe final : public GroundProbe
{
public:
    explicit FixedGroundProbe(Eigen::Vector3f normal, Eigen::Vector3f point =
        Eigen::Vector3f::Zero(), std::string support = "slope")
    {
        hit.hit = true;
        hit.point = point;
        hit.normal = std::move(normal);
        hit.supportIdentity = std::move(support);
    }

    FootGroundHit ProbeGround(const std::string&, const Eigen::Vector3f&,
        const Eigen::Vector3f&) const override
    {
        return hit;
    }

    FootGroundHit hit;
};

Eigen::Matrix4f ComponentIdentity()
{
    return Eigen::Matrix4f::Identity();
}

void Settle(SkeletalPlayback& player, const FixedGroundProbe& probe, float deltaSeconds,
    float seconds = 1.5f)
{
    const int frames = static_cast<int>(std::ceil(seconds / deltaSeconds));
    for (int frame = 0; frame < frames; ++frame)
        player.ApplySlopeWarping(ComponentIdentity(), true, "actor", deltaSeconds, probe);
}

float AngleBetween(const Eigen::Vector3f& a, const Eigen::Vector3f& b)
{
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f)) *
        180.0f / Pi;
}

Eigen::Vector3f GroundNormal(float degrees)
{
    const float radians = degrees * Pi / 180.0f;
    return Eigen::Vector3f(std::sin(radians), std::cos(radians), 0);
}

TEST(SlopeWarping, FlatAndAirborneAreExactNoOps)
{
    const auto fixture = MakeAsset(true);
    SkeletalPlayback player(fixture.asset);
    player.SetSlopeWarpingEnabled(true);
    const auto before = player.Models();

    FixedGroundProbe flat(Eigen::Vector3f::UnitY());
    player.ApplySlopeWarping(ComponentIdentity(), true, "actor", 1.0f / 60, flat);
    EXPECT_FALSE(player.SlopeWarping().active);
    EXPECT_EQ(player.SlopeWarping().reason, "flat");
    for (size_t joint = 0; joint < before.size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(before[joint], 0));

    FixedGroundProbe slope(GroundNormal(30));
    player.ApplySlopeWarping(ComponentIdentity(), false, "actor", 1.0f / 60, slope);
    EXPECT_FALSE(player.SlopeWarping().active);
    EXPECT_EQ(player.SlopeWarping().reason, "airborne");
    for (size_t joint = 0; joint < before.size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(before[joint], 0));
}

TEST(SlopeWarping, TwentyThirtyFortyFiveAlignBoundedAndKeepFeetOnPlane)
{
    for (float degrees : {20.0f, 30.0f, 45.0f})
    {
        const auto fixture = MakeAsset(true);
        SkeletalPlayback player(fixture.asset);
        player.SetSlopeWarpingEnabled(true);
        const Eigen::Vector3f normal = GroundNormal(degrees);
        FixedGroundProbe probe(normal);
        Settle(player, probe, 1.0f / 60);

        const auto& state = player.SlopeWarping();
        ASSERT_TRUE(state.active) << degrees;
        EXPECT_NEAR(state.slopeAngleDegrees, degrees, 0.05f);
        EXPECT_NEAR(state.targetSlopeAngleDegrees, degrees, 0.05f);
        EXPECT_NEAR(state.appliedSlopeAngleDegrees, degrees, 0.1f);
        EXPECT_NEAR(state.weight, 1.0f, 0.001f);

        const Eigen::Vector3f up =
            (player.Models()[fixture.root].block<3, 3>(0, 0) * Eigen::Vector3f::UnitY())
                .normalized();
        EXPECT_LT(AngleBetween(up, normal), 0.2f);
        const float lowest = std::min(
            normal.dot(player.Models()[fixture.footLeft].block<3, 1>(0, 3)),
            normal.dot(player.Models()[fixture.footRight].block<3, 1>(0, 3)));
        EXPECT_NEAR(lowest, 0.0f, 0.25f) << degrees;
    }
}

TEST(SlopeWarping, MaximumSlopeClampsAndWeightZeroIsNoOp)
{
    const auto fixture = MakeAsset(true);
    SkeletalPlayback player(fixture.asset);
    player.SetSlopeWarpingEnabled(true);
    SlopeWarpingParameters parameters;
    parameters.maxSlopeDegrees = 45;
    player.SetSlopeWarpingParameters(parameters);
    const Eigen::Vector3f normal = GroundNormal(60);
    FixedGroundProbe probe(normal);
    Settle(player, probe, 1.0f / 60);
    EXPECT_NEAR(player.SlopeWarping().slopeAngleDegrees, 60, 0.1f);
    EXPECT_NEAR(player.SlopeWarping().targetSlopeAngleDegrees, 45, 0.05f);
    EXPECT_NEAR(player.SlopeWarping().appliedSlopeAngleDegrees, 45, 0.2f);
    const Eigen::Vector3f up =
        (player.Models()[fixture.root].block<3, 3>(0, 0) * Eigen::Vector3f::UnitY())
            .normalized();
    EXPECT_NEAR(AngleBetween(up, normal), 15, 0.3f);

    const auto noWeight = MakeAsset(true);
    SkeletalPlayback disabled(noWeight.asset);
    disabled.SetSlopeWarpingEnabled(true);
    SlopeWarpingParameters zero;
    zero.weight = 0;
    disabled.SetSlopeWarpingParameters(zero);
    const auto before = disabled.Models();
    disabled.ApplySlopeWarping(ComponentIdentity(), true, "actor", 1.0f / 60, probe);
    EXPECT_FALSE(disabled.SlopeWarping().active);
    EXPECT_EQ(disabled.SlopeWarping().reason, "weight-zero");
    for (size_t joint = 0; joint < before.size(); ++joint)
        EXPECT_TRUE(disabled.Models()[joint].isApprox(before[joint], 0));
}

TEST(SlopeWarping, DisabledAndMissingBonesAreExactNoOps)
{
    const auto fixture = MakeAsset(true);
    SkeletalPlayback player(fixture.asset);
    const auto base = player.Models();
    player.SetSlopeWarpingEnabled(true);
    FixedGroundProbe probe(GroundNormal(30));
    Settle(player, probe, 1.0f / 60);
    ASSERT_TRUE(player.SlopeWarping().active);
    player.SetSlopeWarpingEnabled(false);
    EXPECT_FALSE(player.SlopeWarping().enabled);
    EXPECT_FALSE(player.SlopeWarping().active);
    for (size_t joint = 0; joint < base.size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(base[joint], 0));
    player.ApplySlopeWarping(ComponentIdentity(), true, "actor", 1.0f / 60, probe);
    for (size_t joint = 0; joint < base.size(); ++joint)
        EXPECT_TRUE(player.Models()[joint].isApprox(base[joint], 0));

    const auto missing = MakeAsset(false);
    SkeletalPlayback broken(missing.asset);
    broken.SetSlopeWarpingEnabled(true);
    const auto missingBefore = broken.Models();
    broken.ApplySlopeWarping(ComponentIdentity(), true, "actor", 1.0f / 60, probe);
    EXPECT_TRUE(broken.SlopeWarping().enabled);
    EXPECT_FALSE(broken.SlopeWarping().active);
    EXPECT_EQ(broken.SlopeWarping().reason, "missing-joint:foot_l");
    for (size_t joint = 0; joint < missingBefore.size(); ++joint)
        EXPECT_TRUE(broken.Models()[joint].isApprox(missingBefore[joint], 0));
}

TEST(SlopeWarping, ThirtySixtyAndOneTwentyHertzStayStable)
{
    float reference = 0;
    for (float hz : {30.0f, 60.0f, 120.0f})
    {
        const auto fixture = MakeAsset(true);
        SkeletalPlayback player(fixture.asset);
        player.SetSlopeWarpingEnabled(true);
        FixedGroundProbe probe(GroundNormal(35));
        Settle(player, probe, 1.0f / hz, 2.0f);
        ASSERT_TRUE(player.SlopeWarping().active) << hz;
        EXPECT_TRUE(player.Models()[fixture.root].allFinite());
        EXPECT_NEAR(player.SlopeWarping().appliedSlopeAngleDegrees, 35, 0.2f) << hz;
        if (reference == 0) reference = player.SlopeWarping().appliedSlopeAngleDegrees;
        else EXPECT_NEAR(player.SlopeWarping().appliedSlopeAngleDegrees, reference, 0.02f);
    }
}

TEST(SlopeWarping, RootMotionAndPlaybackClockAreNotConsumed)
{
    const auto fixture = MakeAsset(true, true);
    SkeletalPlayback player(fixture.asset);
    player.Play("moving", true, 1, false);
    player.SetSlopeWarpingEnabled(true);
    player.Tick(1.0f / 60);
    const Eigen::Vector3f beforeDelta = player.RootMotionDelta();
    const float beforeTime = player.Time();
    FixedGroundProbe probe(GroundNormal(30));
    Settle(player, probe, 1.0f / 60);
    EXPECT_TRUE(player.RootMotionDelta().isApprox(beforeDelta, 0));
    EXPECT_FLOAT_EQ(player.Time(), beforeTime);
    EXPECT_TRUE(player.RootMotionTotal().allFinite());
}
}
