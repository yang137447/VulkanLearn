#include "animation/animationMontage.h"
#include "animation/animationSequenceData.h"
#include "animation/animationSkeleton.h"
#include "animation/skeletalPlayback.h"
#include "mesh/validation/meshAssetValidator.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace
{
using nlohmann::json;
using VL::Animation::BindMontage;
using VL::Animation::Montage;
using VL::Animation::MontageImportContext;
using VL::Animation::ParseMontage;
using VL::Animation::SequenceData;
using VL::Animation::SkeletonAsset;

std::string ReadFixtureText(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::path(VL_ANIMATION_MONTAGE_FIXTURE_DIR) / std::string(name);
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Montage fixture is not readable: " + path.string());
    }
    return std::string(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

json LoadMontageFixture(std::string_view name)
{
    return json::parse(ReadFixtureText(name));
}

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

json MakeMontageJson()
{
    return json{
        {"format", "vl.anim-montage.v1"},
        {"montage", "AM_Test"},
        {"sequence", "Maps/Test/walk.anim.json"},
        {"sequenceBlake3", std::string(64, 'a')},
        {"slot", "DefaultSlot"},
        {"length", 2.0f},
        {"sections", json::array({
            {{"name", "Default"}, {"startTime", 0.0f}, {"endTime", 1.0f},
             {"nextSection", "Loop"}, {"nextTime", 0.5f}, {"linkMethod", "Absolute"}},
            {{"name", "Loop"}, {"startTime", 1.0f}, {"endTime", 2.0f},
             {"nextSection", ""}, {"nextTime", 0.0f}, {"linkMethod", "Ratio"}}})},
        {"notifies", json::array({
            {{"kind", "notifyState"}, {"track", "Warp"}, {"name", "MotionWarping"},
             {"time", 0.1f}, {"duration", 0.5f},
             {"warp", {{"target", "FrontLedge"}, {"provider", "Bone"},
                 {"bone", "attach"}, {"subtractRemainingRootMotion", true},
                 {"ignoreZAxis", true}, {"warpRotation", true},
                 {"rotationTimeMultiplier", 1.5f}, {"easing", "EaseInOut"}}}},
            {{"kind", "notifyState"}, {"track", "Warp"}, {"name", "MotionWarping"},
             {"time", 1.0f}, {"duration", 0.25f},
             {"warp", {{"target", "BackFloor"}, {"provider", "Static"},
                 {"bone", ""}, {"subtractRemainingRootMotion", false},
                 {"ignoreZAxis", false}, {"warpRotation", false},
                 {"rotationTimeMultiplier", 1.0f}, {"easing", "Linear"},
                 {"staticPose", {{"location", json::array({1.0f, 2.0f, 3.0f})},
                     {"rotation", json::array({0.0f, 0.0f, 0.0f, 1.0f})}}}}}},
            {{"kind", "notify"}, {"track", "FX"}, {"name", "Footstep"},
             {"time", 1.5f}, {"duration", 0.0f}}})},
        {"source", {{"t3d", "AM_Test.copy"}}}
    };
}

SkeletonAsset MakeSkeleton()
{
    SkeletonAsset skeleton;
    skeleton.id = "SK_Test";
    skeleton.root = "root";
    skeleton.sourceDigest = std::string(64, 'a');
    skeleton.joints = {
        {"root", -1, Eigen::Matrix4f::Identity()},
        {"attach", 0, Eigen::Matrix4f::Identity()}};
    skeleton.slots = {"DefaultSlot"};
    return skeleton;
}

SequenceData MakeSequence()
{
    SequenceData sequence;
    sequence.id = "M_Walk";
    sequence.skeletonId = "SK_Test";
    sequence.skeletonDigest = std::string(64, 'a');
    return sequence;
}

MontageImportContext MakeContext(
    const SkeletonAsset& skeleton, const SequenceData& sequence)
{
    MontageImportContext context;
    context.skeleton = &skeleton;
    context.sequences.push_back(
        {"Maps/Test/walk.anim.json", &sequence, std::string(64, 'a')});
    context.clipIds = {"M_Walk"};
    return context;
}

TEST(AnimationMontage, ParsesGaspHurdleSectionsNotifiesAndWarpWindows)
{
    const Montage montage = ParseMontage(
        LoadMontageFixture("AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.json"),
        "Maps/SC_gasp/Animations/AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.json");

    EXPECT_EQ(montage.id, "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot");
    EXPECT_EQ(montage.sequencePath,
        "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.anim.json");
    EXPECT_EQ(montage.sequenceDigest,
        "76b521b6b502ed4e2bad1916dabbd6c2b9964541224a6bd7f6fde10968dd528d");
    EXPECT_EQ(montage.slot, "DefaultSlot");
    EXPECT_NEAR(montage.length, 1.666667f, 1e-6);
    // 导入期不派生 clip id、不计算文件摘要；两者由 loader 填充。
    EXPECT_TRUE(montage.derivedClipId.empty());
    EXPECT_TRUE(montage.sourceDigest.empty());
    EXPECT_FALSE(montage.sourceJson.empty());

    ASSERT_EQ(montage.sections.size(), 1u);
    EXPECT_EQ(montage.sections[0].name, "Default");
    EXPECT_FLOAT_EQ(montage.sections[0].startTime, 0.0f);
    EXPECT_NEAR(montage.sections[0].endTime, montage.length, 1e-6);
    EXPECT_TRUE(montage.sections[0].nextSection.empty());
    EXPECT_FLOAT_EQ(montage.sections[0].nextTime, 0.0f);
    EXPECT_TRUE(montage.sections[0].linkMethod.empty());

    ASSERT_EQ(montage.notifies.size(), 6u);
    // notify 顺序必须按数组保留（UE modifier/添加顺序），不能按时间或名字重排。
    EXPECT_EQ(montage.notifies[0].kind, "notifyState");
    EXPECT_EQ(montage.notifies[0].track, "2");
    EXPECT_EQ(montage.notifies[0].name, "PoseSearchBranchIn");
    EXPECT_FLOAT_EQ(montage.notifies[0].duration, 0.074591f);
    EXPECT_FALSE(montage.notifies[0].warp.has_value());
    EXPECT_EQ(montage.notifies[1].name, "MotionWarping");
    EXPECT_EQ(montage.notifies[5].name, "BP_NotifyState_TraversalBlendOut_C");
    EXPECT_FLOAT_EQ(montage.notifies[5].time, 1.017674f);
    EXPECT_FLOAT_EQ(montage.notifies[5].duration, 0.648993f);

    ASSERT_EQ(montage.windows.size(), 4u);
    EXPECT_EQ(montage.windows[0].target, "FrontLedge");
    EXPECT_FLOAT_EQ(montage.windows[0].startTime, 0.078149f);
    EXPECT_NEAR(montage.windows[0].endTime, 0.234903f, 1e-6);
    EXPECT_EQ(montage.windows[1].target, "FrontLedge");
    EXPECT_FLOAT_EQ(montage.windows[1].startTime, 0.234925f);
    EXPECT_NEAR(montage.windows[1].endTime, 0.398904f, 1e-6);
    EXPECT_EQ(montage.windows[2].target, "BackLedge");
    EXPECT_NEAR(montage.windows[2].endTime, 0.598016f, 1e-6);
    EXPECT_EQ(montage.windows[3].target, "BackFloor");
    EXPECT_FLOAT_EQ(montage.windows[3].startTime, 0.599556f);
    EXPECT_NEAR(montage.windows[3].endTime, 0.831335f, 1e-6);

    const auto& frontLedge = *montage.notifies[1].warp;
    EXPECT_EQ(frontLedge.provider, "Bone");
    EXPECT_EQ(frontLedge.bone, "attach");
    EXPECT_FALSE(frontLedge.subtractRemainingRootMotion);
    EXPECT_FALSE(frontLedge.ignoreZAxis);
    EXPECT_TRUE(frontLedge.warpRotation);
    EXPECT_FLOAT_EQ(frontLedge.rotationTimeMultiplier, 1.0f);
    EXPECT_EQ(frontLedge.easing, "Linear");

    const auto& backFloor = *montage.notifies[4].warp;
    EXPECT_EQ(backFloor.provider, "None");
    EXPECT_TRUE(backFloor.bone.empty());
    EXPECT_FALSE(backFloor.warpRotation);
    EXPECT_EQ(backFloor.target, "BackFloor");
}

TEST(AnimationMontage, CountsMantleAndVaultWarpWindows)
{
    struct Case
    {
        const char* file;
        size_t windows;
    };
    // GASP traversal 真实资产：Mantle 三种速度各 2 个窗口；Vault stand 3 个，
    // Vault run/walk 各 2 个。这里锁定数量，防止导入期丢窗口或按时间重排。
    const Case cases[] = {
        {"AM_M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot.json", 2},
        {"AM_M_Neutral_Traversal_Mantle_1_0_run_F_Lfoot.json", 2},
        {"AM_M_Neutral_Traversal_Mantle_1_0_walk_F_Lfoot.json", 2},
        {"AM_M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot.json", 3},
        {"AM_M_Neutral_Traversal_Vault_1_0_run_F_Lfoot.json", 2},
        {"AM_M_Neutral_Traversal_Vault_1_0_walk_F_Rfoot.json", 2}};
    for (const Case& testCase : cases)
    {
        const Montage montage = ParseMontage(
            LoadMontageFixture(testCase.file),
            std::string("Maps/SC_gasp/Animations/") + testCase.file);
        EXPECT_EQ(montage.windows.size(), testCase.windows) << testCase.file;
        size_t warpNotifies = 0;
        for (const auto& notify : montage.notifies)
            if (notify.warp.has_value())
                ++warpNotifies;
        EXPECT_EQ(warpNotifies, testCase.windows) << testCase.file;
    }
}

TEST(AnimationMontage, ParsesEveryGaspTraversalMontageFixture)
{
    // 资源仓库现有的 10 个 traversal Montage 全部过一遍结构校验；
    // 防止只测 Hurdle/Mantle/Vault 时漏掉 Climb 的 provider/section 形状。
    const char* files[] = {
        "AM_M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Climb_Start_2_5_walk_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.json",
        "AM_M_Neutral_Traversal_Mantle_1_0_run_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Mantle_1_0_walk_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Vault_1_0_run_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot.json",
        "AM_M_Neutral_Traversal_Vault_1_0_walk_F_Rfoot.json"};
    for (const char* file : files)
    {
        SCOPED_TRACE(file);
        const Montage montage = ParseMontage(LoadMontageFixture(file),
            std::string("Maps/SC_gasp/Animations/") + file);
        EXPECT_FALSE(montage.id.empty());
        EXPECT_EQ(montage.slot, "DefaultSlot");
        EXPECT_FALSE(montage.sequencePath.empty());
        EXPECT_GT(montage.length, 0.0f);
        EXPECT_FALSE(montage.sections.empty());
        EXPECT_FALSE(montage.windows.empty());
    }
}

TEST(AnimationMontage, RetainsMultipleSectionsLinkMethodsStaticPoseAndNonLinearEasing)
{
    const Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");

    ASSERT_EQ(montage.sections.size(), 2u);
    EXPECT_EQ(montage.sections[0].nextSection, "Loop");
    EXPECT_FLOAT_EQ(montage.sections[0].nextTime, 0.5f);
    EXPECT_EQ(montage.sections[0].linkMethod, "Absolute");
    EXPECT_EQ(montage.sections[1].linkMethod, "Ratio");
    EXPECT_TRUE(montage.sections[1].nextSection.empty());

    ASSERT_EQ(montage.notifies.size(), 3u);
    ASSERT_TRUE(montage.notifies[0].warp.has_value());
    EXPECT_EQ(montage.notifies[0].warp->easing, "EaseInOut");
    EXPECT_FLOAT_EQ(montage.notifies[0].warp->rotationTimeMultiplier, 1.5f);
    EXPECT_TRUE(montage.notifies[0].warp->subtractRemainingRootMotion);
    EXPECT_TRUE(montage.notifies[0].warp->ignoreZAxis);
    ASSERT_TRUE(montage.notifies[1].warp.has_value());
    EXPECT_EQ(montage.notifies[1].warp->provider, "Static");
    ASSERT_TRUE(montage.notifies[1].warp->staticPose.has_value());
    EXPECT_FLOAT_EQ(montage.notifies[1].warp->staticPose->location[0], 1.0f);
    EXPECT_FLOAT_EQ(montage.notifies[1].warp->staticPose->location[1], 2.0f);
    EXPECT_FLOAT_EQ(montage.notifies[1].warp->staticPose->location[2], 3.0f);
    EXPECT_FLOAT_EQ(montage.notifies[1].warp->staticPose->rotation[3], 1.0f);
    EXPECT_FALSE(montage.notifies[2].warp.has_value());

    // 多 section/link、Static、非 Linear easing 都是“保留不拒绝”，绑定校验也应通过。
    const SkeletonAsset skeleton = MakeSkeleton();
    const SequenceData sequence = MakeSequence();
    const MontageImportContext context = MakeContext(skeleton, sequence);
    EXPECT_EQ(BindMontage(montage, context, "Maps/Test/AM_Test.json"), "M_Walk");
}

TEST(AnimationMontage, RotationMethodDefaultsToSlerpAndPreservesKnownValues)
{
    // 旧资产没有 rotationMethod；缺省必须是 UE 默认 Slerp（最短路径球面插值）。
    json source = MakeMontageJson();
    Montage montage = ParseMontage(source, "Maps/Test/AM_Test.json");
    ASSERT_TRUE(montage.notifies[0].warp.has_value());
    EXPECT_EQ(montage.notifies[0].warp->rotationMethod, "Slerp");

    // 数据层保留其它 UE method（执行层再拒绝），不做静默降级。
    source["notifies"][0]["warp"]["rotationMethod"] = "ConstantRate";
    montage = ParseMontage(source, "Maps/Test/AM_Test.json");
    EXPECT_EQ(montage.notifies[0].warp->rotationMethod, "ConstantRate");

    source["notifies"][0]["warp"]["rotationMethod"] = 3;
    EXPECT_NE(ErrorOf([&] { ParseMontage(source, "Maps/Test/AM_Test.json"); })
        .find("rotationMethod"), std::string::npos);
}

TEST(AnimationMontage, RetainsNotifyStateDurationBeyondMontageLength)
{
    // UE notifyState 的 duration 独立于 Montage 长度：真实 Mantle run 的 BlendOut
    // 从 0.732103 持续 1.767896，末尾超过 length 0.266666；必须原样保留、不裁剪。
    const Montage montage = ParseMontage(
        LoadMontageFixture("AM_M_Neutral_Traversal_Mantle_1_0_run_F_Lfoot.json"),
        "Maps/SC_gasp/Animations/AM_M_Neutral_Traversal_Mantle_1_0_run_F_Lfoot.json");

    ASSERT_EQ(montage.notifies.size(), 4u);
    const auto& blendOut = montage.notifies[3];
    EXPECT_EQ(blendOut.kind, "notifyState");
    EXPECT_EQ(blendOut.name, "BP_NotifyState_TraversalBlendOut_C");
    EXPECT_FLOAT_EQ(blendOut.time, 0.732103f);
    EXPECT_FLOAT_EQ(blendOut.duration, 1.767896f);
    EXPECT_FALSE(blendOut.warp.has_value());
    EXPECT_GT(static_cast<double>(blendOut.time) +
        static_cast<double>(blendOut.duration), static_cast<double>(montage.length));
    // 只有带 warp 的 notifyState 才派生播放窗口；越过末尾的普通 notifyState 不算窗口。
    EXPECT_EQ(montage.windows.size(), 2u);
}

TEST(AnimationMontage, RejectsSectionOutsideMontageLength)
{
    json source = MakeMontageJson();
    source["sections"][1]["endTime"] = 2.5f;
    const std::string message =
        ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("sections[1].endTime"), std::string::npos);
    EXPECT_NE(message.find("montage length"), std::string::npos);

    source = MakeMontageJson();
    source["sections"][0]["startTime"] = -0.1f;
    EXPECT_NE(ErrorOf([&] { ParseMontage(source, "AM_bad.json"); })
        .find("sections[0].startTime"), std::string::npos);

    source = MakeMontageJson();
    source["sections"][0]["endTime"] = 0.0f;
    EXPECT_NE(ErrorOf([&] { ParseMontage(source, "AM_bad.json"); })
        .find("greater than startTime"), std::string::npos);
}

TEST(AnimationMontage, RejectsUnknownNextSectionReference)
{
    json source = MakeMontageJson();
    source["sections"][0]["nextSection"] = "Missing";
    const std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("sections[0].nextSection"), std::string::npos);
    EXPECT_NE(message.find("Missing"), std::string::npos);
}

TEST(AnimationMontage, RejectsUnknownWarpProvider)
{
    json source = MakeMontageJson();
    source["notifies"][0]["warp"]["provider"] = "Model";
    const std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("notifies[0].warp.provider"), std::string::npos);
    EXPECT_NE(message.find("None/Static/Bone"), std::string::npos);
}

TEST(AnimationMontage, RejectsUnknownEasingAndLinkMethod)
{
    json source = MakeMontageJson();
    source["notifies"][0]["warp"]["easing"] = "Bogus";
    std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("notifies[0].warp.easing"), std::string::npos);
    EXPECT_NE(message.find("Bogus"), std::string::npos);

    source = MakeMontageJson();
    source["sections"][0]["linkMethod"] = "Bogus";
    message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("sections[0].linkMethod"), std::string::npos);
    EXPECT_NE(message.find("Bogus"), std::string::npos);
}

TEST(AnimationMontage, RejectsUnknownNotifyKind)
{
    json source = MakeMontageJson();
    source["notifies"][2]["kind"] = "notifyWindow";
    const std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("notifies[2].kind"), std::string::npos);
    EXPECT_NE(message.find("notify"), std::string::npos);
}

TEST(AnimationMontage, RejectsBoneProviderWithoutBoneName)
{
    json source = MakeMontageJson();
    source["notifies"][0]["warp"]["bone"] = "";
    const std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("notifies[0].warp.bone"), std::string::npos);
    EXPECT_NE(message.find("Bone"), std::string::npos);
}

TEST(AnimationMontage, RejectsStaticProviderWithoutStaticPose)
{
    json source = MakeMontageJson();
    source["notifies"][1]["warp"].erase("staticPose");
    const std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("notifies[1].warp.staticPose"), std::string::npos);
    EXPECT_NE(message.find("Static"), std::string::npos);
}

TEST(AnimationMontage, RejectsWarpWindowOutsideMontageLength)
{
    json source = MakeMontageJson();
    source["notifies"][0]["time"] = 1.8f;
    source["notifies"][0]["duration"] = 0.5f;
    const std::string message = ErrorOf([&] { ParseMontage(source, "AM_bad.json"); });
    EXPECT_NE(message.find("notifies[0].duration"), std::string::npos);
    EXPECT_NE(message.find("time+duration"), std::string::npos);
}

TEST(AnimationMontage, RejectsSequencePathThatIsNotALoadedSidecar)
{
    const Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");
    const SkeletonAsset skeleton = MakeSkeleton();
    const SequenceData sequence = MakeSequence();
    MontageImportContext context = MakeContext(skeleton, sequence);
    context.sequences[0].path = "Maps/Test/other.anim.json";
    const std::string message =
        ErrorOf([&] { BindMontage(montage, context, "Maps/Test/AM_Test.json"); });
    EXPECT_NE(message.find("sequence"), std::string::npos);
    EXPECT_NE(message.find("not a loaded sequence sidecar"), std::string::npos);
}

TEST(AnimationMontage, RejectsSequenceDigestMismatch)
{
    const Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");
    const SkeletonAsset skeleton = MakeSkeleton();
    const SequenceData sequence = MakeSequence();
    MontageImportContext context = MakeContext(skeleton, sequence);
    context.sequences[0].digest = std::string(64, 'b');
    const std::string message =
        ErrorOf([&] { BindMontage(montage, context, "Maps/Test/AM_Test.json"); });
    EXPECT_NE(message.find("sequenceBlake3"), std::string::npos);
    EXPECT_NE(message.find("does not match sidecar digest"), std::string::npos);
}

TEST(AnimationMontage, RejectsSlotNotRegisteredInSkeleton)
{
    const Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");
    SkeletonAsset skeleton = MakeSkeleton();
    skeleton.slots = {"OtherSlot"};
    const SequenceData sequence = MakeSequence();
    const MontageImportContext context = MakeContext(skeleton, sequence);
    const std::string message =
        ErrorOf([&] { BindMontage(montage, context, "Maps/Test/AM_Test.json"); });
    EXPECT_NE(message.find("slot"), std::string::npos);
    EXPECT_NE(message.find("DefaultSlot"), std::string::npos);
    EXPECT_NE(message.find("SK_Test"), std::string::npos);
}

TEST(AnimationMontage, RejectsWarpBoneMissingFromSkeleton)
{
    const Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");
    SkeletonAsset skeleton = MakeSkeleton();
    skeleton.joints = {{"root", -1, Eigen::Matrix4f::Identity()}};
    const SequenceData sequence = MakeSequence();
    const MontageImportContext context = MakeContext(skeleton, sequence);
    const std::string message =
        ErrorOf([&] { BindMontage(montage, context, "Maps/Test/AM_Test.json"); });
    EXPECT_NE(message.find("notifies[0].warp.bone"), std::string::npos);
    EXPECT_NE(message.find("attach"), std::string::npos);
}

TEST(AnimationMontage, RejectsDerivedClipIdNotDeclaredInMesh)
{
    const Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");
    const SkeletonAsset skeleton = MakeSkeleton();
    const SequenceData sequence = MakeSequence();
    MontageImportContext context = MakeContext(skeleton, sequence);
    context.clipIds = {"M_Other"};
    const std::string message =
        ErrorOf([&] { BindMontage(montage, context, "Maps/Test/AM_Test.json"); });
    EXPECT_NE(message.find("derived clip id"), std::string::npos);
    EXPECT_NE(message.find("M_Walk"), std::string::npos);
    EXPECT_NE(message.find("skinning.clips"), std::string::npos);
}

TEST(AnimationMontage, RejectsSequenceFromAnotherSkeleton)
{
    Montage montage = ParseMontage(MakeMontageJson(), "Maps/Test/AM_Test.json");
    const SkeletonAsset skeleton = MakeSkeleton();
    SequenceData sequence = MakeSequence();
    sequence.skeletonId = "SK_Other";
    const MontageImportContext context = MakeContext(skeleton, sequence);
    const std::string message =
        ErrorOf([&] { BindMontage(montage, context, "Maps/Test/AM_Test.json"); });
    EXPECT_NE(message.find("SK_Other"), std::string::npos);
    EXPECT_NE(message.find("does not match loaded skeleton"), std::string::npos);

    // Skinning 摘要不一致同样拒绝，防止 Montage 与另一代 SK 串配。
    sequence = MakeSequence();
    sequence.skeletonDigest = std::string(64, 'c');
    const MontageImportContext staleContext = MakeContext(skeleton, sequence);
    EXPECT_NE(ErrorOf([&] {
        BindMontage(montage, staleContext, "Maps/Test/AM_Test.json"); })
        .find("skeletonBlake3"), std::string::npos);
}

TEST(AnimationMontage, FindsMontageByIdOrResourcePath)
{
    VL::Animation::SkeletalAsset asset;
    VL::Animation::Montage montage;
    montage.id = "AM_Test";
    montage.sourcePath = "Maps/Test/Animations/AM_Test.json";
    asset.montages.push_back(montage);

    EXPECT_EQ(asset.FindMontage("AM_Test"), &asset.montages[0]);
    EXPECT_EQ(asset.FindMontage("Maps/Test/Animations/AM_Test.json"), &asset.montages[0]);
    EXPECT_EQ(asset.FindMontage("AM_Missing"), nullptr);
}

TEST(AnimationMontage, MeshPlanWithoutMontagesKeepsSkeletonOnlyCacheKey)
{
    const json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"skeleton", "Maps/Test/Skeletons/SK_body.json"},
            {"clips", json::array({{{"id", "walk"}, {"path", "Maps/Test/walk.fbx"}}})}}}};

    const MeshAssetBuildPlan plan =
        MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_body.json", meshAsset);
    EXPECT_TRUE(plan.animationMontages.empty());
    // 无 montage 只保留 SK/clip 段；旧 |skinning= 段已随 root 字段删除。
    EXPECT_EQ(plan.modelCacheKey,
        "Maps/Test/SM_body.fbx|skeleton=Maps/Test/Skeletons/SK_body.json"
        "|clip=walk:Maps/Test/walk.fbx");
}

TEST(AnimationMontage, MontagePathExtendsPlanAndCacheKey)
{
    const json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"skeleton", "Maps/Test/Skeletons/SK_body.json"},
            {"clips", json::array({{{"id", "walk"},
                {"path", "Maps/Test/walk.fbx"},
                {"sequence", "Maps/Test/walk.anim.json"}}})},
            {"montages", json::array({{{"id", "AM_walk"},
                {"path", "Maps/Test/Animations/AM_walk.json"}}})}}}};

    const MeshAssetBuildPlan plan =
        MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_body.json", meshAsset);
    ASSERT_EQ(plan.animationMontages.size(), 1u);
    EXPECT_EQ(plan.animationMontages[0].id, "AM_walk");
    EXPECT_EQ(plan.animationMontages[0].path, "Maps/Test/Animations/AM_walk.json");
    EXPECT_EQ(plan.modelCacheKey,
        "Maps/Test/SM_body.fbx|skeleton=Maps/Test/Skeletons/SK_body.json"
        "|clip=walk:Maps/Test/walk.fbx"
        "|sequence=walk:Maps/Test/walk.anim.json"
        "|montage=Maps/Test/Animations/AM_walk.json");
}

TEST(AnimationMontage, RejectsMontagesWithoutSkeletonAsset)
{
    const json meshAsset = {
        {"type", "mesh"},
        {"modelDataPath", "Maps/Test/SM_body.fbx"},
        {"materialSlots", json::array({
            {{"name", "Default"}, {"materialInstancePath", "Maps/Test/MI_default.json"}}})},
        {"skinning", {
            {"clips", json::array({{{"id", "walk"}, {"path", "Maps/Test/walk.fbx"}}})},
            {"montages", json::array({{{"id", "AM_walk"},
                {"path", "Maps/Test/Animations/AM_walk.json"}}})}}}};

    const std::string message = ErrorOf([&] {
        MeshAssetValidator::BuildLoadPlan("Maps/Test/SM_body.json", meshAsset); });
    EXPECT_NE(message.find("skinning.skeleton"), std::string::npos);
    EXPECT_NE(message.find("Maps/Test/SM_body.json"), std::string::npos);
}
}
