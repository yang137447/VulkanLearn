#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "physics/fracture/fractureBudget.h"

namespace
{
using namespace VL::Physics::Fracture;

FractureBudgetCandidate MakeCandidate(const std::string& owner,
    std::uint64_t shardId, float boundingRadius, float cameraDistance,
    float settledSeconds)
{
    FractureBudgetCandidate candidate;
    candidate.owner = owner;
    candidate.shardId = shardId;
    candidate.kind = FractureBudgetKind::GlassDynamic;
    candidate.boundingRadius = boundingRadius;
    candidate.cameraDistance = cameraDistance;
    candidate.settledSeconds = settledSeconds;
    return candidate;
}

// 用玻璃动态碎片填到指定总量；每块玻璃都不超过单板上限。
void FillWithGlassShards(FractureBudget& budget, std::size_t count)
{
    std::size_t remaining = count;
    int paneIndex = 0;
    while (remaining > 0)
    {
        const std::string owner = "pane" + std::to_string(paneIndex++);
        const std::size_t chunk =
            std::min(remaining, FractureBudget::GlassPaneLimit);
        budget.OnShardsCreated(owner, FractureBudgetKind::GlassDynamic, chunk);
        remaining -= chunk;
    }
}

class RecordingDispatcher final : public FractureEvictionDispatcher
{
public:
    void EvictFractureBudgetShards(
        const std::vector<FractureBudgetCandidate>& candidates) override
    {
        calls += 1;
        evicted.insert(evicted.end(), candidates.begin(), candidates.end());
    }

    int calls = 0;
    std::vector<FractureBudgetCandidate> evicted;
};

} // namespace

TEST(FractureBudget, GlobalLimitIsSharedAcrossObjects)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 288);
    budget.OnShardsCreated("balloon0", FractureBudgetKind::BalloonShard, 32);
    ASSERT_EQ(budget.TotalShards(), FractureBudget::GlobalLimit);

    FractureBudgetRequest request;
    request.owner = "paneNew";
    request.kind = FractureBudgetKind::GlassDynamic;
    request.count = 8;
    const FractureBudgetDecision decision = budget.Request(request);

    EXPECT_FALSE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 0u);
    EXPECT_TRUE(decision.evictions.empty());
}

TEST(FractureBudget, SettledShardsAreReclaimedBySmallestScreenFootprint)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 288);
    budget.OnShardsCreated("balloon0", FractureBudgetKind::BalloonShard, 28);
    ASSERT_EQ(budget.TotalShards(), 316u);

    budget.UpdateReclaimable(MakeCandidate("pane1", 101, 30.0f, 400.0f, 2.0f));
    budget.UpdateReclaimable(MakeCandidate("pane2", 102, 5.0f, 900.0f, 2.0f));
    budget.UpdateReclaimable(MakeCandidate("balloon0", 103, 20.0f, 200.0f, 2.0f));
    // 未达到 1.5 秒静止阈值的碎片永远不能回收。
    budget.UpdateReclaimable(MakeCandidate("pane1", 104, 1.0f, 5000.0f, 0.4f));

    FractureBudgetRequest request;
    request.owner = "paneNew";
    request.count = 6;
    const FractureBudgetDecision decision = budget.Request(request);

    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 6u);
    ASSERT_EQ(decision.evictions.size(), 2u);
    // 5/900 的屏幕占比最小，先回收；其次是 30/400。
    EXPECT_EQ(decision.evictions[0].shardId, 102u);
    EXPECT_EQ(decision.evictions[1].shardId, 101u);

    for (const FractureBudgetCandidate& candidate : decision.evictions)
    {
        budget.OnShardsRemoved(candidate.owner, candidate.kind);
        budget.UntrackReclaimable(candidate.owner, candidate.shardId);
    }
    budget.OnShardsCreated("paneNew", FractureBudgetKind::GlassDynamic, 6);
    EXPECT_EQ(budget.TotalShards(), FractureBudget::GlobalLimit);
}

TEST(FractureBudget, UnsettledShardsAreNeverReclaimed)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 320);
    budget.UpdateReclaimable(MakeCandidate("pane0", 7, 1.0f, 5000.0f, 1.49f));

    FractureBudgetRequest request;
    request.owner = "paneNew";
    request.count = 4;
    const FractureBudgetDecision decision = budget.Request(request);

    EXPECT_FALSE(decision.granted);
    EXPECT_TRUE(decision.evictions.empty());
}

TEST(FractureBudget, AttachedRequestDegradesToWholePlateDrop)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 320);

    FractureBudgetRequest request;
    request.owner = "pane0";
    request.kind = FractureBudgetKind::GlassAttached;
    request.count = 2;
    request.allowWholePlateDrop = true;
    const FractureBudgetDecision decision = budget.Request(request);

    EXPECT_FALSE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 0u);
    EXPECT_TRUE(decision.dropWholePlate);
    EXPECT_TRUE(decision.evictions.empty());
}

TEST(FractureBudget, PaneLimitStopsGrowthEvenWithGlobalRoom)
{
    FractureBudget budget;
    budget.OnShardsCreated(
        "pane0", FractureBudgetKind::GlassDynamic,
        FractureBudget::GlassPaneLimit);

    FractureBudgetRequest request;
    request.owner = "pane0";
    request.count = 2;
    FractureBudgetDecision decision = budget.Request(request);
    EXPECT_FALSE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 0u);

    // 同一块玻璃上的已静止碎片可以腾出自己的额度。
    budget.UpdateReclaimable(MakeCandidate("pane0", 11, 10.0f, 300.0f, 2.0f));
    decision = budget.Request(request);
    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 1u);
    ASSERT_EQ(decision.evictions.size(), 1u);
    EXPECT_EQ(decision.evictions.front().shardId, 11u);
}

TEST(FractureBudget, BalloonRequestIsTrimmedToRemainingGlobalBudget)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 288);
    budget.OnShardsCreated(
        "balloonOld", FractureBudgetKind::BalloonShard, 12);
    ASSERT_EQ(budget.TotalShards(), 300u);

    FractureBudgetRequest request;
    request.owner = "balloonNew";
    request.kind = FractureBudgetKind::BalloonShard;
    request.count = FractureBudget::BalloonLimit;
    const FractureBudgetDecision decision = budget.Request(request);

    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 20u);
    EXPECT_FALSE(decision.exceededLimit);
}

TEST(FractureBudget, BalloonKeepsVisualFloorWhenBudgetIsExhausted)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 320);

    FractureBudgetRequest request;
    request.owner = "balloonNew";
    request.kind = FractureBudgetKind::BalloonShard;
    request.count = FractureBudget::BalloonLimit;
    const FractureBudgetDecision decision = budget.Request(request);

    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, FractureBudget::MinimumBalloonShards);
    EXPECT_TRUE(decision.exceededLimit);
    // Request 是纯查询，视觉下限不会在查询阶段修改账本。
    EXPECT_EQ(budget.TotalShards(), FractureBudget::GlobalLimit);
}

TEST(FractureBudget, AttachedPlatesAreNotReclaimableCandidates)
{
    FractureBudget budget;
    budget.OnShardsCreated(
        "pane0", FractureBudgetKind::GlassAttached, 10);

    FractureBudgetCandidate attached;
    attached.owner = "pane0";
    attached.shardId = 1;
    attached.kind = FractureBudgetKind::GlassAttached;
    attached.settledSeconds = 10.0f;
    budget.UpdateReclaimable(attached);

    EXPECT_EQ(budget.ReclaimableShards(), 0u);
}

TEST(FractureBudget, ResetClearsUsageAndCandidates)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 96);
    budget.OnShardsCreated(
        "balloon0", FractureBudgetKind::BalloonShard, 8);
    budget.UpdateReclaimable(MakeCandidate("pane0", 3, 10.0f, 200.0f, 5.0f));
    ASSERT_EQ(budget.TotalShards(), 104u);
    ASSERT_EQ(budget.ReclaimableShards(), 1u);

    budget.Reset();

    EXPECT_EQ(budget.TotalShards(), 0u);
    EXPECT_EQ(budget.OwnerShards("pane0"), 0u);
    EXPECT_EQ(budget.ReclaimableShards(), 0u);

    FractureBudgetRequest request;
    request.owner = "pane0";
    request.count = 10;
    const FractureBudgetDecision decision = budget.Request(request);
    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 10u);
    EXPECT_TRUE(decision.evictions.empty());
}

TEST(FractureBudget, AcquireDispatchesEvictionPlanOnce)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 316);
    // 五个候选提供足够的回收额度：8 片请求里 4 片来自全局空位、4 片来自回收。
    budget.UpdateReclaimable(MakeCandidate("pane0", 21, 1.0f, 1000.0f, 3.0f));
    budget.UpdateReclaimable(MakeCandidate("pane0", 22, 2.0f, 1000.0f, 3.0f));
    budget.UpdateReclaimable(MakeCandidate("pane0", 23, 3.0f, 1000.0f, 3.0f));
    budget.UpdateReclaimable(MakeCandidate("pane0", 24, 4.0f, 1000.0f, 3.0f));
    budget.UpdateReclaimable(MakeCandidate("pane0", 25, 5.0f, 1000.0f, 3.0f));

    RecordingDispatcher dispatcher;
    FractureBudgetRequest request;
    request.owner = "paneNew";
    request.count = 8;
    const FractureBudgetDecision decision =
        AcquireFractureBudget(budget, dispatcher, request);

    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 8u);
    EXPECT_EQ(dispatcher.calls, 1);
    ASSERT_EQ(dispatcher.evicted.size(), 4u);
    EXPECT_EQ(dispatcher.evicted.front().shardId, 21u);
    EXPECT_EQ(dispatcher.evicted.back().shardId, 24u);
}

TEST(FractureBudget, ZeroCountRequestIsAlwaysGranted)
{
    FractureBudget budget;
    FillWithGlassShards(budget, 320);

    FractureBudgetRequest request;
    request.owner = "paneNew";
    request.count = 0;
    const FractureBudgetDecision decision = budget.Request(request);

    EXPECT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 0u);
    EXPECT_TRUE(decision.evictions.empty());
}
