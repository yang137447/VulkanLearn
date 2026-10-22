#include "animation/animationScriptData.h"
#include "animation/animationMontage.h"
#include "animation/skeletalPlayback.h"

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;
using nlohmann::json;

std::string ErrorOf(const std::function<void()>& action)
{
    try
    {
        action();
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

std::shared_ptr<SkeletalAsset> MakeAsset()
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
    // Vertex 的 Eigen 成员不会由空构造函数清零；bounds 夹具必须提供真实位置。
    vertex.position = Eigen::Vector3f::Zero();
    section.vertices.push_back(vertex);
    section.influenceOffsets = {0, 1};
    section.influences = {{0, 1}};
    section.inverseBinds = {Eigen::Matrix4f::Identity()};
    asset->mesh.sections.push_back(std::move(section));

    Clip clip;
    clip.id = "walk";
    clip.raw.duration = 2.0f;
    clip.raw.tracks.resize(1);
    clip.raw.tracks[0].translations = {{0, {0, 0, 0}}, {2, {0, 0, 200}}};
    clip.raw.tracks[0].rotations = {{0, ozz::math::Quaternion::identity()}};
    clip.raw.tracks[0].scales = {{0, ozz::math::Float3::one()}};
    ozz::animation::offline::AnimationBuilder animationBuilder;
    clip.runtime = animationBuilder(clip.raw);
    clip.sequence.id = "walk";
    clip.sequence.duration = 2.0f;
    clip.sequence.curves.push_back({"distance_from_ledge", {0, 0.598017f, 0.831335f, 2},
        {-75, 34.098678f, 90.908946f, 110}});
    asset->clips.push_back(std::move(clip));
    return asset;
}

MontageWarp MakeWarp()
{
    MontageWarp warp;
    warp.target = "FrontLedge";
    warp.provider = "Bone";
    warp.bone = "root";
    warp.subtractRemainingRootMotion = true;
    warp.ignoreZAxis = true;
    warp.warpRotation = true;
    warp.rotationTimeMultiplier = 1.5f;
    warp.easing = "Linear";
    return warp;
}

Montage MakeMontage()
{
    Montage montage;
    montage.id = "AM_Test";
    montage.sequencePath = "Maps/Test/walk.anim.json";
    montage.derivedClipId = "walk";
    montage.slot = "DefaultSlot";
    montage.length = 1.5f;
    // 默认夹具必须自洽（nextSection 为空 = 播放到段末结束）：多 section/链接行为由
    // 专门的 ChainMontage 夹具覆盖，否则每次 PlayMontage 都会被链校验拒绝。
    montage.sections.push_back({"Default", 0, 1.5f, "", 0, ""});
    montage.notifies.push_back({"notify", "1", "Footstep", 0, 0, std::nullopt});
    montage.notifies.push_back({"notifyState", "1", "MotionWarping", 0.1f, 0.4f, MakeWarp()});
    montage.windows.push_back({"FrontLedge", 0.1f, 0.5f});
    return montage;
}

// 多 section 链夹具：默认不带 warp notify（window 由 Mesh/Clip 夹具单独验证），
// length 取测试 clip 的时长 2s，section 时间必须落在其内。
Montage ChainMontage(std::vector<MontageSection> sections,
    std::vector<MontageNotify> notifies = {})
{
    Montage montage = MakeMontage();
    montage.id = "AM_Chain";
    montage.length = 2.0f;
    montage.sections = std::move(sections);
    montage.notifies = std::move(notifies);
    montage.windows.clear();
    return montage;
}

// 按 tick 收集 notify 事件，便于断言“只触发一次/不重复不丢”。
void CollectNotifies(const SkeletalPlayback& player, std::vector<FiredNotify>& events)
{
    for (const FiredNotify& event : player.FiredNotifies())
        events.push_back(event);
}

size_t CountEvents(const std::vector<FiredNotify>& events, const std::string& name,
    const std::string& phase, bool interrupted)
{
    size_t count = 0;
    for (const FiredNotify& event : events)
        if (event.name == name && event.phase == phase && event.interrupted == interrupted)
            ++count;
    return count;
}

std::map<std::string, MotionWarpTarget> Targets()
{
    MotionWarpTarget target;
    target.location = Eigen::Vector3f(10, 20, 30);
    return {{"FrontLedge", target}};
}

TEST(AnimationScriptData, PublishesCurvesOnlyForClipsWithSidecarCurves)
{
    auto asset = MakeAsset();
    // 第二个 clip 没有 sidecar；即使存在也不发布空 curves。
    Clip plain;
    plain.id = "jump";
    plain.raw = asset->clips[0].raw;
    ozz::animation::offline::AnimationBuilder builder;
    plain.runtime = builder(plain.raw);
    asset->clips.push_back(std::move(plain));
    asset->montages.push_back(MakeMontage());

    const json catalog = BuildAnimationScriptAssets(*asset);
    ASSERT_TRUE(catalog.at("curves").contains("walk"));
    EXPECT_FALSE(catalog.at("curves").contains("jump"));
    const auto& curve = catalog.at("curves").at("walk").front();
    EXPECT_EQ(curve.at("name"), "distance_from_ledge");
    EXPECT_EQ(curve.at("times"), json::array({0, 0.598017f, 0.831335f, 2}));
    EXPECT_EQ(curve.at("values"), json::array({-75, 34.098678f, 90.908946f, 110}));
}

TEST(AnimationScriptData, SerializesMontageSectionsNotifiesAndWarpFields)
{
    auto asset = MakeAsset();
    Montage montage = MakeMontage();
    // catalog 只做数据序列化，不执行链接校验：这里专门写非默认 link 字段验证 round-trip。
    montage.sections.front() = {"Default", 0, 1.5f, "Loop", 0.5f, "Absolute"};
    asset->montages.push_back(montage);

    const json catalog = BuildAnimationScriptAssets(*asset);
    ASSERT_EQ(catalog.at("montages").size(), 1u);
    const auto& montageJson = catalog.at("montages").front();
    EXPECT_EQ(montageJson.at("id"), "AM_Test");
    EXPECT_EQ(montageJson.at("sequencePath"), "Maps/Test/walk.anim.json");
    EXPECT_EQ(montageJson.at("derivedClipId"), "walk");
    EXPECT_FLOAT_EQ(montageJson.at("length").get<float>(), 1.5f);
    EXPECT_EQ(montageJson.at("slot"), "DefaultSlot");
    ASSERT_EQ(montageJson.at("sections").size(), 1u);
    const auto& section = montageJson.at("sections").front();
    EXPECT_EQ(section.at("name"), "Default");
    EXPECT_FLOAT_EQ(section.at("startTime").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(section.at("endTime").get<float>(), 1.5f);
    EXPECT_EQ(section.at("nextSection"), "Loop");
    EXPECT_FLOAT_EQ(section.at("nextTime").get<float>(), 0.5f);
    EXPECT_EQ(section.at("linkMethod"), "Absolute");
    ASSERT_EQ(montageJson.at("notifies").size(), 2u);
    EXPECT_EQ(montageJson.at("notifies")[0].at("name"), "Footstep");
    EXPECT_FALSE(montageJson.at("notifies")[0].contains("warp"));
    const auto& warp = montageJson.at("notifies")[1].at("warp");
    EXPECT_EQ(warp.at("target"), "FrontLedge");
    EXPECT_EQ(warp.at("provider"), "Bone");
    EXPECT_EQ(warp.at("bone"), "root");
    EXPECT_TRUE(warp.at("subtractRemainingRootMotion").get<bool>());
    EXPECT_TRUE(warp.at("ignoreZAxis").get<bool>());
    EXPECT_TRUE(warp.at("warpRotation").get<bool>());
    EXPECT_FLOAT_EQ(warp.at("rotationTimeMultiplier").get<float>(), 1.5f);
    EXPECT_EQ(warp.at("rotationMethod"), "Slerp");
    EXPECT_EQ(warp.at("easing"), "Linear");
    // staticPose 只由原生执行层从 Montage 数据读取，catalog/C# 不发布也不需要它。
    EXPECT_FALSE(warp.contains("staticPose"));
}

TEST(AnimationScriptData, SerializesFiredNotifyFields)
{
    FiredNotify fired;
    fired.name = "Footstep";
    fired.track = "Footstep Left";
    fired.kind = "notifyState";
    fired.phase = "end";
    fired.time = 0.75f;
    fired.startTime = 0.25f;
    fired.duration = 0.5f;
    fired.interrupted = true;

    const json feedback = BuildAnimationNotifyFeedback({fired});
    ASSERT_EQ(feedback.size(), 1u);
    const auto& value = feedback.front();
    EXPECT_EQ(value.at("name"), "Footstep");
    EXPECT_EQ(value.at("track"), "Footstep Left");
    EXPECT_EQ(value.at("kind"), "notifyState");
    EXPECT_EQ(value.at("phase"), "end");
    EXPECT_FLOAT_EQ(value.at("time").get<float>(), 0.75f);
    EXPECT_FLOAT_EQ(value.at("startTime").get<float>(), 0.25f);
    EXPECT_FLOAT_EQ(value.at("duration").get<float>(), 0.5f);
    EXPECT_TRUE(value.at("interrupted").get<bool>());
}

TEST(AnimationScriptData, BuildsWarpWindowsInNotifyOrderWithTargetsAndFlags)
{
    Montage montage = MakeMontage();
    MontageWarp second = MakeWarp();
    second.target = "BackFloor";
    second.provider = "None";
    second.bone.clear();
    second.subtractRemainingRootMotion = false;
    second.ignoreZAxis = false;
    second.warpRotation = false;
    montage.notifies.push_back({"notifyState", "1", "MotionWarping", 0.6f, 0.2f, second});

    auto targets = Targets();
    MotionWarpTarget backFloor;
    backFloor.location = Eigen::Vector3f(1, 2, 3);
    targets["BackFloor"] = backFloor;
    const auto windows = BuildMontageWarpWindows(montage, targets);
    ASSERT_EQ(windows.size(), 2u);
    EXPECT_EQ(windows[0].targetName, "FrontLedge");
    EXPECT_TRUE(windows[0].targetLocation.isApprox(Eigen::Vector3f(10, 20, 30)));
    EXPECT_FLOAT_EQ(windows[0].startTime, 0.1f);
    EXPECT_FLOAT_EQ(windows[0].endTime, 0.5f);
    EXPECT_TRUE(windows[0].subtractRemainingRootMotion);
    EXPECT_TRUE(windows[0].ignoreVertical);
    EXPECT_EQ(windows[0].warpPointBone, "root");
    EXPECT_TRUE(windows[0].warpRotation);
    EXPECT_FLOAT_EQ(windows[0].rotationTimeMultiplier, 1.5f);
    EXPECT_EQ(windows[0].rotationMethod, "Slerp");
    EXPECT_FALSE(windows[0].hasTargetRotation);
    EXPECT_EQ(windows[1].targetName, "BackFloor");
    EXPECT_TRUE(windows[1].targetLocation.isApprox(Eigen::Vector3f(1, 2, 3)));
    EXPECT_FALSE(windows[1].subtractRemainingRootMotion);
    EXPECT_FALSE(windows[1].ignoreVertical);
    EXPECT_TRUE(windows[1].warpPointBone.empty());
    EXPECT_FALSE(windows[1].warpRotation);
}

TEST(AnimationScriptData, CarriesTargetRotationIntoWindow)
{
    Montage montage = MakeMontage();
    auto targets = Targets();
    targets["FrontLedge"].hasRotation = true;
    targets["FrontLedge"].rotation = Eigen::Quaternionf(
        Eigen::AngleAxisf(0.75f, Eigen::Vector3f::UnitY()));
    const auto windows = BuildMontageWarpWindows(montage, targets);
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_TRUE(windows[0].hasTargetRotation);
    EXPECT_NEAR(windows[0].targetRotation.angularDistance(targets["FrontLedge"].rotation), 0.0f, 1e-6f);
}

TEST(AnimationScriptData, RejectsUnsupportedRotationMethodWithDiagnostics)
{
    Montage montage = MakeMontage();
    montage.notifies[1].warp->rotationMethod = "SlerpWithClampedRate";
    const std::string message = ErrorOf([&] { BuildMontageWarpWindows(montage, Targets()); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("section 'Default'"), std::string::npos);
    EXPECT_NE(message.find("SlerpWithClampedRate"), std::string::npos);
    EXPECT_NE(message.find("only Slerp is supported"), std::string::npos);

    // bWarpRotation=false 时 rotationMethod 不参与执行，数据保留、不拒绝。
    montage.notifies[1].warp->warpRotation = false;
    EXPECT_NO_THROW(BuildMontageWarpWindows(montage, Targets()));
}

TEST(AnimationScriptData, MapsStaticPoseIntoWarpWindow)
{
    // UE WarpPointAnimProvider=Static：staticPose 是 mesh/组件空间静态变换，
    // 数据层 (x,y,z,w) 四元数必须换到 Eigen 的 (w,x,y,z) 并归一。
    Montage montage = MakeMontage();
    montage.notifies[1].warp->provider = "Static";
    montage.notifies[1].warp->bone.clear();
    const float halfAngle = 0.5f * 3.14159265358979323846f / 2.0f;
    montage.notifies[1].warp->staticPose = MontageStaticPose{
        {10.0f, 20.0f, 30.0f}, {0.0f, std::sin(halfAngle), 0.0f, std::cos(halfAngle)}};
    const auto windows = BuildMontageWarpWindows(montage, Targets());
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_TRUE(windows[0].hasStaticWarpPoint);
    EXPECT_TRUE(windows[0].staticWarpPointLocation.isApprox(Eigen::Vector3f(10, 20, 30)));
    const Eigen::Quaternionf expected(Eigen::AngleAxisf(
        0.5f * 3.14159265358979323846f, Eigen::Vector3f::UnitY()));
    EXPECT_NEAR(windows[0].staticWarpPointRotation.angularDistance(expected), 0.0f, 1e-6f);
    EXPECT_TRUE(windows[0].warpPointBone.empty());

    // None/Bone 即使数据里残留 staticPose 也不消费（UE 只看 provider）。
    Montage none = MakeMontage();
    none.notifies[1].warp->provider = "None";
    none.notifies[1].warp->bone.clear();
    none.notifies[1].warp->staticPose = MontageStaticPose{};
    const auto noneWindows = BuildMontageWarpWindows(none, Targets());
    ASSERT_EQ(noneWindows.size(), 1u);
    EXPECT_FALSE(noneWindows[0].hasStaticWarpPoint);
}

TEST(AnimationScriptData, RejectsStaticProviderWithoutUsablePose)
{
    Montage montage = MakeMontage();
    montage.notifies[1].warp->provider = "Static";
    montage.notifies[1].warp->bone.clear();
    montage.notifies[1].warp->staticPose.reset();
    std::string message = ErrorOf([&] { BuildMontageWarpWindows(montage, Targets()); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("section 'Default'"), std::string::npos);
    EXPECT_NE(message.find("Static"), std::string::npos);
    EXPECT_NE(message.find("staticPose"), std::string::npos);

    // 零四元数无法归一，不能静默当 Identity 使用。
    montage.notifies[1].warp->staticPose = MontageStaticPose{{0, 0, 0}, {0, 0, 0, 0}};
    message = ErrorOf([&] { BuildMontageWarpWindows(montage, Targets()); });
    EXPECT_NE(message.find("degenerate staticPose rotation"), std::string::npos);
}

TEST(AnimationScriptData, RejectsUnknownWarpProvider)
{
    Montage montage = MakeMontage();
    montage.notifies[1].warp->provider = "Socket";
    montage.notifies[1].warp->bone.clear();
    const std::string message = ErrorOf([&] { BuildMontageWarpWindows(montage, Targets()); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("section 'Default'"), std::string::npos);
    EXPECT_NE(message.find("Socket"), std::string::npos);
    EXPECT_NE(message.find("not supported"), std::string::npos);
}

TEST(AnimationScriptData, RejectsNonLinearEasingWithMontageSectionAndEasingDiagnostics)
{
    Montage montage = MakeMontage();
    montage.notifies[1].warp->easing = "EaseInOut";
    const std::string message = ErrorOf([&] { BuildMontageWarpWindows(montage, Targets()); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("section 'Default'"), std::string::npos);
    EXPECT_NE(message.find("EaseInOut"), std::string::npos);
    EXPECT_NE(message.find("Linear"), std::string::npos);
}

TEST(AnimationScriptData, RejectsMissingWarpTarget)
{
    const std::string message = ErrorOf([&] { BuildMontageWarpWindows(MakeMontage(), {}); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("requires warp target 'FrontLedge'"), std::string::npos);
}

TEST(AnimationScriptData, RejectsExtraWarpTarget)
{
    auto targets = Targets();
    targets["Extra"] = MotionWarpTarget{};
    const std::string message = ErrorOf([&] { BuildMontageWarpWindows(MakeMontage(), targets); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("does not declare warp target 'Extra'"), std::string::npos);
}

TEST(AnimationScriptData, PlayMontageAppliesRangeNotifiesAndSeeksToStart)
{
    auto asset = MakeAsset();
    asset->montages.push_back(MakeMontage());
    SkeletalPlayback player(asset);
    player.Play("walk", true, 1, false);
    player.Tick(0.2f);

    PlayMontage(player, asset->montages[0], Targets(), false, 1, true);
    EXPECT_EQ(player.SelectedClipId(), "walk");
    EXPECT_EQ(player.MontageId(), "AM_Test");
    EXPECT_FLOAT_EQ(player.MontageLength(), 1.5f);
    EXPECT_FLOAT_EQ(player.Time(), 0.0f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.0f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 1.5f);
    EXPECT_FLOAT_EQ(player.Duration(), 1.5f);
    EXPECT_FALSE(player.IsLooping());

    player.Tick(0.05f);
    bool instant = false;
    for (const auto& notify : player.FiredNotifies())
        instant = instant || (notify.name == "Footstep" && notify.phase == "instant" && notify.time == 0);
    EXPECT_TRUE(instant);

    player.Tick(0.1f); // 0.05 -> 0.15 跨过 warping 窗口起点，begin 只发一次。
    int begins = 0;
    for (const auto& notify : player.FiredNotifies())
        if (notify.name == "MotionWarping" && notify.phase == "begin") ++begins;
    EXPECT_EQ(begins, 1);
    EXPECT_EQ(player.ActiveNotifyStates().size(), 1u);
}

TEST(AnimationScriptData, PlayMontageStartTimeSkipsEarlierNotifiesAndRootMotion)
{
    auto asset = MakeAsset();
    asset->montages.push_back(MakeMontage());
    const Montage& montage = asset->montages[0];
    // 夹具：walk 2 s、根 z 从 0 线性到 200（100 cm/s）；Footstep 在 0，warp 窗口 [0.1,0.5]。
    // 入口 0.877 模拟 Catch 的 PoseSearchBranchIn：前摇已过，窗口也已结束。
    SkeletalPlayback baseline(asset);
    PlayMontage(baseline, montage, Targets(), false, 1, true);
    EXPECT_FLOAT_EQ(baseline.Time(), 0.0f);
    baseline.Tick(0.05f);
    ASSERT_EQ(baseline.FiredNotifies().size(), 1u);
    EXPECT_EQ(baseline.FiredNotifies()[0].name, "Footstep");
    EXPECT_EQ(baseline.FiredNotifies()[0].phase, "instant");
    EXPECT_EQ(baseline.MotionWarpState(), "FrontLedge:waiting");
    EXPECT_NEAR(baseline.RootMotionDelta().z(), 5.0f, 1e-3f);

    SkeletalPlayback player(asset);
    PlayMontage(player, montage, Targets(), false, 1, true, 0.877f);
    // 入口装配 = Play + 范围 + notify 表 + 一次初始 seek：时钟/范围都从 0.877 开始。
    EXPECT_FLOAT_EQ(player.Time(), 0.877f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.877f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 1.5f);
    EXPECT_FLOAT_EQ(player.Duration(), 1.5f - 0.877f);
    EXPECT_TRUE(player.FiredNotifies().empty());
    EXPECT_TRUE(player.ActiveNotifyStates().empty());

    player.Tick(0.05f);
    EXPECT_FLOAT_EQ(player.Time(), 0.927f);
    // 0.877 之前的事件不补发，跨过窗口起点也不产生 begin；窗口已结束后不再激活。
    EXPECT_TRUE(player.FiredNotifies().empty());
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
    EXPECT_EQ(player.MotionWarpState(), "FrontLedge:done");
    // 根位移只从入口起积分：0.05 s * 100 cm/s，而不是从 clip 0 起的 0.927 s。
    EXPECT_NEAR(player.RootMotionDelta().z(), 5.0f, 1e-3f);
    // 总根位移也以入口为积分起点：100 * (1.5 - 0.877)。
    EXPECT_NEAR(player.RootMotionTotal().z(), 62.3f, 0.01f);
    EXPECT_NEAR(player.RootMotionRemaining().z(), 100.0f * (1.5f - 0.927f), 0.01f);
}

TEST(AnimationScriptData, PlayMontageRejectsStartTimeOutsideSectionZero)
{
    auto asset = MakeAsset();
    asset->montages.push_back(MakeMontage());
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], Targets(), false, 1, true);
    player.Tick(0.1f);
    const float time = player.Time();
    const auto pose = player.Pose(0);

    const auto expectRejected = [&](float startTime) {
        const std::string message = ErrorOf([&] {
            PlayMontage(player, asset->montages[0], Targets(), false, 1, true, startTime); });
        EXPECT_FALSE(message.empty()) << "startTime " << startTime << " should be rejected";
        EXPECT_NE(message.find("AM_Test"), std::string::npos) << startTime;
        EXPECT_NE(message.find("startTime"), std::string::npos) << startTime;
        // 任何拒绝都不得改变当前播放器。
        EXPECT_FLOAT_EQ(player.Time(), time) << startTime;
        EXPECT_EQ(player.Pose(0), pose) << startTime;
        EXPECT_EQ(player.MontageId(), "AM_Test") << startTime;
    };
    expectRejected(std::nanf(""));
    expectRejected(std::numeric_limits<float>::infinity());
    expectRejected(-0.1f);
    // section 0 = [0,1.5]：段末本身与不足 1 ms 的余量都拒绝，诊断带范围与请求值。
    expectRejected(1.5f);
    expectRejected(1.4995f);
    const std::string message = ErrorOf([&] {
        PlayMontage(player, asset->montages[0], Targets(), false, 1, true, 1.6f); });
    EXPECT_NE(message.find("outside section 'Default' playable range [0.000000, 1.500000)"),
        std::string::npos);
    EXPECT_NE(message.find("1.600000"), std::string::npos);

    // section 0 从 0.2 开始时：显式 0.1 是越界请求；缺省 0 仍是“从 section 0 起点”，
    // 保持引入入口时间之前的旧调用行为。
    auto chainAsset = MakeAsset();
    chainAsset->montages.push_back(ChainMontage(
        {{"A", 0.2f, 0.6f, "B", 0.6f, "Absolute"}, {"B", 0.6f, 1.2f, "", 0, ""}}));
    SkeletalPlayback chain(chainAsset);
    const std::string beforeStart = ErrorOf([&] {
        PlayMontage(chain, chainAsset->montages[0], {}, false, 1, true, 0.1f); });
    EXPECT_NE(beforeStart.find("outside section 'A' playable range [0.200000, 0.600000)"),
        std::string::npos);
    PlayMontage(chain, chainAsset->montages[0], {}, false, 1, true);
    EXPECT_FLOAT_EQ(chain.Time(), 0.2f);
    EXPECT_FLOAT_EQ(chain.PlaybackRangeStart(), 0.2f);
}

TEST(AnimationScriptData, MontageSectionChainStartsFromEntryTime)
{
    auto asset = MakeAsset();
    // A [0,0.5] 连续链接到 B [0.5,1.5]；入口 0.25 = A 段中间。
    asset->montages.push_back(ChainMontage(
        {{"A", 0.0f, 0.5f, "B", 0.5f, "Absolute"}, {"B", 0.5f, 1.5f, "", 0, ""}},
        {{"notify", "1", "BeforeEntry", 0.1f, 0}, {"notify", "1", "AfterEntry", 0.3f, 0},
         {"notify", "1", "InB", 0.6f, 0}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true, 0.25f);
    EXPECT_EQ(player.MontageSectionIndex(), 0u);
    EXPECT_EQ(player.MontageSectionName(), "A");
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.25f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 0.5f);

    std::vector<FiredNotify> events;
    player.Tick(0.4f); // 0.25→0.5 走完 A，再推进 B 0.15
    CollectNotifies(player, events);
    // 入口之前的事件不补发；入口之后的照常触发一次。
    EXPECT_EQ(CountEvents(events, "BeforeEntry", "instant", false), 0u);
    EXPECT_EQ(CountEvents(events, "AfterEntry", "instant", false), 1u);
    EXPECT_EQ(CountEvents(events, "InB", "instant", false), 1u);
    EXPECT_EQ(player.MontageSectionIndex(), 1u);
    EXPECT_EQ(player.MontageSectionName(), "B");
    EXPECT_NEAR(player.Time(), 0.65f, 1e-5f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.5f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 1.5f);
    // 连续过渡不算跳变：0.4 s 全部计入根位移（0.4 * 100 cm/s）。
    EXPECT_NEAR(player.RootMotionDelta().z(), 40.0f, 0.01f);
}

TEST(AnimationScriptData, PlayMontageWritesWarpedYawIntoRootMotionChannel)
{
    auto asset = MakeAsset();
    auto& clip = asset->clips[0];
    // 根 yaw 90°/2s（45°/s）；Montage 窗口 [0.1,0.5] 内目标 90°。
    // 旋转键按 60 Hz 密采样：ozz 轨道用 NLerp，稀疏键会让 1/60 s 的样本偏离线性角度。
    clip.raw.tracks[0].translations = {{0, {0, 0, 0}}, {2, {0, 0, 0}}};
    clip.raw.tracks[0].rotations.clear();
    constexpr int rotationSteps = 120;
    for (int step = 0; step <= rotationSteps; ++step)
    {
        const float fraction = static_cast<float>(step) / rotationSteps;
        clip.raw.tracks[0].rotations.push_back({2.0f * fraction,
            ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3(0, 1, 0),
                1.57079632679489661923f * fraction)});
    }
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);

    Montage montage = MakeMontage();
    montage.notifies[1].warp->subtractRemainingRootMotion = false;
    montage.notifies[1].warp->rotationTimeMultiplier = 1.0f;
    asset->montages.push_back(montage);
    SkeletalPlayback player(asset);

    std::map<std::string, MotionWarpTarget> targets;
    targets["FrontLedge"].location = Eigen::Vector3f::Zero();
    targets["FrontLedge"].hasRotation = true;
    targets["FrontLedge"].rotation = Eigen::Quaternionf(
        Eigen::AngleAxisf(3.14159265358979323846f / 2.0f, Eigen::Vector3f::UnitY()));
    PlayMontage(player, asset->montages[0], targets, false, 1, true);

    constexpr float radiansToDegrees = 180.0f / 3.14159265358979323846f;
    const float step = 1.0f / 60.0f;
    Eigen::Matrix4f mesh = Eigen::Matrix4f::Identity();
    Eigen::Vector3f actor = Eigen::Vector3f::Zero();
    float actorYaw = 0;
    for (int tick = 1; tick <= 30; ++tick)
    {
        player.Tick(step, &mesh, &actor, actorYaw);
        actorYaw += player.RootMotionYawDelta();
    }
    // 30 tick 后 previous=0.5=窗口 End：最后一个激活 tick 的 alpha=1，Actor yaw 精确到 90°。
    EXPECT_NEAR(actorYaw * radiansToDegrees, 90.0f, 0.25f);
    // 窗口结束/中断后不再施加旋转：下一 tick 只剩 45°/s 的原始根 yaw。
    player.Tick(step, &mesh, &actor, actorYaw);
    EXPECT_NEAR(player.RootMotionYawDelta() * radiansToDegrees, 0.75f, 1e-3f);
}

TEST(AnimationScriptData, PlayMontageRejectsLoopAndMissingDerivedClip)
{
    auto asset = MakeAsset();
    asset->montages.push_back(MakeMontage());
    SkeletalPlayback player(asset);
    const std::string loop = ErrorOf([&] {
        PlayMontage(player, asset->montages[0], Targets(), true, 1, true); });
    EXPECT_NE(loop.find("looping"), std::string::npos);

    Montage missing = MakeMontage();
    missing.derivedClipId = "missing";
    const std::string message = ErrorOf([&] {
        PlayMontage(player, missing, Targets(), false, 1, true); });
    EXPECT_NE(message.find("AM_Test"), std::string::npos);
    EXPECT_NE(message.find("missing"), std::string::npos);
}

TEST(AnimationScriptData, FailedPlayMontageLeavesPlayerUntouched)
{
    auto asset = MakeAsset();
    asset->montages.push_back(MakeMontage());
    SkeletalPlayback player(asset);
    player.Play("walk", true, 1, false);
    player.Tick(0.25f);
    const float time = player.Time();
    const auto selected = player.SelectedClip();
    const auto montageId = player.MontageId();

    Montage broken = MakeMontage();
    broken.notifies[1].warp->easing = "Step";
    EXPECT_THROW(PlayMontage(player, broken, Targets(), false, 1, true), std::invalid_argument);
    EXPECT_FLOAT_EQ(player.Time(), time);
    EXPECT_EQ(player.SelectedClip(), selected);
    EXPECT_EQ(player.MontageId(), montageId);
    // 缺 target 同样在构造窗口时失败，不改变时钟。
    EXPECT_THROW(PlayMontage(player, asset->montages[0], {}, false, 1, true), std::invalid_argument);
    EXPECT_FLOAT_EQ(player.Time(), time);
    EXPECT_EQ(player.SelectedClip(), selected);
}

// ---- section 链播放 ----
// 链接公式见 ResolveMontageSectionLinkTime 与 UE FAnimLinkableElement（Absolute/Relative/
// Proportional）；UE 5.8 runtime 已不使用 NextTime，这里锁定 VL 数据层的 NextLink 语义。

TEST(AnimationScriptData, MontageSectionsStartAtSectionZeroAndFollowAbsoluteLink)
{
    auto asset = MakeAsset();
    asset->montages.push_back(ChainMontage({{"A", 0.0f, 0.5f, "B", 0.7f, "Absolute"},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);
    EXPECT_EQ(player.MontageSectionIndex(), 0u);
    EXPECT_EQ(player.MontageSectionName(), "A");
    EXPECT_FLOAT_EQ(player.Time(), 0.0f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.0f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 0.5f);
    EXPECT_FLOAT_EQ(player.Duration(), 0.5f);

    player.Tick(0.25f);
    EXPECT_FLOAT_EQ(player.Time(), 0.25f);
    EXPECT_EQ(player.MontageSectionIndex(), 0u);

    // 0.25 -> 0.5 结束 A；Absolute 的 0.7 是序列绝对时间，剩余 0.05 s 继续推进 B。
    player.Tick(0.3f);
    EXPECT_EQ(player.MontageSectionIndex(), 1u);
    EXPECT_EQ(player.MontageSectionName(), "B");
    EXPECT_NEAR(player.Time(), 0.75f, 1e-5f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.7f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 1.2f);
    // 根运动只累加实际移动区间（0.25→0.5 + 0.7→0.75 = 0.3 s），跳跃 0.5→0.7 不计。
    EXPECT_NEAR(player.RootMotionDelta().z(), 30.0f, 0.01f);

    player.Tick(0.5f);
    EXPECT_FLOAT_EQ(player.Time(), 1.2f);
    EXPECT_TRUE(player.Completed());
    EXPECT_FALSE(player.IsPlaying());
}

TEST(AnimationScriptData, MontageRelativeLinkIsRelativeToCurrentSectionStart)
{
    auto asset = MakeAsset();
    // Relative: A.start 0.2 + nextTime 0.5 = 0.7，正好是 B 的起点。
    asset->montages.push_back(ChainMontage({{"A", 0.2f, 0.6f, "B", 0.5f, "Relative"},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);
    EXPECT_FLOAT_EQ(player.Time(), 0.2f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.2f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeEnd(), 0.6f);

    player.Tick(0.5f); // 0.2 -> 0.6 结束后跳到 0.7，再走剩余 0.1
    EXPECT_EQ(player.MontageSectionIndex(), 1u);
    EXPECT_NEAR(player.Time(), 0.8f, 1e-5f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.7f);
}

TEST(AnimationScriptData, MontageRatioLinkScalesCurrentSectionLength)
{
    auto asset = MakeAsset();
    // Ratio: A.start 0.2 + 1.25 * (0.6-0.2) = 0.7。
    asset->montages.push_back(ChainMontage({{"A", 0.2f, 0.6f, "B", 1.25f, "Ratio"},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);
    player.Tick(0.5f);
    EXPECT_EQ(player.MontageSectionIndex(), 1u);
    EXPECT_NEAR(player.Time(), 0.8f, 1e-5f);
    EXPECT_FLOAT_EQ(player.PlaybackRangeStart(), 0.7f);
}

TEST(AnimationScriptData, MontageEmptyNextSectionStopsAndKeepsPose)
{
    auto asset = MakeAsset();
    asset->montages.push_back(ChainMontage({{"A", 0.0f, 0.5f, "", 0, ""},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);
    player.Tick(0.8f);
    EXPECT_FLOAT_EQ(player.Time(), 0.5f);
    EXPECT_EQ(player.MontageSectionIndex(), 0u);
    EXPECT_TRUE(player.Completed());
    EXPECT_FALSE(player.IsPlaying());
    // 停止后保留当前姿态/时钟，不再推进。
    player.Tick(0.2f);
    EXPECT_FLOAT_EQ(player.Time(), 0.5f);
}

TEST(AnimationScriptData, RejectsMontageLinkToUnknownSection)
{
    auto asset = MakeAsset();
    asset->montages.push_back(ChainMontage({{"A", 0.0f, 0.5f, "Missing", 0, "Absolute"},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    const std::string message = ErrorOf([&] {
        PlayMontage(player, asset->montages[0], {}, false, 1, true); });
    EXPECT_NE(message.find("AM_Chain"), std::string::npos);
    EXPECT_NE(message.find("section 'A'"), std::string::npos);
    EXPECT_NE(message.find("references unknown next section 'Missing'"), std::string::npos);
}

TEST(AnimationScriptData, RejectsUnknownMontageLinkMethod)
{
    auto asset = MakeAsset();
    // UE 的枚举名是 Proportional；VL 数据层用 Ratio，未知值必须拒绝而不是按 Absolute 猜。
    asset->montages.push_back(ChainMontage({{"A", 0.0f, 0.5f, "B", 0.7f, "Proportional"},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    const std::string message = ErrorOf([&] {
        PlayMontage(player, asset->montages[0], {}, false, 1, true); });
    EXPECT_NE(message.find("AM_Chain"), std::string::npos);
    EXPECT_NE(message.find("Proportional"), std::string::npos);
    EXPECT_NE(message.find("Absolute/Relative/Ratio"), std::string::npos);
}

TEST(AnimationScriptData, RejectsMontageLinkOutsideTargetSection)
{
    auto asset = MakeAsset();
    asset->montages.push_back(ChainMontage({{"A", 0.0f, 0.5f, "B", 1.5f, "Absolute"},
        {"B", 0.7f, 1.2f, "", 0, ""}}));
    SkeletalPlayback player(asset);
    const std::string message = ErrorOf([&] {
        PlayMontage(player, asset->montages[0], {}, false, 1, true); });
    EXPECT_NE(message.find("AM_Chain"), std::string::npos);
    EXPECT_NE(message.find("outside next section 'B'"), std::string::npos);
}

TEST(AnimationScriptData, MontageSectionTransitionFiresBoundaryNotifiesExactlyOnce)
{
    auto asset = MakeAsset();
    // Swing 的区间 [0.4,1.0] 跨越 A/B 边界；Edge 恰好落在边界 0.5 上。
    asset->montages.push_back(ChainMontage(
        {{"A", 0.0f, 0.5f, "B", 0.5f, "Absolute"}, {"B", 0.5f, 1.5f, "", 0, ""}},
        {{"notifyState", "1", "Swing", 0.4f, 0.6f}, {"notify", "1", "Edge", 0.5f, 0}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);

    std::vector<FiredNotify> events;
    for (int tick = 0; tick < 12; ++tick) // 0 -> 1.2
    {
        player.Tick(0.1f);
        CollectNotifies(player, events);
    }
    // 连续过渡（目标起点 == 段末）不算跳变：begin 一次、end 一次（未中断），边界即时事件
    // 只由下一段的 from 触发一次。
    EXPECT_EQ(CountEvents(events, "Swing", "begin", false), 1u);
    EXPECT_EQ(CountEvents(events, "Edge", "instant", false), 1u);
    EXPECT_EQ(CountEvents(events, "Swing", "end", false), 1u);
    EXPECT_EQ(CountEvents(events, "Swing", "end", true), 0u);
    for (const FiredNotify& event : events)
    {
        if (event.name == "Edge") EXPECT_FLOAT_EQ(event.time, 0.5f);
        if (event.name == "Swing" && event.phase == "end") EXPECT_FLOAT_EQ(event.time, 1.0f);
    }
    EXPECT_EQ(player.MontageSectionIndex(), 1u);
}

TEST(AnimationScriptData, MontageForwardGapSkipsNotifiesWithoutReplaying)
{
    auto asset = MakeAsset();
    // A 在 0.5 结束，链接到绝对 0.8：中间 (0.5,0.8) 的 InGap 被跳过，A/B 内的事件不重复。
    asset->montages.push_back(ChainMontage(
        {{"A", 0.0f, 0.5f, "B", 0.8f, "Absolute"}, {"B", 0.8f, 1.5f, "", 0, ""}},
        {{"notify", "1", "InA", 0.25f, 0}, {"notify", "1", "InA2", 0.3f, 0},
         {"notify", "1", "InGap", 0.6f, 0}, {"notify", "1", "InB", 0.85f, 0}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);

    std::vector<FiredNotify> events;
    player.Tick(0.9f); // 0 -> 0.5（A 段），跳到 0.8 后推进到 1.2
    CollectNotifies(player, events);
    EXPECT_EQ(CountEvents(events, "InA", "instant", false), 1u);
    EXPECT_EQ(CountEvents(events, "InA2", "instant", false), 1u);
    EXPECT_EQ(CountEvents(events, "InGap", "instant", false), 0u);
    EXPECT_EQ(CountEvents(events, "InB", "instant", false), 1u);
    EXPECT_NEAR(player.Time(), 1.2f, 1e-5f);

    for (int tick = 0; tick < 5; ++tick) // 1.2 -> 1.4，不得重放 A/边界前事件
    {
        player.Tick(0.1f);
        CollectNotifies(player, events);
    }
    EXPECT_EQ(CountEvents(events, "InA", "instant", false), 1u);
    EXPECT_EQ(CountEvents(events, "InB", "instant", false), 1u);
    EXPECT_EQ(player.MontageSectionIndex(), 1u);
}

TEST(AnimationScriptData, MontageForwardGapClosesNotifyStatesSkippedByTheJump)
{
    auto asset = MakeAsset();
    // Span 的作者区间 [0.2,0.6] 结束在被跳过的 (0.5,0.8) 里：不能永久留在活动表，
    // 下一次扫描以新段起点 0.8 补 end（事件时间取扫描起点，文档已注明）。
    asset->montages.push_back(ChainMontage(
        {{"A", 0.0f, 0.5f, "B", 0.8f, "Absolute"}, {"B", 0.8f, 1.5f, "", 0, ""}},
        {{"notifyState", "1", "Span", 0.2f, 0.4f}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);

    std::vector<FiredNotify> events;
    player.Tick(0.9f);
    CollectNotifies(player, events);
    EXPECT_EQ(CountEvents(events, "Span", "begin", false), 1u);
    EXPECT_EQ(CountEvents(events, "Span", "end", false), 1u);
    EXPECT_EQ(CountEvents(events, "Span", "end", true), 0u);
    for (const FiredNotify& event : events)
        if (event.name == "Span" && event.phase == "end") EXPECT_FLOAT_EQ(event.time, 0.8f);
    EXPECT_TRUE(player.ActiveNotifyStates().empty());
}

TEST(AnimationScriptData, MontageBackwardLinkEndsActiveStatesInterrupted)
{
    auto asset = MakeAsset();
    // B 结束回到 A 起点（section 循环）：回跳前按循环回绕语义结束活动区间。
    asset->montages.push_back(ChainMontage(
        {{"A", 0.0f, 0.5f, "B", 0.5f, "Absolute"},
         {"B", 0.5f, 1.0f, "A", 0.0f, "Absolute"}},
        {{"notifyState", "1", "LoopState", 0.2f, 1.3f}}));
    SkeletalPlayback player(asset);
    PlayMontage(player, asset->montages[0], {}, false, 1, true);

    std::vector<FiredNotify> events;
    for (int tick = 0; tick < 13; ++tick) // 0 -> 1.3：1.0 回跳到 0 后进入第二遍
    {
        player.Tick(0.1f);
        CollectNotifies(player, events);
    }
    EXPECT_EQ(CountEvents(events, "LoopState", "begin", false), 2u);
    ASSERT_EQ(CountEvents(events, "LoopState", "end", true), 1u);
    for (const FiredNotify& event : events)
        if (event.name == "LoopState" && event.phase == "end")
            EXPECT_FLOAT_EQ(event.time, 1.0f);
    EXPECT_EQ(player.MontageSectionIndex(), 0u); // 第二遍在 A 段内
}
}
