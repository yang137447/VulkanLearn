#include <gtest/gtest.h>
#include "animation/inertialization.h"
#include "animation/rootOffset.h"
#include "animation/skeletalPlayback.h"
#include "animation/motionRuntime.h"
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <cmath>

namespace
{
using namespace VL::Animation;

JointKinematics Advance(JointKinematics value, float seconds)
{
    value.translation += value.linearVelocity * seconds;
    value.rotation = RotationExp(value.angularVelocity * seconds) * value.rotation;
    value.scale = (value.scale.array().log() + value.logScaleVelocity.array() * seconds).exp().matrix();
    return value;
}

std::shared_ptr<SkeletalAsset> CorrectionAsset()
{
    auto asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    raw.roots[0].children.resize(1);
    raw.roots[0].children[0].name = "hip";
    raw.roots[0].children[0].transform = ozz::math::Transform::identity();
    asset->mesh.runtime = ozz::animation::offline::SkeletonBuilder()(raw);
    asset->mesh.joints.push_back({"root", -1, Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()});
    asset->mesh.joints.push_back({"hip", 0, Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()});
    SkinSection section;
    for (unsigned vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
    {
        Vertex vertex{};
        vertex.position = {float(vertexIndex == 1), float(vertexIndex == 2), 0};
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(vertexIndex);
        section.influences.push_back({1, 1});
    }
    section.influenceOffsets.push_back(3);
    section.inverseBinds = {Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()};
    asset->mesh.sections.push_back(std::move(section));
    for (int member = 0; member < 2; ++member)
    {
        Clip clip;
        clip.id = member == 0 ? "source" : "target";
        clip.raw.duration = 4;
        clip.raw.tracks.resize(2);
        for (auto& track : clip.raw.tracks)
        {
            track.translations = {{0, {0, 0, 0}}, {4, {0, 0, 0}}};
            track.rotations = {{0, ozz::math::Quaternion::identity()}, {4, ozz::math::Quaternion::identity()}};
            track.scales = {{0, {1, 1, 1}}, {4, {1, 1, 1}}};
        }
        clip.raw.tracks[1].translations =
            member == 0 ? decltype(clip.raw.tracks[1].translations){{0, {0, 0, 0}}, {4, {40, 0, 0}}} :
                decltype(clip.raw.tracks[1].translations){{0, {80, 0, 0}}, {4, {0, 0, 0}}};
        clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
        asset->clips.push_back(std::move(clip));
    }
    return asset;
}

Eigen::Matrix4f Component(float x, float yaw)
{
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    result.block<3, 3>(0, 0) = Eigen::AngleAxisf(yaw, Eigen::Vector3f::UnitY()).toRotationMatrix();
    result(0, 3) = x;
    return result;
}

TEST(Inertialization, MovingTranslationAndNoncommutingRotationMatchInitialVelocityAndFiniteEndpoint)
{
    JointKinematics source, target;
    source.translation = {12, -4, 8};
    source.linearVelocity = {8, 3, -5};
    source.rotation = RotationExp({1.1f, -0.8f, 0.4f});
    source.angularVelocity = {0.8f, 0.3f, -0.5f};
    source.scale = {1.3f, 0.8f, 1.1f};
    source.logScaleVelocity = {0.1f, -0.2f, 0.05f};
    target.translation = {-3, 10, -8};
    target.linearVelocity = {-2, 4, 1};
    target.rotation = RotationExp({-0.6f, 0.9f, 0.7f});
    target.angularVelocity = {-0.2f, 0.7f, 0.9f};
    target.scale = {0.9f, 1.1f, 0.7f};
    target.logScaleVelocity = {-0.1f, 0.05f, 0.02f};
    FiniteInertialization correction;
    correction.Begin({source}, {target}, 0.4f);
    const auto start = correction.Evaluate({target}, 0).front();
    EXPECT_TRUE(start.translation.isApprox(source.translation, 0.00001f));
    EXPECT_TRUE(start.rotation.isApprox(source.rotation, 0.00001f));
    EXPECT_TRUE(start.linearVelocity.isApprox(source.linearVelocity, 0.00001f));
    EXPECT_TRUE(start.angularVelocity.isApprox(source.angularVelocity, 0.00001f));
    EXPECT_TRUE(start.logScaleVelocity.isApprox(source.logScaleVelocity, 0.00001f));
    constexpr float step = 0.0001f;
    const auto next = correction.Evaluate({Advance(target, step)}, step).front();
    EXPECT_LT(((next.translation - start.translation) / step - source.linearVelocity).norm(), 0.03f);
    EXPECT_LT((RotationLog(next.rotation * start.rotation.conjugate()) / step -
        source.angularVelocity).norm(), 0.015f);
    const auto endTarget = Advance(target, 0.4f);
    const auto end = correction.Evaluate({endTarget}, 0.4f).front();
    EXPECT_TRUE(end.translation.isApprox(endTarget.translation, 0));
    EXPECT_TRUE(end.rotation.isApprox(endTarget.rotation, 0));
    EXPECT_TRUE(end.linearVelocity.isApprox(endTarget.linearVelocity, 0));
    EXPECT_TRUE(end.angularVelocity.isApprox(endTarget.angularVelocity, 0));
    EXPECT_TRUE(end.scale.isApprox(endTarget.scale, 0));
}

TEST(Inertialization, RepeatedInterruptionsPreserveCurrentPoseAndAngularVelocity)
{
    JointKinematics source, target;
    source.rotation = RotationExp({0.8f, -0.4f, 0.3f});
    source.angularVelocity = {0.2f, 0.7f, -0.5f};
    FiniteInertialization correction;
    for (int interruption = 0; interruption < 32; ++interruption)
    {
        target.translation = {float(interruption % 2 ? 15 : -8), 2, -3};
        target.rotation = RotationExp({0.1f, float(interruption % 2 ? 0.9f : -1.2f), 0.4f});
        target.angularVelocity = {0.3f, -0.6f, 0.7f};
        correction.Begin({source}, {target}, 0.18f);
        const auto begin = correction.Evaluate({target}, 0).front();
        EXPECT_TRUE(begin.translation.isApprox(source.translation, 0.0001f));
        EXPECT_LT(RotationLog(begin.rotation * source.rotation.conjugate()).norm(), 0.00001f);
        EXPECT_TRUE(begin.angularVelocity.isApprox(source.angularVelocity, 0.0001f));
        source = correction.Evaluate({Advance(target, 0.02f)}, 0.02f).front();
        EXPECT_TRUE(source.translation.allFinite());
        EXPECT_TRUE(source.angularVelocity.allFinite());
    }
}

TEST(Inertialization, PlaybackRetainsSingleClockPauseFiniteCompletionAndImmutablePose)
{
    const auto asset = CorrectionAsset();
    SkeletalPlayback player(asset), target(asset);
    player.Play("source", true, 1, true);
    EXPECT_FALSE(player.InertializationEnabled());
    player.Tick(0.5f);
    const auto old = player.Pose(0);
    const auto oldMatrix = old->matrices[1];
    player.SetInertializationEnabled(true);
    EXPECT_EQ(player.Pose(0), old);
    player.TransitionTo("target", 0.3f, true, 0.4f);
    EXPECT_EQ(player.Pose(0), old);
    player.Tick(0.0001f);
    EXPECT_NEAR((player.Models()[1](0, 3) - oldMatrix(0, 3)) / 0.0001f, 10, 0.1f);
    player.Tick(0.0999f);
    const auto before = player.Pose(0);
    player.Tick(0);
    player.SetPlaying(false);
    player.Tick(0.2f);
    EXPECT_EQ(player.Pose(0), before);
    player.SetPlaying(true);
    player.SetRate(2);
    EXPECT_EQ(player.Pose(0), before);
    player.Tick(0.3f);
    EXPECT_NEAR(player.Time(), 1, 0.00001f);
    EXPECT_FALSE(player.IsBlending());
    target.Play("target", true, 1, true);
    target.Seek(1);
    EXPECT_TRUE(player.Models()[1].isApprox(target.Models()[1], 0.0001f));
    EXPECT_TRUE(old->matrices[1].isApprox(oldMatrix, 0));
    EXPECT_TRUE(player.MotionModels()[0].isIdentity(0.0001f));
    player.Play("source", true, 1, true);
    EXPECT_FALSE(player.InertializationEnabled());
}

TEST(VisualRootOffset, ActualStopAndHeadingVelocityChangesCreateBoundedLagAndRelease)
{
    RootOffsetSolver solver;
    solver.SetEnabled(true);
    constexpr float dt = 1.0f / 60;
    solver.Update(Component(0, 0), {100, 0, 0}, dt);
    EXPECT_FALSE(solver.State().active);
    solver.Update(Component(100 * dt, 0), {100, 0, 0}, dt);
    EXPECT_FLOAT_EQ(solver.State().worldTranslation.norm(), 0);
    solver.Update(Component(100 * dt, 0.1f), {0, 0, 0}, dt);
    EXPECT_GT(solver.State().worldTranslation.x() / dt, 50);
    EXPECT_LT(solver.State().yawRadians, 0);
    EXPECT_LE(solver.State().worldTranslation.norm(), 20);
    EXPECT_LE(std::abs(solver.State().yawRadians), 3.14159265358979323846f / 4);
    const float visibleYaw = 0.1f + solver.State().yawRadians;
    EXPECT_GT(visibleYaw, 0);
    EXPECT_LT(visibleYaw, 0.05f);
    for (int frame = 0; frame < 120; ++frame)
        solver.Update(Component(100 * dt, 0.1f), Eigen::Vector3f::Zero(), dt);
    EXPECT_LT(solver.State().worldTranslation.norm(), 0.001f);
    EXPECT_LT(std::abs(solver.State().yawRadians), 0.00001f);
    solver.Update(Component(300, 0), Eigen::Vector3f::Zero(), dt);
    EXPECT_EQ(solver.State().reason, "teleported");
    EXPECT_FALSE(solver.State().active);
    EXPECT_TRUE(solver.State().worldTranslation.isZero(0));
    solver.Update(Component(0, 0), Eigen::Vector3f::Zero(), dt);
    solver.Update(Component(0, 0.9f), {5000, 0, 0}, dt);
    EXPECT_EQ(solver.State().reason, "saturated");
    EXPECT_LE(solver.State().worldTranslation.norm(), 20.00001f);
    solver.Update(Component(0, 1.8f), {5000, 0, 0}, dt);
    EXPECT_LE(std::abs(solver.State().yawRadians), 3.14159265358979323846f / 4);
    solver.Update(Component(0, 1.8f), {5000, 0, 0}, 0.2f);
    EXPECT_EQ(solver.State().reason, "large-delta");
    EXPECT_FALSE(solver.State().active);
}

TEST(VisualRootOffset, PlaybackSeparatesMotionHistoryAndFreezesOldSnapshotsAndPausedRoot)
{
    const auto asset = CorrectionAsset();
    SkeletalPlayback player(asset), control(asset);
    player.Play("source", true, 1, true);
    control.Play("source", true, 1, true);
    player.SetRootOffsetEnabled(true);
    constexpr float dt = 1.0f / 60;
    auto component = Component(0, 0);
    for (int frame = 0; frame < 4; ++frame)
    {
        player.Tick(dt); control.Tick(dt);
        const Eigen::Vector3f velocity = frame < 2 ? Eigen::Vector3f(100, 0, 0) : Eigen::Vector3f::Zero();
        component = Component(frame == 0 ? 0 : 100 * dt, frame < 2 ? 0 : 0.1f);
        player.ApplyVisualRoot(component, velocity, dt);
        const auto observed = ObserveMotion(player, component, {1}, frame * dt);
        const auto expected = ObserveMotion(control, component, {1}, frame * dt);
        EXPECT_TRUE(observed.root.isApprox(expected.root, 0));
        EXPECT_TRUE(observed.joints.front().isApprox(expected.joints.front(), 0));
    }
    ASSERT_TRUE(player.RootOffset().active);
    EXPECT_GT((player.PreIkModels()[1] - player.MotionModels()[1]).norm(), 0.1f);
    const auto old = player.Pose(0);
    const auto frozenMatrix = old->matrices[1];
    const auto frozenOffset = player.RootOffset().worldTranslation;
    player.SetPlaying(false);
    player.ApplyVisualRoot(Component(20, 0.3f), Eigen::Vector3f::Zero(), dt);
    EXPECT_EQ(player.Pose(0), old);
    EXPECT_TRUE(player.RootOffset().worldTranslation.isApprox(frozenOffset, 0));
    player.SetPlaying(true);
    player.SetRate(2);
    EXPECT_EQ(player.Pose(0), old);
    const float beforeTime = player.Time();
    player.SetRootOffsetEnabled(false);
    EXPECT_FLOAT_EQ(player.Time(), beforeTime);
    EXPECT_TRUE(player.PreIkModels()[1].isApprox(player.MotionModels()[1], 0));
    EXPECT_TRUE(old->matrices[1].isApprox(frozenMatrix, 0));
    player.SetRootOffsetEnabled(true);
    player.Seek(0.5f);
    EXPECT_FALSE(player.RootOffset().active);
    EXPECT_TRUE(player.RootOffsetEnabled());
    player.Play("source", true, 1, true);
    EXPECT_FALSE(player.RootOffsetEnabled());
}
}
