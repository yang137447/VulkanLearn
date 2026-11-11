#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "physics/fracture/fractureBudget.h"

namespace
{
using namespace VL::Physics::Fracture;

FractureBudgetCandidate Candidate(
    const std::string& owner, const std::string& budgetClass,
    std::uint64_t id, float settled, float radius, float distance)
{
    FractureBudgetCandidate candidate;
    candidate.owner = owner;
    candidate.budgetClass = budgetClass;
    candidate.shardId = id;
    candidate.settledSeconds = settled;
    candidate.boundingRadius = radius;
    candidate.cameraDistance = distance;
    return candidate;
}

FractureBudgetRequest Request(
    const std::string& owner, const std::string& budgetClass,
    std::size_t count)
{
    FractureBudgetRequest request;
    request.owner = owner;
    request.budgetClass = budgetClass;
    request.count = count;
    return request;
}

class CountingDispatcher final : public FractureEvictionDispatcher
{
public:
    void EvictFractureBudgetShards(
        const std::vector<FractureBudgetCandidate>& candidates) override
    {
        for (const FractureBudgetCandidate& candidate : candidates)
            evicted.push_back(candidate);
    }

    std::vector<FractureBudgetCandidate> evicted;
};
} // namespace

TEST(FractureBudget, UnknownClassIsRejectedInsteadOfSilentlyUsingGlass)
{
    FractureBudget budget;
    EXPECT_THROW(budget.OnShardsCreated("pane", "unknown", 1),
        std::invalid_argument);
    EXPECT_THROW(budget.Request(Request("pane", "unknown", 1)),
        std::invalid_argument);
}

TEST(FractureBudget, GlassDynamicAndAttachedSharePaneOwnerLimit)
{
    FractureBudget budget;
    budget.OnShardsCreated("pane", "glass.dynamic", 48);
    budget.OnShardsCreated("pane", "glass.attached", 47);
    EXPECT_TRUE(budget.Request(Request("pane", "glass.dynamic", 1)).granted);
    budget.OnShardsCreated("pane", "glass.attached", 1);
    EXPECT_FALSE(budget.Request(Request("pane", "glass.dynamic", 1)).granted);
}

TEST(FractureBudget, GlobalLimitIsSharedAcrossOwners)
{
    FractureBudget budget;
    budget.OnShardsCreated("paneA", "glass.dynamic", 96);
    budget.OnShardsCreated("paneB", "glass.dynamic", 96);
    budget.OnShardsCreated("paneC", "glass.dynamic", 96);
    budget.OnShardsCreated("paneD", "glass.dynamic", 32);
    ASSERT_EQ(budget.TotalShards(), FractureBudget::GlobalLimit);
    EXPECT_FALSE(budget.Request(Request("paneE", "glass.dynamic", 1)).granted);
}

TEST(FractureBudget, BalloonKeepsVisualFloorWhenGlobalBudgetIsExhausted)
{
    FractureBudget budget;
    budget.OnShardsCreated("paneA", "glass.dynamic", 96);
    budget.OnShardsCreated("paneB", "glass.dynamic", 96);
    budget.OnShardsCreated("paneC", "glass.dynamic", 96);
    budget.OnShardsCreated("paneD", "glass.dynamic", 32);
    const FractureBudgetDecision decision =
        budget.Request(Request("balloon", "balloon.shard", 8));
    EXPECT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, FractureBudget::MinimumBalloonShards);
    EXPECT_TRUE(decision.exceededLimit);
}

TEST(FractureBudget, AttachedRequestDegradesToWholePlateDrop)
{
    FractureBudget budget;
    budget.OnShardsCreated("pane", "glass.dynamic", 96);
    FractureBudgetRequest request = Request("pane", "glass.attached", 2);
    request.allowWholePlateDrop = true;
    const FractureBudgetDecision decision = budget.Request(request);
    EXPECT_FALSE(decision.granted);
    EXPECT_TRUE(decision.dropWholePlate);
}

TEST(FractureBudget, SettledShardsAreReclaimedBySmallestScreenFootprint)
{
    FractureBudget budget;
    budget.OnShardsCreated("pane", "glass.dynamic", 96);
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.dynamic", 1, 2.0f, 20.0f, 100.0f));
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.dynamic", 2, 2.0f, 5.0f, 100.0f));
    const FractureBudgetDecision decision =
        budget.Request(Request("pane", "glass.dynamic", 1));
    ASSERT_TRUE(decision.granted);
    ASSERT_EQ(decision.evictions.size(), 1u);
    EXPECT_EQ(decision.evictions.front().shardId, 2u);
}

TEST(FractureBudget, UnsettledShardsAreNeverReclaimed)
{
    FractureBudget budget;
    budget.OnShardsCreated("pane", "glass.dynamic", 96);
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.dynamic", 1, 0.5f, 5.0f, 100.0f));
    const FractureBudgetDecision decision =
        budget.Request(Request("pane", "glass.dynamic", 1));
    EXPECT_FALSE(decision.granted);
    EXPECT_TRUE(decision.evictions.empty());
}

TEST(FractureBudget, AttachedPlatesAreNotReclaimable)
{
    FractureBudget budget;
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.attached", 1, 10.0f, 5.0f, 100.0f));
    EXPECT_EQ(budget.ReclaimableShards(), 0u);
}

TEST(FractureBudget, ExcludeShardIdsPreventsParentEviction)
{
    FractureBudget budget;
    budget.OnShardsCreated("pane", "glass.dynamic", 96);
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.dynamic", 1, 2.0f, 1.0f, 100.0f));
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.dynamic", 2, 2.0f, 20.0f, 100.0f));
    FractureBudgetRequest request = Request("pane", "glass.dynamic", 1);
    request.excludeShardIds = {1};
    const FractureBudgetDecision decision = budget.Request(request);
    ASSERT_TRUE(decision.granted);
    ASSERT_EQ(decision.evictions.size(), 1u);
    EXPECT_EQ(decision.evictions.front().shardId, 2u);
}

TEST(FractureBudget, AcquireDispatchesEvictionPlanOnce)
{
    FractureBudget budget;
    CountingDispatcher dispatcher;
    budget.OnShardsCreated("pane", "glass.dynamic", 96);
    budget.UpdateReclaimable(Candidate(
        "pane", "glass.dynamic", 1, 2.0f, 5.0f, 100.0f));
    const FractureBudgetDecision decision = AcquireFractureBudget(
        budget, dispatcher, Request("pane", "glass.dynamic", 1));
    ASSERT_TRUE(decision.granted);
    EXPECT_EQ(dispatcher.evicted.size(), 1u);
    EXPECT_EQ(dispatcher.evicted.front().shardId, 1u);
}

TEST(FractureBudget, CustomClassCanBeRegisteredWithoutChangingSwitches)
{
    FractureBudget budget;
    budget.RegisterClass({"ceramic.shard", "ceramic", 10, 0, true});
    budget.OnShardsCreated("vase", "ceramic.shard", 10);
    EXPECT_FALSE(budget.Request(
        Request("vase", "ceramic.shard", 1)).granted);
}

TEST(FractureBudget, ResetClearsUsageButKeepsRegisteredClasses)
{
    FractureBudget budget;
    budget.OnShardsCreated("pane", "glass.dynamic", 4);
    budget.Reset();
    EXPECT_EQ(budget.TotalShards(), 0u);
    EXPECT_NO_THROW(budget.OnShardsCreated("pane", "glass.dynamic", 1));
}

TEST(FractureBudget, ZeroCountRequestIsAlwaysGranted)
{
    FractureBudget budget;
    const FractureBudgetDecision decision =
        budget.Request(Request("pane", "glass.dynamic", 0));
    EXPECT_TRUE(decision.granted);
    EXPECT_EQ(decision.grantedCount, 0u);
}
