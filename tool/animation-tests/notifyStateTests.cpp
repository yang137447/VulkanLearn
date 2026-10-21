#include "animation/skeletalPlayback.h"

#include <cmath>
#include <string>
#include <vector>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

Eigen::Matrix4f Translation(float x, float y, float z)
{
    Eigen::Matrix4f value = Eigen::Matrix4f::Identity();
    value.block<3, 1>(0, 3) = Eigen::Vector3f(x, y, z);
    return value;
}

Clip MakeClip(const std::string& id, float duration, const std::vector<SequenceNotify>& notifies)
{
    Clip clip;
    clip.id = id;
    clip.raw.duration = duration;
    clip.raw.tracks.resize(2);
    // root 在 [0,duration] 内沿 +Z 线性移动 10 cm/s，便于核对播放范围的根位移积分。
    clip.raw.tracks[0].translations = {{0, {0, 0, 0}}, {duration, {0, 0, duration * 10}}};
    clip.raw.tracks[0].rotations = {{0, ozz::math::Quaternion::identity()}};
    clip.raw.tracks[0].scales = {{0, ozz::math::Float3::one()}};
    clip.raw.tracks[1].translations = {{0, {0, 10, 0}}};
    clip.raw.tracks[1].rotations = {{0, ozz::math::Quaternion::identity()}};
    clip.raw.tracks[1].scales = {{0, ozz::math::Float3::one()}};
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    clip.sequence.id = id;
    clip.sequence.duration = duration;
    clip.sequence.notifies = notifies;
    return clip;
}

// 最小两关节资产；第二个片段 "other" 只在需要切片段时生成。
std::shared_ptr<SkeletalAsset> MakeNotifyAsset(float duration,
    std::vector<SequenceNotify> notifies, std::vector<SequenceNotify> otherNotifies = {})
{
    auto asset = std::make_shared<SkeletalAsset>();
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    raw.roots[0].children.resize(1);
    raw.roots[0].children[0].name = "hip";
    raw.roots[0].children[0].transform = ozz::math::Transform::identity();
    raw.roots[0].children[0].transform.translation = {0, 10, 0};
    ozz::animation::offline::SkeletonBuilder skeletonBuilder;
    asset->mesh.runtime = skeletonBuilder(raw);
    asset->mesh.joints = {
        {"root", -1, Translation(0, 0, 0), Translation(0, 0, 0)},
        {"hip", 0, Translation(0, 10, 0), Translation(0, 10, 0)}};
    SkinSection section;
    for (uint32_t joint = 0; joint < 2; ++joint)
    {
        Vertex vertex{};
        vertex.position = asset->mesh.joints[joint].restModel.block<3, 1>(0, 3);
        section.vertices.push_back(vertex);
        section.influenceOffsets.push_back(joint);
        section.influences.push_back({joint, 1});
        section.inverseBinds.push_back(asset->mesh.joints[joint].restModel.inverse());
    }
    section.influenceOffsets.push_back(2);
    asset->mesh.sections.push_back(std::move(section));
    asset->clips.push_back(MakeClip("clip", duration, notifies));
    if (!otherNotifies.empty()) asset->clips.push_back(MakeClip("other", duration, otherNotifies));
    return asset;
}

std::string Describe(const FiredNotify& event)
{
    return event.name + ":" + event.phase + (event.interrupted ? ":interrupted" : "");
}

std::vector<std::string> EventLog(const SkeletalPlayback& player)
{
    std::vector<std::string> result;
    for (const auto& event : player.FiredNotifies()) result.push_back(Describe(event));
    return result;
}

TEST(NotifyPlayback, InstantNotifyFiresOnceAndBoundaryAtCurrentIsDeferred)
{
    const auto asset = MakeNotifyAsset(1, {
        {"track", "Start", 0, 0},
        {"track", "Quarter", 0.25f, 0},
        {"track", "Half", 0.5f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", false, 1, true);
    EXPECT_TRUE(player.FiredNotifies().empty());

    player.Tick(0.25f);
    // time==0 的 notify 在首 tick 触发（下界含）；time==current 的事件留到下一 tick。
    ASSERT_EQ(player.FiredNotifies().size(), 1u);
    EXPECT_EQ(player.FiredNotifies()[0].name, "Start");
    EXPECT_EQ(player.FiredNotifies()[0].kind, "notify");
    EXPECT_EQ(player.FiredNotifies()[0].phase, "instant");
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].startTime, 0);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].duration, 0);
    EXPECT_FALSE(player.FiredNotifies()[0].interrupted);

    player.Tick(0.125f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Quarter:instant"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0.25f);

    player.Tick(0.125f); // 到达 0.5：Half 落在当前时刻，不触发
    EXPECT_TRUE(player.FiredNotifies().empty());

    player.Tick(0.125f); // 0.5 -> 0.625：补发 Half 一次
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Half:instant"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0.5f);

    player.Tick(0.5f); // 到片段末尾
    EXPECT_TRUE(player.FiredNotifies().empty());
    EXPECT_TRUE(player.Completed());
    EXPECT_FALSE(player.IsPlaying());
}

TEST(NotifyPlayback, NotifyStateFiresBeginThenNaturalEnd)
{
    const auto asset = MakeNotifyAsset(1, {{"track", "Window", 0.25f, 0.5f}});
    SkeletalPlayback player(asset);
    player.Play("clip", false, 1, true);
    player.Tick(0.25f); // 恰好到达 0.25：begin 延后
    EXPECT_TRUE(player.FiredNotifies().empty());
    EXPECT_TRUE(player.ActiveNotifyStates().empty());

    player.Tick(0.125f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:begin"}));
    EXPECT_EQ(player.FiredNotifies()[0].kind, "notifyState");
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0.25f);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].startTime, 0.25f);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].duration, 0.5f);
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);
    EXPECT_EQ(player.ActiveNotifyStates()[0].name, "Window");
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].startTime, 0.25f);
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].endTime, 0.75f);

    player.Tick(0.25f); // 0.375 -> 0.625：仍在区间内
    EXPECT_TRUE(player.FiredNotifies().empty());
    EXPECT_EQ(player.ActiveNotifyStates().size(), 1u);

    player.Tick(0.25f); // 0.625 -> 0.875：跨过 0.75
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:end"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0.75f);
    EXPECT_FALSE(player.FiredNotifies()[0].interrupted);
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
}

TEST(NotifyPlayback, NotifyStateBeyondClipEndEndsInterruptedAtCompletion)
{
    const auto asset = MakeNotifyAsset(1, {{"track", "Long", 0.5f, 0.75f}});
    SkeletalPlayback player(asset);
    player.Play("clip", false, 1, true);
    player.Tick(0.5f); // 到 0.5：begin 延后
    EXPECT_TRUE(player.FiredNotifies().empty());
    player.Tick(0.25f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Long:begin"}));
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);
    // 作者原值不被裁剪：endTime 越过片段末尾。
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].endTime, 1.25f);

    player.Tick(0.25f); // 0.75 -> 1.0：范围结束，补 end(interrupted)
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Long:end:interrupted"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 1);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].startTime, 0.5f);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].duration, 0.75f);
    EXPECT_TRUE(player.Completed());
    EXPECT_FALSE(player.IsPlaying());
    EXPECT_TRUE(player.ActiveNotifyStates().empty());

    player.Tick(0.25f); // 已 completed，不重复补发
    EXPECT_TRUE(player.FiredNotifies().empty());
}

TEST(NotifyPlayback, StopAndClipSwitchEndActiveStatesInterrupted)
{
    const auto asset = MakeNotifyAsset(1, {{"track", "Window", 0.25f, 0.5f}},
        {{"track", "Other", 0.5f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", true, 1, true);
    player.Tick(0.375f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:begin"}));

    player.Stop();
    EXPECT_FALSE(player.IsPlaying());
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:begin", "Window:end:interrupted"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies().back().time, 0.375f);
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
    // Stop 不移动时钟；补发的 end 留到下一 Tick 的事件窗口，不会被同帧 Tick 清掉。
    player.Tick(0.25f);
    EXPECT_FLOAT_EQ(player.Time(), 0.375f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:end:interrupted"}));
    player.Tick(0.25f);
    EXPECT_TRUE(player.FiredNotifies().empty());

    // 切片段：旧片段活动区间补 end(interrupted)，新片段不继承旧 Montage 源/播放范围。
    player.Play("clip", true, 1, true);
    player.Tick(0.375f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:begin"}));
    player.Play("other", true, 1, true);
    EXPECT_EQ(player.SelectedClipId(), "other");
    EXPECT_FLOAT_EQ(player.Time(), 0);
    ASSERT_EQ(player.FiredNotifies().size(), 2u);
    EXPECT_EQ(player.FiredNotifies().back().name, "Window");
    EXPECT_EQ(player.FiredNotifies().back().phase, "end");
    EXPECT_TRUE(player.FiredNotifies().back().interrupted);
    EXPECT_FLOAT_EQ(player.FiredNotifies().back().time, 0.375f);
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
    player.Tick(0.5f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:end:interrupted"}));
}

TEST(NotifyPlayback, LoopWrapFiresTailThenHeadAndRestartsCrossingStates)
{
    const auto asset = MakeNotifyAsset(1, {
        {"track", "Zero", 0, 1.2f},      // 跨越循环点：回绕 end(interrupted)，新循环再 begin
        {"track", "Head", 0.125f, 0},
        {"track", "ExactEnd", 0.625f, 0.375f}, // 恰好在循环末尾自然结束，不算中断
        {"track", "Tail", 0.875f, 0},
        {"track", "Late", 0.9375f, 0.25f}}); // 也跨越循环点
    SkeletalPlayback player(asset);
    player.Play("clip", true, 1, true);
    player.Tick(0.5f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Zero:begin", "Head:instant"}));

    // 0.5 -> 1.25：回绕到 0.25。顺序固定为尾部事件、回绕 end、头部 begin/instant。
    player.Tick(0.75f);
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{
        "ExactEnd:begin", "Tail:instant", "Late:begin", "ExactEnd:end",
        "Zero:end:interrupted", "Late:end:interrupted", "Zero:begin", "Head:instant"}));
    const auto& events = player.FiredNotifies();
    EXPECT_FLOAT_EQ(events[3].time, 1); // ExactEnd 自然结束于 rangeEnd
    EXPECT_FALSE(events[3].interrupted);
    EXPECT_FLOAT_EQ(events[4].time, 1);
    EXPECT_TRUE(events[4].interrupted);
    EXPECT_FLOAT_EQ(events[7].time, 0.125f);
    // 回绕后只剩 Zero 重新进入区间；Late 的起点在头部扫描窗口之外，不重放。
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);
    EXPECT_EQ(player.ActiveNotifyStates()[0].name, "Zero");
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].endTime, 1.2f);
}

TEST(NotifyPlayback, SeekClearsActiveStateWithoutReplayingSkippedEvents)
{
    const auto asset = MakeNotifyAsset(1, {
        {"track", "Window", 0.25f, 0.25f},
        {"track", "Later", 0.625f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", false, 1, true);
    player.Tick(0.375f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Window:begin"}));
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);

    player.Seek(0.5f);
    EXPECT_TRUE(player.FiredNotifies().empty());
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
    EXPECT_FLOAT_EQ(player.Time(), 0.5f);

    // seek 之后的第一 tick 只处理新位置之后的事件：不补 Window 的 end，Later 正常触发。
    player.Tick(0.25f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Later:instant"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0.625f);
    player.Tick(0.25f);
    EXPECT_TRUE(player.FiredNotifies().empty());
}

TEST(NotifyPlayback, OverlappingAndSameNameNotifiesTrackIndependently)
{
    const auto asset = MakeNotifyAsset(1, {
        {"track", "Echo", 0.25f, 0},
        {"track", "Blend", 0.25f, 0.5f},
        {"track", "Echo", 0.75f, 0},
        {"track", "Blend", 0.5f, 0.5f}});
    SkeletalPlayback player(asset);
    player.Play("clip", false, 1, true);
    player.Tick(0.25f);
    EXPECT_TRUE(player.FiredNotifies().empty());
    player.Tick(0.25f); // 0.25 -> 0.5
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{
        "Echo:instant", "Blend:begin"}));
    // 第二个 Blend 在 0.5 才进入区间；begin 落在当前时刻，延后到下一 tick。
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].startTime, 0.25f);

    player.Tick(0.25f); // 0.5 -> 0.75：第二个 Blend 开始，第一个 Blend 自然结束
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{
        "Blend:begin", "Blend:end"}));
    EXPECT_FALSE(player.FiredNotifies()[1].interrupted);
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].startTime, 0.5f);

    player.Tick(0.25f); // 0.75 -> 1.0：同名 Echo 第二次触发、Blend 自然结束
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{
        "Echo:instant", "Blend:end"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[0].time, 0.75f);
    EXPECT_FALSE(player.FiredNotifies()[1].interrupted);
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
}

TEST(NotifyPlayback, PlaybackRangeClampsDurationRootMotionAndNotifyDomain)
{
    const auto asset = MakeNotifyAsset(2, {
        {"track", "Before", 0.25f, 0},
        {"track", "Inside", 1.25f, 0},
        {"track", "Long", 1.5f, 0.5f},
        {"track", "Edge", 1.875f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", false, 1, true);
    EXPECT_FLOAT_EQ(player.Duration(), 2);
    EXPECT_NEAR(player.RootMotionTotal().z(), 20, 0.01f);

    player.SetPlaybackRange(1, 1.875f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 1);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 1.875f);
    EXPECT_FLOAT_EQ(player.Duration(), 0.875f);
    // 根位移积分按范围：T(1.875)-T(1.0)=8.75，而不是整段 20。
    EXPECT_NEAR(player.RootMotionTotal().z(), 8.75f, 0.01f);
    EXPECT_THROW(player.SetPlaybackRange(-1, 1), std::invalid_argument);
    EXPECT_THROW(player.SetPlaybackRange(1, 1), std::invalid_argument);
    // end 越过片段末尾按片段 clamp（§5.3），不是拒绝；clamp 后为空的范围才拒绝。
    player.SetPlaybackRange(1, 2.5f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 2);
    EXPECT_FLOAT_EQ(player.Duration(), 1);
    EXPECT_THROW(player.SetPlaybackRange(2, 3), std::invalid_argument);
    player.SetPlaybackRange(1, 1.875f);
    EXPECT_THROW(player.Seek(0.5f), std::invalid_argument);

    // 范围之前的 notify 不触发：0 -> 0.5 虽然跨过 0.25，但有效域从 1.0 才开始。
    player.Tick(0.5f);
    EXPECT_TRUE(player.FiredNotifies().empty());

    player.Seek(1);
    EXPECT_NEAR(player.RootMotionRemaining().z(), 8.75f, 0.01f);
    player.Tick(0.25f); // 1.0 -> 1.25：Inside 恰好落在当前时刻，延后
    EXPECT_TRUE(player.FiredNotifies().empty());
    player.Tick(0.25f); // 1.25 -> 1.5
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{"Inside:instant"}));
    EXPECT_NEAR(player.RootMotionRemaining().z(), 3.75f, 0.01f);
    player.Tick(0.25f); // 1.5 -> 1.75：Long 进入区间
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{"Long:begin"}));

    // 到达范围末尾（不是片段末尾）：Edge 在 terminal tick 触发；Long 补 end(interrupted)。
    player.Tick(0.125f);
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{
        "Edge:instant", "Long:end:interrupted"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies()[1].time, 1.875f);
    EXPECT_FLOAT_EQ(player.FiredNotifies()[1].duration, 0.5f);
    EXPECT_TRUE(player.Completed());
    EXPECT_NEAR(player.RootMotionRemaining().z(), 0, 0.01f);
}

TEST(NotifyPlayback, MontageNotifiesMergeWithSequenceNotifiesAndPlayResetsOverlay)
{
    const auto asset = MakeNotifyAsset(1, {{"Footstep", "Foot", 0.25f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", true, 1, true);
    player.SetMontageNotifies({
        {"notifyState", "2", "Warp", 0.125f, 0.25f},
        {"notify", "1", "Ping", 0.5f, 0}});

    player.Tick(0.5f); // 0 -> 0.5：Warp begin 0.125、Foot 0.25、Warp end 0.375
    EXPECT_EQ(EventLog(player), (std::vector<std::string>{
        "Warp:begin", "Foot:instant", "Warp:end"}));
    EXPECT_EQ(player.FiredNotifies()[0].kind, "notifyState");
    EXPECT_EQ(player.FiredNotifies()[1].kind, "notify");
    EXPECT_EQ(player.FiredNotifies()[1].track, "Footstep");
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
    player.Tick(0.25f); // 0.5 -> 0.75：Montage 瞬时 notify
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Ping:instant"}));

    // Play 重新开始会清掉 Montage 覆盖层；Sequence notify 仍保留并执行。
    player.Play("clip", true, 1, true);
    player.Tick(0.5f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Foot:instant"}));
}

TEST(NotifyPlayback, ReplacingMontageNotifiesEndsTheOldSourcesActiveStates)
{
    const auto asset = MakeNotifyAsset(1, {}, {{"track", "Other", 0.5f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", true, 1, true);
    player.SetMontageNotifies({{"notifyState", "1", "Old", 0.25f, 0.5f}});
    player.Tick(0.5f);
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Old:begin"}));
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);

    player.SetMontageNotifies({{"notify", "1", "New", 0.75f, 0}});
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{
        "Old:begin", "Old:end:interrupted"}));
    EXPECT_TRUE(player.ActiveNotifyStates().empty());

    player.Tick(0.25f); // 0.5 -> 0.75：新来源生效
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Old:end:interrupted"}));
    player.Tick(0.25f); // 0.75 -> 1.0
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"New:instant"}));
}

TEST(NotifyPlayback, TransitionEndsActiveStateAndResetsMontagePlaybackRange)
{
    const auto asset = MakeNotifyAsset(2, {}, {{"track", "Other", 0.75f, 0}});
    SkeletalPlayback player(asset);
    player.Play("clip", true, 1, true);
    player.SetPlaybackRange(1, 1.5f);
    // Montage notify 的区间越过播放范围末尾：活动时可见作者原值 endTime，范围结束时中断。
    player.SetMontageNotifies({{"notifyState", "1", "Range", 1.25f, 0.5f}});
    player.Seek(1);
    player.Tick(0.25f); // 1.0 -> 1.25：begin 恰好落在当前时刻，延后
    EXPECT_TRUE(player.FiredNotifies().empty());
    player.Tick(0.125f); // 1.25 -> 1.375
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Range:begin"}));
    ASSERT_EQ(player.ActiveNotifyStates().size(), 1u);
    EXPECT_FLOAT_EQ(player.ActiveNotifyStates()[0].endTime, 1.75f);

    player.TransitionTo("other", 0.5f, true, 0);
    EXPECT_EQ(player.SelectedClipId(), "other");
    EXPECT_FLOAT_EQ(player.Duration(), 2); // 过渡开启新播放段，回到整段片段
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{
        "Range:begin", "Range:end:interrupted"}));
    EXPECT_FLOAT_EQ(player.FiredNotifies().back().time, 1.375f);
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
    player.Tick(0.25f); // 0.5 -> 0.75：补发的 end 仍在下一 Tick 的事件窗口
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Range:end:interrupted"}));
    player.Tick(0.25f); // 0.75 -> 1.0：新片段自己的 notify
    ASSERT_EQ(EventLog(player), (std::vector<std::string>{"Other:instant"}));
}
}
