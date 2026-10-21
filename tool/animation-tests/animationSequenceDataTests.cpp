#include "animation/animationSequenceData.h"
#include "animation/animationSkeleton.h"
#include "animation/animationAssets.h"
#include "animation/skeletalPlayback.h"

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <gtest/gtest.h>

namespace
{
using nlohmann::json;
using VL::Animation::FloatCurve;
using VL::Animation::ParseSequence;
using VL::Animation::SampleFloatCurve;
using VL::Animation::SampleFloatCurveOrThrow;
using VL::Animation::SequenceData;
using VL::Animation::SkeletonAsset;

json MakeSequenceJson()
{
    return json{
        {"format", "vl.anim-sequence.v1"},
        {"sequence", "M_Test"},
        {"skeleton", "SK_Test"},
        {"skeletonBlake3", std::string(64, 'a')},
        {"duration", 1.6666666f},
        {"frameRate", {{"numerator", 30}, {"denominator", 1}}},
        {"numFrames", 50},
        {"numKeys", 51},
        {"rateScale", 1.0},
        {"rootMotion", {
            {"enableRootMotion", true},
            {"forceRootLock", true},
            {"rootMotionRootLock", "RefPose"},
            {"additiveAnimType", "None"}}},
        {"tracks", json::array({"root", "pelvis"})},
        {"sampling", {{"interpolation", "linear"}, {"outside", "clamp"}}},
        {"floatCurveKeys", json::array({
            {{"name", "distance_from_ledge"},
             {"times", json::array({0.0f, 0.5f, 1.0f})},
             {"values", json::array({1.0f, 2.0f, 3.0f})}}})},
        {"notifies", json::array({
            {{"track", "Footstep"}, {"name", "Footstep Left"},
             {"time", 0.25f}, {"duration", 0.0f}}})},
        {"syncMarkers", json::array()},
        {"source", {{"fbx", "M_Test.fbx"}}}
    };
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

SkeletonAsset MakeSkeleton()
{
    SkeletonAsset skeleton;
    skeleton.id = "SK_Test";
    skeleton.root = "root";
    skeleton.sourceDigest = std::string(64, 'a');
    skeleton.joints = {
        {"root", -1, Eigen::Matrix4f::Identity()},
        {"pelvis", 0, Eigen::Matrix4f::Identity()}};
    skeleton.slots = {"DefaultSlot"};
    skeleton.curves = {{"distance_from_ledge", "Float"}};
    return skeleton;
}

SequenceData CurveSequence(const std::vector<float>& times,
    const std::vector<float>& values)
{
    SequenceData sequence;
    sequence.id = "M_Test";
    sequence.curves.push_back(FloatCurve{"Phase", times, values});
    return sequence;
}

json GaspHurdleSequenceJson()
{
    // 自造 30 Hz 夹具：51 个键覆盖 50 帧；曲线按 UE 参考点
    // (0.598017, 34.098671) / (0.831335, 90.908951) 定义的线性函数采样。
    constexpr double t0 = 0.598017;
    constexpr double t1 = 0.831335;
    constexpr double v0 = 34.098671;
    constexpr double v1 = 90.908951;
    const double slope = (v1 - v0) / (t1 - t0);
    const double intercept = v0 - slope * t0;

    json times = json::array();
    json values = json::array();
    for (int frame = 0; frame <= 50; ++frame)
    {
        const double time = static_cast<double>(frame) / 30.0;
        times.push_back(time);
        values.push_back(slope * time + intercept);
    }

    json source = MakeSequenceJson();
    source["sequence"] = "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot";
    source["duration"] = 50.0 / 30.0;
    source["numFrames"] = 50;
    source["numKeys"] = 51;
    source["tracks"] = json::array({"root"});
    source["floatCurveKeys"] = json::array({
        {{"name", "distance_from_ledge"}, {"times", times}, {"values", values}}});
    source["notifies"] = json::array();
    return source;
}

TEST(AnimationSequenceData, ParsesFrameRateCurvesNotifiesAndRetainedData)
{
    const SequenceData sequence = ParseSequence(
        MakeSequenceJson(), "Maps/Test/walk.anim.json");

    EXPECT_EQ(sequence.id, "M_Test");
    EXPECT_EQ(sequence.skeletonId, "SK_Test");
    EXPECT_EQ(sequence.skeletonDigest, std::string(64, 'a'));
    EXPECT_FLOAT_EQ(sequence.duration, 1.6666666f);
    EXPECT_EQ(sequence.frameRateNumerator, 30);
    EXPECT_EQ(sequence.frameRateDenominator, 1);
    EXPECT_EQ(sequence.numFrames, 50);
    EXPECT_EQ(sequence.numKeys, 51);
    EXPECT_FLOAT_EQ(sequence.rateScale, 1.0f);
    EXPECT_TRUE(sequence.enableRootMotion);
    EXPECT_TRUE(sequence.forceRootLock);
    EXPECT_EQ(sequence.rootMotionRootLock, "RefPose");
    EXPECT_EQ(sequence.additiveAnimType, "None");
    EXPECT_EQ(sequence.tracks, (std::vector<std::string>{"root", "pelvis"}));
    ASSERT_EQ(sequence.curves.size(), 1u);
    EXPECT_EQ(sequence.curves[0].name, "distance_from_ledge");
    EXPECT_EQ(sequence.curves[0].times, (std::vector<float>{0.0f, 0.5f, 1.0f}));
    ASSERT_EQ(sequence.notifies.size(), 1u);
    EXPECT_EQ(sequence.notifies[0].name, "Footstep Left");
    EXPECT_FLOAT_EQ(sequence.notifies[0].time, 0.25f);
    EXPECT_EQ(sequence.syncMarkersJson, "[]");
    EXPECT_FALSE(sequence.sourceJson.empty());
    EXPECT_TRUE(sequence.sourceDigest.empty());
}

TEST(AnimationSequenceData, RejectsWrongFormatAndNonPositiveDuration)
{
    json source = MakeSequenceJson();
    source["format"] = "vl.anim-sequence.v0";
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("format"),
        std::string::npos);

    for (const float duration : {0.0f, -1.0f,
        std::numeric_limits<float>::quiet_NaN()})
    {
        source = MakeSequenceJson();
        source["duration"] = duration;
        EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("duration"),
            std::string::npos);
    }
}

TEST(AnimationSequenceData, RejectsFrameRateDurationMismatch)
{
    json source = MakeSequenceJson();
    source["numFrames"] = 40;
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("numFrames"),
        std::string::npos);
}

TEST(AnimationSequenceData, RejectsInvalidCurveTimesValuesAndNames)
{
    json source = MakeSequenceJson();
    source["floatCurveKeys"][0]["times"] = json::array({0.0f, 0.5f, 0.5f});
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("strictly greater"),
        std::string::npos);

    source = MakeSequenceJson();
    source["floatCurveKeys"][0]["times"] = json::array({0.0f, 0.5f});
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("equal length"),
        std::string::npos);

    source = MakeSequenceJson();
    source["floatCurveKeys"][0]["times"] = json::array({0.0f, 1001.0f, 1002.0f});
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("[-1e3, 1e3]"),
        std::string::npos);

    source = MakeSequenceJson();
    source["floatCurveKeys"][0]["values"][1] =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("finite"),
        std::string::npos);

    source = MakeSequenceJson();
    source["floatCurveKeys"][0]["times"] = json::array();
    source["floatCurveKeys"][0]["values"] = json::array();
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("at least one key"),
        std::string::npos);

    source = MakeSequenceJson();
    source["floatCurveKeys"][0]["name"] = "";
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("must not be empty"),
        std::string::npos);
}

TEST(AnimationSequenceData, RejectsDuplicateCurveNamesIgnoringCase)
{
    json source = MakeSequenceJson();
    source["floatCurveKeys"].push_back(
        {{"name", "Distance_From_Ledge"},
         {"times", json::array({0.0f})},
         {"values", json::array({0.0f})}});
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("case-insensitively"),
        std::string::npos);
}

TEST(AnimationSequenceData, RejectsInvalidNotifies)
{
    json source = MakeSequenceJson();
    source["notifies"][0]["time"] = -0.1f;
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("notifies[0].time"),
        std::string::npos);

    source = MakeSequenceJson();
    source["notifies"][0]["time"] = 10.0f;
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("notifies[0].time"),
        std::string::npos);

    source = MakeSequenceJson();
    source["notifies"][0]["duration"] = -0.1f;
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("notifies[0].duration"),
        std::string::npos);

    source = MakeSequenceJson();
    source["notifies"][0]["time"] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("finite"),
        std::string::npos);

    source = MakeSequenceJson();
    source["notifies"][0]["duration"] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("finite"),
        std::string::npos);
}

TEST(AnimationSequenceData, PreservesNotifyStateIntervalBeyondSequenceEnd)
{
    // M_Neutral_Jump_F_Land_Stand_Light_Lfoot 的真实 notifyState：
    // 序列 2.766667 s，time=0.532511532，duration=2.467488527，end=3.0 > 序列时长。
    // UE notifyState 的 duration 独立于序列长度，这里必须原样保留而非裁剪。
    json source = MakeSequenceJson();
    source["duration"] = 2.766667f;
    source["numFrames"] = 83;
    source["numKeys"] = 84;
    source["notifies"] = json::array({
        {{"track", "PoseSearch"},
         {"name", "PoseSearchBlockTransition"},
         {"time", 0.532511532f},
         {"duration", 2.467488527f}}});

    const SequenceData sequence = ParseSequence(source, "jump_land.anim.json");
    ASSERT_EQ(sequence.notifies.size(), 1u);
    const auto& notify = sequence.notifies[0];
    EXPECT_EQ(notify.name, "PoseSearchBlockTransition");
    // 逐位保留原始 float；不做按片段时长的 clamp/裁剪。
    EXPECT_EQ(notify.time, 0.532511532f);
    EXPECT_EQ(notify.duration, 2.467488527f);
    EXPECT_NEAR(notify.EndTime(), 3.0f, 1e-5f);
    EXPECT_GT(notify.EndTime(), sequence.duration);
}

TEST(AnimationSequenceData, PreservesKnownAdditiveValuesAndRejectsUnknownEnums)
{
    json source = MakeSequenceJson();
    source["rootMotion"]["additiveAnimType"] = "LocalSpace";
    EXPECT_EQ(ParseSequence(source, "walk.anim.json").additiveAnimType, "LocalSpace");

    source["rootMotion"]["additiveAnimType"] = "Bogus";
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("additive"),
        std::string::npos);

    source = MakeSequenceJson();
    source["rootMotion"]["rootMotionRootLock"] = "Bogus";
    EXPECT_NE(ErrorOf([&] { ParseSequence(source, "walk.anim.json"); }).find("root motion"),
        std::string::npos);
}

TEST(AnimationSequenceData, RejectsSkeletonMismatchUnregisteredCurvesAndUnknownTracks)
{
    const SkeletonAsset skeleton = MakeSkeleton();
    SequenceData sequence = ParseSequence(MakeSequenceJson(), "walk.anim.json");
    EXPECT_NO_THROW(VL::Animation::ValidateSequenceAgainstSkeleton(
        sequence, skeleton, "walk.anim.json"));

    sequence.skeletonId = "SK_Other";
    EXPECT_NE(ErrorOf([&] { VL::Animation::ValidateSequenceAgainstSkeleton(
        sequence, skeleton, "walk.anim.json"); }).find("does not match"), std::string::npos);

    sequence = ParseSequence(MakeSequenceJson(), "walk.anim.json");
    sequence.skeletonDigest = std::string(64, 'b');
    EXPECT_NE(ErrorOf([&] { VL::Animation::ValidateSequenceAgainstSkeleton(
        sequence, skeleton, "walk.anim.json"); }).find("skeletonBlake3"), std::string::npos);

    sequence = ParseSequence(MakeSequenceJson(), "walk.anim.json");
    sequence.curves[0].name = "contact_l";
    const std::string message = ErrorOf([&] {
        VL::Animation::ValidateSequenceAgainstSkeleton(
            sequence, skeleton, "walk.anim.json"); });
    EXPECT_NE(message.find("contact_l"), std::string::npos);
    EXPECT_NE(message.find("not registered"), std::string::npos);

    sequence = ParseSequence(MakeSequenceJson(), "walk.anim.json");
    sequence.tracks.push_back("missing_joint");
    EXPECT_NE(ErrorOf([&] { VL::Animation::ValidateSequenceAgainstSkeleton(
        sequence, skeleton, "walk.anim.json"); }).find("missing_joint"), std::string::npos);
}

TEST(AnimationSequenceData, SamplesSingleKeyAsConstant)
{
    const SequenceData sequence = CurveSequence({0.25f}, {7.0f});
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", -100.0f), 7.0f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", 0.25f), 7.0f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", 100.0f), 7.0f);
}

TEST(AnimationSequenceData, ClampsAndInterpolatesLinearly)
{
    const SequenceData sequence = CurveSequence({0.1f, 0.2f}, {0.0f, 10.0f});
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", -5.0f), 0.0f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", 0.15f), 5.0f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", 0.2f), 10.0f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "Phase", 5.0f), 10.0f);
}

TEST(AnimationSequenceData, FindsCurvesCaseInsensitivelyAndReportsMissingNames)
{
    const SequenceData sequence = CurveSequence({0.0f, 1.0f}, {0.0f, 10.0f});
    EXPECT_TRUE(VL::Animation::HasCurve(sequence, "pHASE"));
    EXPECT_NE(VL::Animation::FindCurve(sequence, "pHASE"), nullptr);
    EXPECT_FALSE(SampleFloatCurve(sequence, "missing", 0.5f).has_value());
    EXPECT_THROW(SampleFloatCurveOrThrow(sequence, "missing", 0.5f), std::invalid_argument);
    EXPECT_FLOAT_EQ(SampleFloatCurveOrThrow(sequence, "PHASE", 0.5f), 5.0f);
    EXPECT_THROW(SampleFloatCurve(sequence, "Phase",
        std::numeric_limits<float>::infinity()), std::invalid_argument);
    EXPECT_THROW(SampleFloatCurve(sequence, "Phase",
        std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
}

TEST(AnimationSequenceData, SamplesGaspDistanceFromLedgeAtThirtyHertz)
{
    const SequenceData sequence = ParseSequence(
        GaspHurdleSequenceJson(), "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle.anim.json");
    ASSERT_EQ(sequence.curves.size(), 1u);
    ASSERT_EQ(sequence.curves[0].times.size(), 51u);

    const float backLedge = SampleFloatCurveOrThrow(
        sequence, "distance_from_ledge", 0.598017f);
    const float backFloor = SampleFloatCurveOrThrow(
        sequence, "distance_from_ledge", 0.831335f);
    EXPECT_NEAR(backLedge, 34.098671f, 0.5f);
    EXPECT_NEAR(backFloor, 90.908951f, 0.5f);
    EXPECT_NEAR(std::abs(backLedge - backFloor), 56.810280f, 0.5f);
    EXPECT_FLOAT_EQ(*SampleFloatCurve(sequence, "DISTANCE_FROM_LEDGE", 0.598017f), backLedge);
}

TEST(AnimationSequenceData, LegacyClipWithoutSidecarKeepsEmptySequence)
{
    VL::Animation::Clip clip;
    EXPECT_TRUE(clip.sequence.id.empty());
    EXPECT_TRUE(clip.sequence.curves.empty());
    EXPECT_TRUE(clip.sequence.notifies.empty());
    EXPECT_FLOAT_EQ(clip.sequence.duration, 0.0f);
    EXPECT_FALSE(SampleFloatCurve(clip.sequence, "distance_from_ledge", 0.0f).has_value());

    VL::Animation::SkeletalAsset asset;
    EXPECT_FALSE(asset.HasSkeleton());
    EXPECT_TRUE(asset.skeleton.id.empty());
    EXPECT_TRUE(asset.sourceInputs.empty());
}
}
