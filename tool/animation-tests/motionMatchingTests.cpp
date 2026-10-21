#include "animation/motionMatching.h"
#include "animation/motionRuntime.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <tuple>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

namespace
{
using namespace VL::Animation;

MotionDatabase SmallDatabase()
{
    MotionDatabase database;
    database.schema.trajectoryTimes = {0};
    database.schema.joints = {"root"};
    database.schema.sampleRate = 10;
    database.clipIds = {"Walk", "Idle"};
    database.looping = {true, true};
    database.means.assign(10, 0);
    database.scales.assign(10, 1);
    database.frames = {{0, 0, std::vector<float>(10, 0)},
        {0, 0.5f, std::vector<float>(10, 0)}, {1, 0, std::vector<float>(10, 0)}};
    return database;
}

MotionMatchingPolicy UnrestrictedPolicy()
{
    MotionMatchingPolicy policy;
    policy.continuationBias = 0;
    policy.minimumImprovement = 0;
    policy.reselectionWindow = 0;
    policy.sameClipMinimumJump = 0;
    return policy;
}

MotionQuery ZeroQuery(const std::shared_ptr<const MotionSearchDatabase>& database)
{
    return BindMotionQuery(database, database->Data().schema, std::vector<float>(10, 0));
}

SkeletonMesh SpeedMatchMesh()
{
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "root";
    raw.roots[0].transform = ozz::math::Transform::identity();
    SkeletonMesh mesh;
    mesh.runtime = ozz::animation::offline::SkeletonBuilder()(raw);
    mesh.joints.push_back({"root", -1, Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()});
    return mesh;
}

Clip SpeedMatchClip(float distance = 200, float pitchDegrees = 0, float yawDegrees = 0)
{
    Clip clip;
    clip.id = "linear";
    clip.raw.duration = 2;
    clip.raw.tracks.resize(1);
    auto& root = clip.raw.tracks[0];
    root.translations = {{0, {7, 11, -9}}, {2, {7, 11, -9 + distance}}};
    const float radiansPerDegree = 3.14159265358979323846f / 180;
    const Eigen::Quaternionf rotation(Eigen::AngleAxisf(pitchDegrees * radiansPerDegree, Eigen::Vector3f::UnitX()) *
        Eigen::AngleAxisf(yawDegrees * radiansPerDegree, Eigen::Vector3f::UnitY()));
    root.rotations = {{0, ozz::math::Quaternion::identity()},
        {2, {rotation.x(), rotation.y(), rotation.z(), rotation.w()}}};
    root.scales = {{0, ozz::math::Float3::one()}};
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    return clip;
}

void ExpectSpeedMatchInactive(const MotionSpeedMatchState& state, const char* reason)
{
    EXPECT_FALSE(state.active);
    EXPECT_FLOAT_EQ(state.rate, 1);
    EXPECT_FLOAT_EQ(state.targetRate, 1);
    EXPECT_EQ(state.reason, reason);
}
}

TEST(MotionMatching, LoopMeanUsesRawRootCycleAndRejectsStationaryOrRotatingMembers)
{
    const auto mesh = SpeedMatchMesh();
    const auto clip = SpeedMatchClip();
    const auto mean = ExtractLoopRootVelocity(mesh, clip, true);
    ASSERT_TRUE(mean.available);
    EXPECT_NEAR(mean.velocity.x(), 0, 1e-5f);
    EXPECT_NEAR(mean.velocity.y(), 0, 1e-5f);
    EXPECT_NEAR(mean.velocity.z(), 100, 1e-5f);
    EXPECT_FALSE(ExtractLoopRootVelocity(mesh, clip, false).available);
    EXPECT_FALSE(ExtractLoopRootVelocity(mesh, SpeedMatchClip(0), true).available);
    EXPECT_FALSE(ExtractLoopRootVelocity(mesh, SpeedMatchClip(200, 0, 90), true).available);
    // pitch 不改变水平 heading，仍必须拒绝；仅检查端点 yaw 会漏掉它。
    EXPECT_FALSE(ExtractLoopRootVelocity(mesh, SpeedMatchClip(200, 5, 0), true).available);
}

TEST(MotionMatching, SpeedMatchUsesAchievedDirectionBoundsRateAndImmediatelyResetsOutsideSteadyMotion)
{
    MotionLoopVelocity source{{0, 0, 500}, true};
    MotionTrack track;
    track.achievedVelocity = {450, 100, 0};
    track.walkVelocity = {5000, 0, 0};
    Eigen::Matrix4f component = Eigen::Matrix4f::Identity();
    component.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(3.14159265358979323846f / 2, Eigen::Vector3f::UnitY()).toRotationMatrix();
    auto state = ComputeMotionSpeedMatch(source, track, component, true, false, false, 1, 0.05f);
    ASSERT_TRUE(state.active);
    EXPECT_NEAR(state.sourceSpeed, 500, 1e-4f);
    EXPECT_FLOAT_EQ(state.actualSpeed, 450);
    EXPECT_NEAR(state.targetRate, 0.9f, 1e-6f);
    EXPECT_NEAR(state.rate, 0.9f, 1e-6f);
    EXPECT_NEAR(state.residualSpeed, 0, 1e-4f);
    state = ComputeMotionSpeedMatch(source, track, component, true, false, false, 1, 1.0f / 60);
    EXPECT_NEAR(state.rate, 1 - 2.0f / 60, 1e-6f);
    source.velocity.z() = 700;
    state = ComputeMotionSpeedMatch(source, track, component, true, false, false, 1, 1);
    EXPECT_FLOAT_EQ(state.targetRate, 0.8f);
    EXPECT_FLOAT_EQ(state.rate, 0.8f);
    EXPECT_NEAR(state.residualSpeed, -110, 1e-4f);
    track.achievedVelocity.x() = 1000;
    EXPECT_FLOAT_EQ(ComputeMotionSpeedMatch(source, track, component, true, false, false, 1, 1).rate, 1.2f);
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, false, false, false, 0.9f, 0.01f),
        "disabled");
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, true, false, 0.9f, 0.01f),
        "blending");
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, false, true, 0.9f, 0.01f),
        "action-locked");
    track.grounded = false;
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, false, false, 0.9f, 0.01f),
        "airborne");
    track.grounded = true;
    track.achievedVelocity = {0, 0, 450};
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, false, false, 0.9f, 0.01f),
        "direction-mismatch");
    track.achievedVelocity = {9, 0, 0};
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, false, false, 0.9f, 0.01f),
        "actual-speed-low");
    track.achievedVelocity = {450, 0, 0};
    source.velocity.z() = 9;
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, false, false, 0.9f, 0.01f),
        "source-speed-low");
    source.available = false;
    ExpectSpeedMatchInactive(ComputeMotionSpeedMatch(source, track, component, true, false, false, 0.9f, 0.01f),
        "source-unavailable");
}

TEST(MotionMatching, RetimedContinuationUsesRuntimeSecondsAndExtendsLoopAcrossBothEndpoints)
{
    const auto mesh = SpeedMatchMesh();
    const auto clip = SpeedMatchClip();
    FeatureSchema schema;
    schema.joints = {"root"};
    schema.trajectoryTimes = {-0.25f, 0, 0.5f};
    schema.velocityStep = 0.1f;
    // 原根 100 cm/s，倍率 .8：过去 .25 秒退 20 cm，未来 .5 秒进 40 cm，速度 80。
    for (float time : {0.05f, 1.0f, 1.95f, 2.0f})
    {
        const auto features = ExtractClipFeatures(mesh, clip, schema, time, true, 0.8f);
        ASSERT_EQ(features.size(), 18u);
        EXPECT_NEAR(features[1], -20, 1e-3f);
        EXPECT_NEAR(features[5], 0, 1e-3f);
        EXPECT_NEAR(features[9], 40, 1e-3f);
        EXPECT_NEAR(features[17], 80, 1e-3f);
        const auto nominal = ExtractClipFeatures(mesh, clip, schema, time, true);
        EXPECT_NEAR(nominal[9], 50, 1e-3f);
        EXPECT_NEAR(nominal[17], 100, 1e-3f);
    }
    EXPECT_TRUE(CanExtractClipFeatures(clip, schema, 1.5f, false));
    EXPECT_FALSE(CanExtractClipFeatures(clip, schema, 1.5f, false, 1.2f));
    EXPECT_TRUE(CanExtractClipFeatures(clip, schema, 0.2f, false, 0.8f));
    EXPECT_FALSE(CanExtractClipFeatures(clip, schema, 0.2f, false));
    EXPECT_THROW(ExtractClipFeatures(mesh, clip, schema, 1.5f, false, 1.2f), std::invalid_argument);
}

TEST(MotionMatching, WeightedSquaredDistanceHasIndependentTrajectoryAndPoseCosts)
{
    auto data = SmallDatabase();
    data.schema.trajectoryPositionWeight = 2;
    data.schema.trajectoryFacingWeight = 3;
    data.schema.posePositionWeight = 4;
    data.schema.poseVelocityWeight = 5;
    data.means[0] = 10;
    data.scales[0] = 2;
    data.frames[0].features = {14, 0, 1, 0, 2, 0, 0, 3, 0, 0};
    data.frames.resize(1);
    const auto database = FreezeMotionDatabase(data);
    auto rawQuery = std::vector<float>(10, 0);
    rawQuery[0] = 10;
    const auto query = BindMotionQuery(database, data.schema, rawQuery);
    const auto result = SearchMotionDatabase(query, {"Walk"});
    ASSERT_TRUE(result.selected);
    // 首项归一化差2；2*2² + 3*1² = 11，姿态 4*2² + 5*3² = 61。
    EXPECT_DOUBLE_EQ(result.selected->trajectoryCost, 11);
    EXPECT_DOUBLE_EQ(result.selected->poseCost, 61);
    EXPECT_DOUBLE_EQ(result.selected->totalCost, 72);
}

TEST(MotionMatching, MatchesIndependentExhaustiveOracleAndBoundsTopK)
{
    auto data = SmallDatabase();
    data.frames.clear();
    std::mt19937 generator(4763);
    std::uniform_int_distribution<int> values(-4, 4);
    std::vector<float> rawQuery(10);
    for (float& value : rawQuery) value = static_cast<float>(values(generator));
    for (uint32_t member = 0; member < 2; ++member)
        for (int frame = 0; frame < 20; ++frame)
        {
            std::vector<float> raw(10);
            for (float& value : raw) value = static_cast<float>(values(generator));
            data.frames.push_back({member, frame * 0.1f, raw});
        }
    std::vector<std::tuple<double, std::string, size_t>> expected;
    for (size_t i = 0; i < data.frames.size(); ++i)
    {
        double cost = 0;
        for (size_t column = 0; column < 10; ++column)
        {
            const double difference = rawQuery[column] - data.frames[i].features[column];
            cost += difference * difference;
        }
        expected.emplace_back(cost, data.clipIds[data.frames[i].clip], i);
    }
    std::sort(expected.begin(), expected.end());
    const auto database = FreezeMotionDatabase(data);
    MotionMatchingPolicy policy = UnrestrictedPolicy();
    policy.topK = 3;
    const auto query = BindMotionQuery(database, data.schema, rawQuery);
    const auto result = SearchMotionDatabase(query, {"Idle", "Walk"}, std::nullopt, policy);
    ASSERT_EQ(result.topCandidates.size(), 3u);
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.allowedFrameCount, 40u);
    EXPECT_EQ(result.selected->frameIndex, std::get<2>(expected[0]));
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_DOUBLE_EQ(result.topCandidates[i].distanceCost, std::get<0>(expected[i]));
        EXPECT_EQ(result.topCandidates[i].frameIndex, std::get<2>(expected[i]));
    }
}

TEST(MotionMatching, FilterNeverWidensAnEmptyOrUnknownSelection)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    const auto empty = SearchMotionDatabase(query, {});
    EXPECT_FALSE(empty.selected);
    EXPECT_TRUE(empty.topCandidates.empty());
    EXPECT_EQ(empty.reason, "no-allowed-candidates");
    const auto unknown = SearchMotionDatabase(query, {"Forbidden"});
    EXPECT_FALSE(unknown.selected);
    EXPECT_EQ(unknown.allowedFrameCount, 0u);
}

TEST(MotionMatching, FilterExcludesTheNumericallyBestForbiddenClip)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 2;
    data.frames[1].features[0] = 3;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"});
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Walk");
    EXPECT_DOUBLE_EQ(result.selected->distanceCost, 4);
    EXPECT_EQ(result.allowedFrameCount, 2u);
}

TEST(MotionMatching, TiesUseClipIdThenFrameRegardlessOfAllowedOrder)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    const auto left = SearchMotionDatabase(query, {"Walk", "Idle", "Walk"});
    const auto right = SearchMotionDatabase(query, {"Idle", "Walk"});
    ASSERT_TRUE(left.selected);
    ASSERT_TRUE(right.selected);
    EXPECT_EQ(left.selected->clipId, "Idle");
    EXPECT_EQ(left.selected->frameIndex, right.selected->frameIndex);
    const auto walk = SearchMotionDatabase(query, {"Walk"});
    ASSERT_TRUE(walk.selected);
    EXPECT_EQ(walk.selected->frameIndex, 0u);
}

TEST(MotionMatching, FrozenDatabaseDoesNotRetainWritableSourceAliases)
{
    auto source = SmallDatabase();
    const auto database = FreezeMotionDatabase(source);
    source.frames[0].features[0] = 100;
    source.means[0] = 100;
    source.clipIds[0] = "Changed";
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"});
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Walk");
    EXPECT_DOUBLE_EQ(result.selected->distanceCost, 0);
}

TEST(MotionMatching, RejectsInvalidDatabaseStatisticsAndFrameData)
{
    auto data = SmallDatabase();
    data.scales[0] = 0;
    EXPECT_THROW(FreezeMotionDatabase(data), std::invalid_argument);
    data = SmallDatabase();
    data.means[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(FreezeMotionDatabase(data), std::exception);
    data = SmallDatabase();
    data.frames[0].features.pop_back();
    EXPECT_THROW(FreezeMotionDatabase(data), std::invalid_argument);
    data = SmallDatabase();
    data.frames[0].features[0] = std::numeric_limits<float>::infinity();
    EXPECT_THROW(FreezeMotionDatabase(data), std::exception);
}

TEST(MotionMatching, RejectsWrongSchemaDimensionsAndNonfiniteQueries)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    auto schema = database->Data().schema;
    schema.posePositionWeight = 2;
    EXPECT_THROW(BindMotionQuery(database, schema, std::vector<float>(10, 0)), std::invalid_argument);
    EXPECT_THROW(BindMotionQuery(database, database->Data().schema, {0}), std::invalid_argument);
    auto raw = std::vector<float>(10, 0);
    raw[2] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(BindMotionQuery(database, database->Data().schema, raw), std::invalid_argument);
    auto query = ZeroQuery(database);
    query.normalized[0] = std::numeric_limits<float>::infinity();
    EXPECT_THROW(SearchMotionDatabase(query, {"Walk"}), std::invalid_argument);
}

TEST(MotionMatching, RejectsNormalizationOverflowAndPreservesFiniteLargeDistance)
{
    auto data = SmallDatabase();
    data.scales[0] = 1e-100;
    const auto database = FreezeMotionDatabase(data);
    auto raw = std::vector<float>(10, 0);
    raw[0] = 1;
    EXPECT_THROW(BindMotionQuery(database, data.schema, raw), std::invalid_argument);
    data.scales[0] = 1;
    const auto largeDatabase = FreezeMotionDatabase(data);
    raw[0] = std::numeric_limits<float>::max();
    const auto result = SearchMotionDatabase(BindMotionQuery(largeDatabase, data.schema, raw), {"Walk"});
    ASSERT_TRUE(result.selected);
    EXPECT_TRUE(std::isfinite(result.selected->totalCost));
    EXPECT_GT(result.selected->totalCost, 1e76);
}

TEST(MotionMatching, ExactContinuationUsesSharedStatisticsAndReportsBias)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 10;
    data.frames[1].features[0] = 10;
    const auto database = FreezeMotionDatabase(data);
    MotionContinuation current{"Walk", 0.02f, 1, std::vector<float>(10, 0)};
    MotionMatchingPolicy policy = UnrestrictedPolicy();
    policy.continuationBias = 0.25;
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Idle", "Walk"}, current, policy);
    ASSERT_TRUE(result.selected);
    ASSERT_TRUE(result.continuation);
    EXPECT_TRUE(result.selected->continuing);
    EXPECT_DOUBLE_EQ(result.continuation->distanceCost, 0);
    EXPECT_DOUBLE_EQ(result.continuation->policyCost, -0.25);
    EXPECT_DOUBLE_EQ(result.continuation->totalCost, -0.25);
    EXPECT_DOUBLE_EQ(result.continuation->quantizationSeconds, 0);
}

TEST(MotionMatching, QuantizedContinuationIsLimitedToHalfASample)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    const auto near = SearchMotionDatabase(query, {"Walk"}, MotionContinuation{"Walk", 0.02f, 1});
    ASSERT_TRUE(near.continuation);
    EXPECT_NEAR(near.continuation->quantizationSeconds, 0.02, 1e-7);
    const auto gap = SearchMotionDatabase(query, {"Walk"}, MotionContinuation{"Walk", 0.2f, 1});
    EXPECT_FALSE(gap.continuation);
}

TEST(MotionMatching, ExactContinuationDoesNotRequireAlignmentWithStoredFrameTimes)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 1;
    data.frames[1].features[0] = 2;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0.2f, 1, std::vector<float>(10, 0)}, UnrestrictedPolicy(), 1);
    ASSERT_TRUE(result.continuation);
    ASSERT_TRUE(result.selected);
    EXPECT_TRUE(result.selected->continuing);
    EXPECT_DOUBLE_EQ(result.selected->distanceCost, 0);
    EXPECT_DOUBLE_EQ(result.selected->quantizationSeconds, 0);
}

TEST(MotionMatching, ExactSubframeContinuationDoesNotRemoveADistinctNearbyFrame)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    auto continuedFeatures = std::vector<float>(10, 0);
    continuedFeatures[0] = 10;
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0.02f, 1, continuedFeatures}, UnrestrictedPolicy(), 1);
    ASSERT_TRUE(result.continuation);
    ASSERT_TRUE(result.selected);
    EXPECT_DOUBLE_EQ(result.continuation->distanceCost, 100);
    EXPECT_TRUE(result.switched);
    EXPECT_EQ(result.selected->frameIndex, 0u);
    EXPECT_DOUBLE_EQ(result.selected->distanceCost, 0);
}

TEST(MotionMatching, ContinuationIgnoresStoredRowsBeyondTheActualClipDuration)
{
    auto data = SmallDatabase();
    data.frames[1].time = 5;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0.99f, 1});
    ASSERT_TRUE(result.continuation);
    EXPECT_EQ(result.continuation->frameIndex, 0u);
    EXPECT_NEAR(result.continuation->quantizationSeconds, 0.01, 1e-7);
}

TEST(MotionMatching, ExactContinuationStillRequiresAValidDatabaseMemberWindow)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {-0.2f, 0, 0.2f};
    data.means.assign(18, 0);
    data.scales.assign(18, 1);
    data.looping[0] = false;
    data.frames = {{0, 0.2f, std::vector<float>(18, 0)},
        {0, 0.8f, std::vector<float>(18, 0)}};
    const auto database = FreezeMotionDatabase(data);
    const auto query = BindMotionQuery(database, data.schema, std::vector<float>(18, 0));
    const auto tail = SearchMotionDatabase(query, {"Walk"},
        MotionContinuation{"Walk", 0.99f, 1, std::vector<float>(18, 0)});
    EXPECT_FALSE(tail.continuation);
    const auto head = SearchMotionDatabase(query, {"Walk"}, MotionContinuation{"Walk", 0.1f, 1});
    EXPECT_FALSE(head.continuation);
}

TEST(MotionMatching, NonloopTailRejectsCheaperSameClipRewindAndKeepsOtherMembersCompeting)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {0.4f};
    data.looping[0] = false;
    data.frames[0].time = 0.4f;
    data.frames[1].time = 0.5f;
    data.frames[2].features[0] = 3;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk", "Idle"},
        MotionContinuation{"Walk", 0.8f, 1}, UnrestrictedPolicy(), 1);
    EXPECT_FALSE(result.continuation);
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Idle");
    EXPECT_DOUBLE_EQ(result.selected->totalCost, 9);
    EXPECT_EQ(result.policyRejectedCount, 2u);
    ASSERT_EQ(result.topCandidates.size(), 3u);
    EXPECT_EQ(result.topCandidates[0].clipId, "Walk");
    EXPECT_DOUBLE_EQ(result.topCandidates[0].totalCost, 0);
    EXPECT_FALSE(result.topCandidates[0].eligible);
    EXPECT_EQ(result.topCandidates[0].rejection, "nonloop-tail-rewind");
}

TEST(MotionMatching, NonloopTailWithOnlyTheCurrentMemberReturnsNoEligibleCandidate)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {0.4f};
    data.looping[0] = false;
    data.frames[0].time = 0.4f;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0.8f, 1}, UnrestrictedPolicy(), 1);
    EXPECT_FALSE(result.continuation);
    EXPECT_FALSE(result.selected);
    EXPECT_EQ(result.reason, "no-allowed-candidates");
    EXPECT_EQ(result.allowedFrameCount, 2u);
    EXPECT_EQ(result.policyRejectedCount, 2u);
}

TEST(MotionMatching, LoopingMemberCanStillSelectAnEarlierFrameAcrossTheTailWindow)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {0.4f};
    const auto database = FreezeMotionDatabase(data);
    auto currentRaw = std::vector<float>(10, 0);
    currentRaw[0] = 10;
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0.8f, 1, currentRaw}, UnrestrictedPolicy(), 1);
    ASSERT_TRUE(result.continuation);
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Walk");
    EXPECT_FALSE(result.selected->continuing);
    EXPECT_DOUBLE_EQ(result.selected->time, 0);
    EXPECT_EQ(result.policyRejectedCount, 0u);
}

TEST(MotionMatching, TailPolicyStillRejectsInvalidActualTimeAndDurationBeforeFiltering)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {0.4f};
    data.looping[0] = false;
    const auto database = FreezeMotionDatabase(data);
    const auto query = ZeroQuery(database);
    EXPECT_THROW(SearchMotionDatabase(query, {"Idle"}, MotionContinuation{"Walk", 1.1f, 1}),
        std::invalid_argument);
    EXPECT_THROW(SearchMotionDatabase(query, {}, MotionContinuation{"Walk", 0.8f, 0}),
        std::invalid_argument);
    EXPECT_THROW(SearchMotionDatabase(query, {"Walk"}, MotionContinuation{"Walk",
        std::numeric_limits<float>::quiet_NaN(), 1}), std::invalid_argument);
}

TEST(MotionMatching, UnknownCurrentMemberCannotApplyTailPolicyToOtherCandidates)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {0.4f};
    data.looping[0] = false;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Removed", 0.8f, 1}, UnrestrictedPolicy(), 1);
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Walk");
    EXPECT_EQ(result.policyRejectedCount, 0u);
}

TEST(MotionMatching, MissingQuantizedContinuationInsideTheCompleteWindowDoesNotActivateTailPolicy)
{
    auto data = SmallDatabase();
    data.schema.trajectoryTimes = {0.4f};
    data.looping[0] = false;
    data.frames[0].time = 0.1f;
    data.frames[1].time = 0.2f;
    const auto database = FreezeMotionDatabase(data);
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0.6f, 1}, UnrestrictedPolicy(), 1);
    EXPECT_FALSE(result.continuation);
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Walk");
    EXPECT_EQ(result.policyRejectedCount, 0u);
}

TEST(MotionMatching, ChangingAllowedMembersInvalidatesLockedContinuation)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    MotionMatchingPolicy policy = UnrestrictedPolicy();
    policy.actionLocked = true;
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Idle"},
        MotionContinuation{"Walk", 0, 1}, policy);
    EXPECT_FALSE(result.continuation);
    ASSERT_TRUE(result.selected);
    EXPECT_EQ(result.selected->clipId, "Idle");
    EXPECT_TRUE(result.switched);
}

TEST(MotionMatching, DatabaseSwitchRebindsStatisticsAndRevalidatesCurrentClip)
{
    const auto first = FreezeMotionDatabase(SmallDatabase());
    auto changed = SmallDatabase();
    changed.clipIds[0] = "Run";
    changed.means[0] = 10;
    changed.scales[0] = 2;
    changed.frames[2].features[0] = 10;
    const auto second = FreezeMotionDatabase(changed);
    MotionMatcher matcher;
    EXPECT_TRUE(matcher.Update(ZeroQuery(first), {"Walk"}, std::nullopt, 0.01).selected);
    auto raw = std::vector<float>(10, 0);
    raw[0] = 10;
    const auto result = matcher.Update(BindMotionQuery(second, changed.schema, raw), {"Idle"},
        MotionContinuation{"Walk", 0, 1}, 0.01);
    ASSERT_TRUE(result.selected);
    EXPECT_FALSE(result.continuation);
    EXPECT_EQ(result.selected->clipId, "Idle");
    EXPECT_DOUBLE_EQ(result.selected->distanceCost, 0);
}

TEST(MotionMatching, ActionLockPublishesRejectedAlternativesWithoutSwitching)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 2;
    data.frames[1].features[0] = 3;
    const auto database = FreezeMotionDatabase(data);
    auto policy = UnrestrictedPolicy();
    policy.actionLocked = true;
    const auto result = SearchMotionDatabase(ZeroQuery(database), {"Walk", "Idle"},
        MotionContinuation{"Walk", 0, 1}, policy, 1);
    ASSERT_TRUE(result.selected);
    EXPECT_TRUE(result.selected->continuing);
    EXPECT_EQ(result.reason, "action-locked");
    ASSERT_EQ(result.topCandidates.size(), 3u);
    EXPECT_FALSE(result.topCandidates[0].eligible);
    EXPECT_EQ(result.topCandidates[0].clipId, "Idle");
    EXPECT_EQ(result.topCandidates[0].rejection, "action-locked");
}

TEST(MotionMatching, ReselectionWindowHoldsThenAllowsBetterClip)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 2;
    const auto database = FreezeMotionDatabase(data);
    auto policy = UnrestrictedPolicy();
    policy.reselectionWindow = 0.2;
    const auto query = ZeroQuery(database);
    const auto held = SearchMotionDatabase(query, {"Walk", "Idle"}, MotionContinuation{"Walk", 0, 1},
        policy, 0.1);
    ASSERT_TRUE(held.selected);
    EXPECT_TRUE(held.selected->continuing);
    EXPECT_EQ(held.reason, "reselection-window");
    const auto switched = SearchMotionDatabase(query, {"Walk", "Idle"}, MotionContinuation{"Walk", 0, 1},
        policy, 0.3);
    ASSERT_TRUE(switched.selected);
    EXPECT_EQ(switched.selected->clipId, "Idle");
    EXPECT_TRUE(switched.switched);
}

TEST(MotionMatching, MinimumImprovementRejectsMarginalGain)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 1;
    data.frames[1].features[0] = 2;
    const auto database = FreezeMotionDatabase(data);
    auto policy = UnrestrictedPolicy();
    policy.minimumImprovement = 1.1;
    const auto held = SearchMotionDatabase(ZeroQuery(database), {"Walk", "Idle"},
        MotionContinuation{"Walk", 0, 1}, policy, 1);
    ASSERT_TRUE(held.selected);
    EXPECT_TRUE(held.selected->continuing);
    ASSERT_FALSE(held.topCandidates.empty());
    EXPECT_EQ(held.topCandidates[0].rejection, "minimum-improvement");
    policy.minimumImprovement = 0.9;
    const auto switched = SearchMotionDatabase(ZeroQuery(database), {"Walk", "Idle"},
        MotionContinuation{"Walk", 0, 1}, policy, 1);
    EXPECT_TRUE(switched.switched);
}

TEST(MotionMatching, SameClipNearbyJumpIsRejectedAndDistantJumpIsAllowed)
{
    auto data = SmallDatabase();
    data.frames[0].features[0] = 2;
    const auto database = FreezeMotionDatabase(data);
    auto policy = UnrestrictedPolicy();
    policy.sameClipMinimumJump = 0.6;
    const auto held = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0, 1}, policy, 1);
    ASSERT_TRUE(held.selected);
    EXPECT_TRUE(held.selected->continuing);
    EXPECT_EQ(held.topCandidates[0].rejection, "same-clip-nearby");
    policy.sameClipMinimumJump = 0.4;
    const auto jumped = SearchMotionDatabase(ZeroQuery(database), {"Walk"},
        MotionContinuation{"Walk", 0, 1}, policy, 1);
    ASSERT_TRUE(jumped.selected);
    EXPECT_EQ(jumped.selected->frameIndex, 1u);
    EXPECT_TRUE(jumped.switched);
}

TEST(MotionMatching, ZeroDeltaDoesNotSearchEvenOnFirstUpdate)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    MotionMatcher matcher;
    const auto first = matcher.Update(ZeroQuery(database), {"Idle"}, std::nullopt, 0);
    EXPECT_FALSE(first.searched);
    EXPECT_FALSE(first.selected);
    const auto positive = matcher.Update(ZeroQuery(database), {"Idle"}, std::nullopt, 0.01);
    EXPECT_TRUE(positive.searched);
    ASSERT_TRUE(positive.selected);
    const auto paused = matcher.Update(ZeroQuery(database), {}, std::nullopt, 0);
    EXPECT_FALSE(paused.searched);
    ASSERT_TRUE(paused.selected);
    EXPECT_EQ(paused.selected->clipId, "Idle");
}

TEST(MotionMatching, SearchIntervalDoesNotAccumulatePausedTime)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    MotionMatcher matcher;
    auto policy = UnrestrictedPolicy();
    policy.searchInterval = 0.1;
    EXPECT_TRUE(matcher.Update(query, {"Walk"}, std::nullopt, 0.01, policy).searched);
    const MotionContinuation current{"Walk", 0, 1};
    EXPECT_FALSE(matcher.Update(query, {"Walk"}, current, 0.04, policy).searched);
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(matcher.Update(query, {"Walk"}, current, 0, policy).searched);
    EXPECT_FALSE(matcher.Update(query, {"Walk"}, current, 0.04, policy).searched);
    EXPECT_TRUE(matcher.Update(query, {"Walk"}, current, 0.021, policy).searched);
}

TEST(MotionMatching, EmptySelectionUsesSearchIntervalAndResetStartsFresh)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    MotionMatcher matcher;
    EXPECT_TRUE(matcher.Update(query, {}, std::nullopt, 0.01).searched);
    const auto throttled = matcher.Update(query, {}, std::nullopt, 0.01);
    EXPECT_FALSE(throttled.searched);
    EXPECT_FALSE(throttled.selected);
    EXPECT_EQ(throttled.reason, "no-allowed-candidates");
    matcher.Reset();
    const auto paused = matcher.Update(query, {"Idle"}, std::nullopt, 0);
    EXPECT_FALSE(paused.selected);
    EXPECT_TRUE(matcher.Update(query, {"Idle"}, std::nullopt, 0.01).searched);
}

TEST(MotionMatching, FilterChangeSearchesImmediatelyWithoutWaitingForInterval)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    MotionMatcher matcher;
    EXPECT_TRUE(matcher.Update(query, {"Walk"}, std::nullopt, 0.01).searched);
    const auto changed = matcher.Update(query, {"Idle"}, MotionContinuation{"Walk", 0, 1}, 0.01);
    EXPECT_TRUE(changed.searched);
    ASSERT_TRUE(changed.selected);
    EXPECT_EQ(changed.selected->clipId, "Idle");
    const auto empty = matcher.Update(query, {}, MotionContinuation{"Idle", 0, 1}, 0.01);
    EXPECT_FALSE(empty.selected);
}

TEST(MotionMatching, RejectsInvalidPolicyTimeAndContinuation)
{
    const auto database = FreezeMotionDatabase(SmallDatabase());
    const auto query = ZeroQuery(database);
    auto policy = UnrestrictedPolicy();
    policy.topK = 0;
    EXPECT_THROW(SearchMotionDatabase(query, {"Walk"}, std::nullopt, policy), std::invalid_argument);
    policy = UnrestrictedPolicy();
    policy.minimumImprovement = std::numeric_limits<double>::infinity();
    EXPECT_THROW(SearchMotionDatabase(query, {"Walk"}, std::nullopt, policy), std::invalid_argument);
    EXPECT_THROW(SearchMotionDatabase(query, {"Walk"}, MotionContinuation{"Walk", 2, 1}),
        std::invalid_argument);
    MotionMatcher matcher;
    EXPECT_THROW(matcher.Update(query, {"Walk"}, std::nullopt, -1), std::invalid_argument);
}
