#include "animation/animationSyncGroup.h"
#include "animation/skeletalPlayback.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

SyncGroupNode Node(std::string_view identity, std::string_view group,
    std::string_view localKey, std::string_view ownerKey, SyncRole role,
    SyncMethod method = SyncMethod::SyncGroup,
    SyncGroupScope scope = SyncGroupScope::Local, float weight = 1,
    bool transitioning = false, float time = 0, float duration = 2,
    float rate = 1, bool looping = true,
    const std::vector<SyncMarker>* markers = nullptr)
{
    SyncGroupNode node;
    node.identity = identity;
    node.group = group;
    node.localKey = localKey;
    node.ownerKey = ownerKey;
    node.role = role;
    node.method = method;
    node.scope = scope;
    node.weight = weight;
    node.transitioning = transitioning;
    node.time = time;
    node.rangeStart = 0;
    node.rangeEnd = duration;
    node.rate = rate;
    node.looping = looping;
    node.markers = markers;
    return node;
}

std::shared_ptr<SkeletalAsset> MakeAsset(float walkDuration = 2.0f,
    float jogDuration = 4.0f)
{
    auto asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    ozz::animation::offline::SkeletonBuilder skeletonBuilder;
    asset->mesh.runtime = skeletonBuilder(raw);
    asset->mesh.joints = {{"root", -1, Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()}};
    SkinSection section;
    Vertex vertex{};
    vertex.position = Eigen::Vector3f::Zero();
    section.vertices.push_back(vertex);
    section.influenceOffsets = {0, 1};
    section.influences = {{0, 1}};
    section.inverseBinds = {Eigen::Matrix4f::Identity()};
    asset->mesh.sections.push_back(std::move(section));

    auto addClip = [&](const char* id, float duration, SyncRole role,
        std::vector<SyncMarker> markers) {
        Clip clip;
        clip.id = id;
        clip.raw.duration = duration;
        clip.raw.tracks.resize(1);
        clip.raw.tracks[0].translations = {{0, {0, 0, 0}}, {duration, {0, 0, 100}}};
        clip.raw.tracks[0].rotations = {{0, ozz::math::Quaternion::identity()}};
        clip.raw.tracks[0].scales = {{0, ozz::math::Float3::one()}};
        ozz::animation::offline::AnimationBuilder builder;
        clip.runtime = builder(clip.raw);
        clip.sequence.id = id;
        clip.sequence.duration = duration;
        clip.sequence.syncMarkers = std::move(markers);
        clip.sequence.players = {{
            "Locomotion", role, SyncMethod::SyncGroup, SyncGroupScope::Local, 0, true}};
        asset->clips.push_back(std::move(clip));
    };
    addClip("walk", walkDuration, SyncRole::AlwaysFollower,
        {{"Foot_L", 0.25f}, {"Foot_R", 0.75f}, {"Foot_L", 1.25f}, {"Foot_R", 1.75f}});
    addClip("jog", jogDuration, SyncRole::CanBeLeader,
        {{"Foot_L", 0.5f}, {"Foot_R", 1.5f}, {"Foot_L", 2.5f}, {"Foot_R", 3.5f}});
    return asset;
}

TEST(AnimationSyncGroup, RoleMatrixElectsAlwaysLeaderOverCanBeLeader)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("follower", "Locomotion", "a", "actor", SyncRole::AlwaysFollower));
    registry.AddNode(Node("can", "Locomotion", "a", "actor", SyncRole::CanBeLeader,
        SyncMethod::SyncGroup, SyncGroupScope::Local, 0.9f));
    registry.AddNode(Node("always", "Locomotion", "a", "actor", SyncRole::AlwaysLeader));
    registry.Resolve();

    EXPECT_TRUE(registry.Results()[2].leader);
    EXPECT_FALSE(registry.Results()[2].follower);
    EXPECT_TRUE(registry.Results()[0].follower);
    EXPECT_TRUE(registry.Results()[1].follower);
    EXPECT_EQ(registry.Results()[0].leaderIndex, 2);
}

TEST(AnimationSyncGroup, CanBeLeaderUsesHighestActiveWeight)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("light", "Locomotion", "a", "actor", SyncRole::CanBeLeader,
        SyncMethod::SyncGroup, SyncGroupScope::Local, 0.2f));
    registry.AddNode(Node("heavy", "Locomotion", "a", "actor", SyncRole::CanBeLeader,
        SyncMethod::SyncGroup, SyncGroupScope::Local, 0.8f));
    registry.Resolve();

    EXPECT_TRUE(registry.Results()[1].leader);
    EXPECT_TRUE(registry.Results()[0].follower);
}

TEST(AnimationSyncGroup, ExclusiveAlwaysLeaderRejectsSecondExclusive)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("first", "Locomotion", "a", "actor", SyncRole::ExclusiveAlwaysLeader));
    registry.AddNode(Node("second", "Locomotion", "a", "actor", SyncRole::ExclusiveAlwaysLeader));
    registry.Resolve();

    EXPECT_TRUE(registry.Results()[0].leader);
    EXPECT_TRUE(registry.Results()[1].doNotSync);
    EXPECT_EQ(registry.Results()[1].diagnostic, SyncGroupDiagnostic::ExclusiveLeaderRejected);
}

TEST(AnimationSyncGroup, TransitionLeaderLeadsOnlyWhileBlendingThenBecomesFollower)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("transition", "Locomotion", "a", "actor",
        SyncRole::TransitionLeader, SyncMethod::SyncGroup, SyncGroupScope::Local, 1, true));
    registry.AddNode(Node("target", "Locomotion", "a", "actor", SyncRole::CanBeLeader));
    registry.Resolve();
    EXPECT_TRUE(registry.Results()[0].leader);
    EXPECT_TRUE(registry.Results()[1].follower);

    registry.BeginTick();
    registry.AddNode(Node("transition", "Locomotion", "a", "actor",
        SyncRole::TransitionLeader, SyncMethod::SyncGroup, SyncGroupScope::Local, 1, false));
    registry.AddNode(Node("target", "Locomotion", "a", "actor", SyncRole::CanBeLeader));
    registry.Resolve();
    EXPECT_FALSE(registry.Results()[0].leader);
    EXPECT_TRUE(registry.Results()[0].follower);
    EXPECT_TRUE(registry.Results()[1].leader);
}

TEST(AnimationSyncGroup, TransitionFollowerIsExcludedOnlyDuringBlend)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("leader", "Locomotion", "a", "actor", SyncRole::AlwaysLeader));
    registry.AddNode(Node("transition", "Locomotion", "a", "actor",
        SyncRole::TransitionFollower, SyncMethod::SyncGroup, SyncGroupScope::Local, 1, true));
    registry.Resolve();
    EXPECT_TRUE(registry.Results()[1].doNotSync);
    EXPECT_EQ(registry.Results()[1].diagnostic, SyncGroupDiagnostic::TransitionFollowerInactive);

    registry.BeginTick();
    registry.AddNode(Node("leader", "Locomotion", "a", "actor", SyncRole::AlwaysLeader));
    registry.AddNode(Node("transition", "Locomotion", "a", "actor",
        SyncRole::TransitionFollower, SyncMethod::SyncGroup, SyncGroupScope::Local, 1, false));
    registry.Resolve();
    EXPECT_TRUE(registry.Results()[1].follower);
    EXPECT_TRUE(registry.Results()[1].hasFollow);
}

TEST(AnimationSyncGroup, FollowerWithoutLeaderIsDiagnosedNotSilentlyDrifting)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("follower", "Locomotion", "a", "actor", SyncRole::AlwaysFollower));
    registry.Resolve();

    EXPECT_TRUE(registry.Results()[0].follower);
    EXPECT_FALSE(registry.Results()[0].hasFollow);
    EXPECT_EQ(registry.Results()[0].diagnostic, SyncGroupDiagnostic::NoLeader);
}

TEST(AnimationSyncGroup, GraphMethodIsRetainedButNotExecuted)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("graph", "Locomotion", "a", "actor", SyncRole::CanBeLeader,
        SyncMethod::Graph));
    registry.Resolve();

    EXPECT_TRUE(registry.Results()[0].doNotSync);
    EXPECT_EQ(registry.Results()[0].authoredMethod, SyncMethod::Graph);
    EXPECT_EQ(registry.Results()[0].diagnostic, SyncGroupDiagnostic::GraphNotExecuted);
}

TEST(AnimationSyncGroup, LocalAndComponentScopesPartitionGroupsDifferently)
{
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("local-a", "Locomotion", "mesh-a", "actor",
        SyncRole::AlwaysLeader, SyncMethod::SyncGroup, SyncGroupScope::Local));
    registry.AddNode(Node("local-b", "Locomotion", "mesh-b", "actor",
        SyncRole::AlwaysLeader, SyncMethod::SyncGroup, SyncGroupScope::Local));
    registry.Resolve();
    EXPECT_EQ(registry.Groups().size(), 2u);
    EXPECT_TRUE(registry.Results()[0].leader);
    EXPECT_TRUE(registry.Results()[1].leader);

    registry.BeginTick();
    registry.AddNode(Node("component-a", "Locomotion", "mesh-a", "actor",
        SyncRole::AlwaysLeader, SyncMethod::SyncGroup, SyncGroupScope::Component));
    registry.AddNode(Node("component-b", "Locomotion", "mesh-b", "actor",
        SyncRole::CanBeLeader, SyncMethod::SyncGroup, SyncGroupScope::Component));
    registry.Resolve();
    EXPECT_EQ(registry.Groups().size(), 1u);
    EXPECT_TRUE(registry.Results()[0].leader);
    EXPECT_TRUE(registry.Results()[1].follower);
}

TEST(AnimationSyncGroup, MarkerSyncAlignsByRecentSameNameInterval)
{
    const std::vector<SyncMarker> leaderMarkers = {
        {"Foot_L", 0.5f}, {"Foot_R", 1.5f}, {"Foot_L", 2.5f}};
    const std::vector<SyncMarker> followerMarkers = {
        {"Foot_L", 1.0f}, {"Foot_R", 2.0f}, {"Foot_L", 3.0f}};
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("leader", "Locomotion", "a", "actor", SyncRole::AlwaysLeader,
        SyncMethod::SyncGroup, SyncGroupScope::Local, 1, false, 1.0f, 4.0f, 1.0f,
        true, &leaderMarkers));
    registry.AddNode(Node("follower", "Locomotion", "a", "actor", SyncRole::AlwaysFollower,
        SyncMethod::MarkerSync, SyncGroupScope::Local, 1, false, 1.2f, 4.0f, 1.0f,
        true, &followerMarkers));
    registry.Resolve();

    // leader 处于 Foot_L [0.5,2.5] 的 alpha=0.25；follower 同名区间 [1.0,3.0]。
    EXPECT_TRUE(registry.Results()[1].follower);
    EXPECT_EQ(registry.Results()[1].effectiveMethod, SyncMethod::MarkerSync);
    EXPECT_FLOAT_EQ(registry.Results()[1].followTime, 1.5f);
}

TEST(AnimationSyncGroup, MarkerSyncFallsBackToSyncGroupWhenFollowerMarkerMissing)
{
    const std::vector<SyncMarker> leaderMarkers = {{"Foot_L", 0.5f}, {"Foot_L", 2.5f}};
    const std::vector<SyncMarker> followerMarkers = {{"Foot_R", 1.0f}, {"Foot_R", 2.0f}};
    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(Node("leader", "Locomotion", "a", "actor", SyncRole::AlwaysLeader,
        SyncMethod::SyncGroup, SyncGroupScope::Local, 1, false, 1.0f, 4.0f, 1.0f,
        true, &leaderMarkers));
    registry.AddNode(Node("follower", "Locomotion", "a", "actor", SyncRole::AlwaysFollower,
        SyncMethod::MarkerSync, SyncGroupScope::Local, 1, false, 1.0f, 4.0f, 1.0f,
        true, &followerMarkers));
    registry.Resolve();

    EXPECT_EQ(registry.Results()[1].effectiveMethod, SyncMethod::SyncGroup);
    EXPECT_EQ(registry.Results()[1].diagnostic, SyncGroupDiagnostic::FollowerMarkerMissing);
    EXPECT_TRUE(registry.Results()[1].hasFollow);
    EXPECT_FLOAT_EQ(registry.Results()[1].followTime, 1.0f);

    registry.BeginTick();
    registry.AddNode(Node("leader", "Locomotion", "a", "actor", SyncRole::AlwaysLeader,
        SyncMethod::SyncGroup, SyncGroupScope::Local, 1, false, 1.0f, 4.0f, 1.0f, true, nullptr));
    registry.AddNode(Node("follower", "Locomotion", "a", "actor", SyncRole::AlwaysFollower,
        SyncMethod::MarkerSync, SyncGroupScope::Local, 1, false, 1.0f, 4.0f, 1.0f,
        true, &followerMarkers));
    registry.Resolve();
    EXPECT_EQ(registry.Results()[1].diagnostic, SyncGroupDiagnostic::LeaderMarkerMissing);
    EXPECT_EQ(registry.Results()[1].effectiveMethod, SyncMethod::SyncGroup);
}

TEST(AnimationSyncGroup, MarkerIntervalWrapsAcrossLoopBoundary)
{
    const std::vector<SyncMarker> markers = {{"Foot_L", 0.5f}, {"Foot_R", 1.5f}};
    float previous = 0;
    float next = 0;
    ASSERT_TRUE(AnimationSyncGroupRegistry::MarkerInterval(
        markers, "Foot_L", 3.2f, 0, 4.0f, true, previous, next));
    EXPECT_FLOAT_EQ(previous, 0.5f);
    EXPECT_FLOAT_EQ(next, 4.5f);

    ASSERT_TRUE(AnimationSyncGroupRegistry::MarkerInterval(
        markers, "Foot_L", 0.1f, 0, 4.0f, true, previous, next));
    EXPECT_FLOAT_EQ(previous, -3.5f);
    EXPECT_FLOAT_EQ(next, 0.5f);
}

TEST(AnimationSyncGroup, FollowerRateUsesDurationRatioAndClamps)
{
    bool clamped = false;
    EXPECT_FLOAT_EQ(AnimationSyncGroupRegistry::FollowerRate(1.0f, 2.0f, 4.0f, clamped), 2.0f);
    EXPECT_FALSE(clamped);
    EXPECT_FLOAT_EQ(AnimationSyncGroupRegistry::FollowerRate(4.0f, 1.0f, 100.0f, clamped), 4.0f);
    EXPECT_TRUE(clamped);
}

TEST(AnimationSyncGroup, ContinuousAlignmentKeepsPhaseErrorWithinOneFrame)
{
    for (float hz : {30.0f, 60.0f, 120.0f})
    {
        const float dt = 1.0f / hz;
        float leaderTime = 0.0f;
        float followerTime = 0.9f;
        AnimationSyncGroupRegistry registry;
        for (int frame = 0; frame < 240; ++frame)
        {
            registry.BeginTick();
            registry.AddNode(Node("leader", "Locomotion", "a", "actor",
                SyncRole::AlwaysLeader, SyncMethod::SyncGroup, SyncGroupScope::Local,
                1, false, leaderTime, 2.0f, 1.0f));
            registry.AddNode(Node("follower", "Locomotion", "a", "actor",
                SyncRole::AlwaysFollower, SyncMethod::SyncGroup, SyncGroupScope::Local,
                1, false, followerTime, 4.0f, 1.0f));
            registry.Resolve();
            ASSERT_TRUE(registry.Results()[1].hasFollow);
            followerTime = registry.Results()[1].followTime;
            leaderTime += dt;
            followerTime += dt * registry.Results()[1].followRate;

            const float leaderPhase = std::fmod(leaderTime, 2.0f) / 2.0f;
            const float followerPhase = std::fmod(followerTime, 4.0f) / 4.0f;
            float error = std::abs(leaderPhase - followerPhase);
            error = std::min(error, 1.0f - error);
            // 对齐发生在 tick 前，剩余误差不超过一帧的归一化时间。
            EXPECT_LE(error * 4.0f, dt + 1e-5f) << "hz=" << hz << " frame=" << frame;
        }
    }
}

TEST(AnimationSyncGroup, RegistryDoesNotGrowPersistentContainersPerTick)
{
    AnimationSyncGroupRegistry registry;
    auto tick = [&]() {
        registry.BeginTick();
        registry.AddNode(Node("leader", "Locomotion", "a", "actor", SyncRole::AlwaysLeader));
        registry.AddNode(Node("follower", "Locomotion", "a", "actor", SyncRole::AlwaysFollower));
        registry.Resolve();
    };
    tick();
    const size_t nodeCapacity = registry.NodeCapacity();
    const size_t resultCapacity = registry.ResultCapacity();
    const size_t groupCapacity = registry.GroupCapacity();
    for (int frame = 0; frame < 1000; ++frame) tick();
    EXPECT_EQ(registry.NodeCapacity(), nodeCapacity);
    EXPECT_EQ(registry.ResultCapacity(), resultCapacity);
    EXPECT_EQ(registry.GroupCapacity(), groupCapacity);
}

TEST(AnimationSyncGroup, PlaybackBuildsSourceClockAndContinuouslyFollows)
{
    auto asset = MakeAsset();
    SkeletalPlayback leader(asset);
    SkeletalPlayback follower(asset);
    leader.Play("jog", true, 1, true);
    follower.Play("walk", true, 1, true);

    AnimationSyncGroupRegistry registry;
    for (int frame = 0; frame < 120; ++frame)
    {
        registry.BeginTick();
        registry.AddNode(leader.BuildSyncGroupNode("leader", "actor", "mesh", false));
        registry.AddNode(follower.BuildSyncGroupNode("follower", "actor", "mesh", false));
        registry.Resolve();
        leader.ApplySyncGroupResult(registry.Results()[0],
            registry.Results()[0].leaderIndex >= 0 ?
                registry.Nodes()[static_cast<size_t>(registry.Results()[0].leaderIndex)].identity :
                std::string_view());
        follower.ApplySyncGroupResult(registry.Results()[1],
            registry.Results()[1].leaderIndex >= 0 ?
                registry.Nodes()[static_cast<size_t>(registry.Results()[1].leaderIndex)].identity :
                std::string_view());
        leader.Tick(1.0f / 60);
        follower.Tick(1.0f / 60);
        EXPECT_TRUE(follower.SyncActive());
        EXPECT_EQ(follower.SyncLeader(), "leader");
        EXPECT_STREQ(follower.CurrentSyncScopeName(), "Local");
    }
    EXPECT_NEAR(follower.SyncPhase(), leader.SyncPhase(),
        1.0f / 60 / asset->clips[0].runtime->duration());
}

TEST(AnimationSyncGroup, TransitionSourceLeadsIncomingFollowerForWholeBlend)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    player.Play("jog", true, 1, true);
    player.Seek(1.0f);
    const float sourcePhase = player.SyncPhase();
    player.TransitionToSynced("walk", 0, true, 1.0f, true, "Locomotion", sourcePhase);
    ASSERT_TRUE(player.HasSyncSourceClock());

    AnimationSyncGroupRegistry registry;
    for (int frame = 0; frame < 90 && player.HasSyncSourceClock(); ++frame)
    {
        registry.BeginTick();
        const SyncGroupNode target =
            player.BuildSyncGroupNode("target", "actor", "mesh", false);
        const SyncGroupNode source =
            player.BuildSyncGroupNode("source", "actor", "mesh", true);
        ASSERT_EQ(source.group, "Locomotion");
        ASSERT_EQ(source.role, SyncRole::CanBeLeader);
        registry.AddNode(target);
        registry.AddNode(source);
        registry.Resolve();
        // 旧片段（Jog）在混合窗口内是 CanBeLeader；新片段（Walk）持续跟随，
        // 不是只在 TransitionToSynced 那一帧对齐。
        EXPECT_TRUE(registry.Results()[1].leader);
        EXPECT_TRUE(registry.Results()[0].follower);
        EXPECT_TRUE(registry.Results()[0].hasFollow);
        const float leaderPhase = std::fmod(source.time, 4.0f) / 4.0f;
        const float followerPhase = std::fmod(target.time, 2.0f) / 2.0f;
        float error = std::abs(leaderPhase - followerPhase);
        error = std::min(error, 1.0f - error);
        EXPECT_LE(error * 2.0f, 1.0f / 60 + 1e-5f) << "frame=" << frame;
        player.ApplySyncGroupResult(registry.Results()[0], source.identity);
        player.Tick(1.0f / 60);
    }
    // 混合结束后影子源时钟必须释放，播放器回到唯一目标时钟。
    EXPECT_FALSE(player.HasSyncSourceClock());
}

TEST(AnimationSyncGroup, FinishedNonLoopingSourceDoesNotLeadIncomingCycle)
{
    auto asset = MakeAsset();
    SkeletalPlayback player(asset);
    // 一次性起步片段播完后仍作为交叉淡入的源；它的相位已经 clamp 在末尾，
    // 再拿它当 leader 会把目标循环钉死在末帧（表现为“卡脚”）。
    player.Play("walk", false, 1, true);
    player.Tick(asset->clips[0].runtime->duration());
    ASSERT_TRUE(player.Completed());
    player.TransitionToSynced("jog", 0, true, 0.5f, true, "Locomotion", 0);
    ASSERT_TRUE(player.HasSyncSourceClock());

    const SyncGroupNode target = player.BuildSyncGroupNode("target", "actor", "mesh", false);
    const SyncGroupNode source = player.BuildSyncGroupNode("source", "actor", "mesh", true);
    EXPECT_FALSE(source.active) << "finished non-looping source must not stay in the election";

    AnimationSyncGroupRegistry registry;
    registry.BeginTick();
    registry.AddNode(target);
    registry.AddNode(source);
    registry.Resolve();
    EXPECT_FALSE(registry.Results()[0].hasFollow);
    EXPECT_LT(registry.Results()[0].leaderIndex, 0);
}
} // namespace
