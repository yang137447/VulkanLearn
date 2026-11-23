#include "terrainCompositedRuntimeCandidateTests.cpp"
#include "render/terrain/terrainCompositedRequestDiagnostics.h"

namespace VL
{
namespace
{
TEST_F(TerrainCompositedRuntimeCandidateTest, R10NotSelectedDespiteFreePoolIsNotPoolExhaustion)
{
    PublishCurrent();
    const auto desired = runtime->Token({"island", 0, 0, 0});
    runtime->ConsumeFeedback({desired});
    auto row = TerrainCompositedRequestProbe::Inspect(*runtime, desired, 2);
    EXPECT_EQ(std::string(row.currentGate), "queued-eligible");
    ASSERT_TRUE(row.page);
    EXPECT_TRUE(row.page->pageQueued);
    runtime->ConsumeFeedback({});
    row = TerrainCompositedRequestProbe::Inspect(*runtime, desired, 2);
    EXPECT_EQ(std::string(row.currentGate), "not-selected");
    EXPECT_FALSE(row.page->pageQueued);
    EXPECT_GT(row.page->freeSlots, 500u);
    EXPECT_EQ(row.queuedCancel, 1u);
    const auto before = runtime->Statistics().surfaces.at("island").queuedCancel;
    const auto again = TerrainCompositedRequestProbe::Inspect(*runtime, desired, 2);
    EXPECT_EQ(again.queuedCancel, before);
    EXPECT_EQ(runtime->Statistics().surfaces.at("island").queuedCancel, before);
    TerrainCompositedRequestProbe::Write(std::cout, row, 11, 2);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R10ActualFailureExhaustsAttemptsWithFreeSlots)
{
    auto token = runtime->Token(Root());
    for (uint32_t attempt = 0; attempt < budget.surface.maxReadAttempts; ++attempt)
    {
        auto row = TerrainCompositedRequestProbe::Inspect(*runtime, token, 100);
        while (row.page->backoffTicks)
        {
            Record(100);
            runtime->Abort();
            row = TerrainCompositedRequestProbe::Inspect(*runtime, token, 100);
        }
        const auto page = runtime->ReserveProduction(token, 100);
        ASSERT_TRUE(page);
        EXPECT_FALSE(runtime->CompleteProduction(*page, 100, false));
    }
    const auto exhausted = TerrainCompositedRequestProbe::Inspect(*runtime, token, 100);
    EXPECT_EQ(std::string(exhausted.currentGate), "attempts-exhausted");
    EXPECT_EQ(exhausted.page->attempts, budget.surface.maxReadAttempts);
    EXPECT_FALSE(exhausted.page->pageQueued);
    EXPECT_GT(exhausted.page->freeSlots, 500u);
    TerrainCompositedRequestProbe::Write(std::cout, exhausted, 20, 100);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R10CollectorUnarmedHasNoOutputBoundedCapturesAndChargeReturns)
{
    TerrainCompositedExplicitRequestCollector collector;
    std::ostringstream output;
    const auto baseline = owner->Statistics().current.cpuBytes;
    EXPECT_FALSE(collector.Capture(*runtime, 0, 0, output));
    EXPECT_TRUE(output.str().empty());
    EXPECT_FALSE(collector.Arm(std::vector<TerrainCompositedVTTaskToken>(9), *owner));
    auto token = runtime->Token(Root());
    const nlohmann::json plan{{"schema", "rvt-explicit-page-probe-v1"},
        {"pages", {{{"surface", "island"}, {"mip", token.key.mip}, {"x", 0}, {"y", 0},
                     {"expectedWorldGeneration", token.worldGeneration}}}}};
    ASSERT_TRUE(collector.ArmPlan(*runtime, plan, *owner));
    EXPECT_GT(owner->Statistics().current.cpuBytes, baseline);
    EXPECT_FALSE(collector.Arm({token}, *owner));
    for (uint32_t capture = 0; capture < collector.MaxCaptures; ++capture)
        EXPECT_TRUE(collector.Capture(*runtime, capture, 0, output));
    EXPECT_FALSE(collector.Capture(*runtime, 13, 0, output));
    const auto text = output.str();
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), collector.MaxCaptures);
    collector.Cancel();
    EXPECT_EQ(owner->Statistics().current.cpuBytes, baseline);
    EXPECT_FALSE(collector.Capture(*runtime, 14, 0, output));
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R10QueueCapIsObservableWithoutPoolPressure)
{
    runtime.reset();
    budget.surface.queuedRequests = 1;
    runtime = std::make_unique<TerrainCompositedRuntime>(backend, budget);
    runtime->RegisterPreviewSurface(desc, 42, 1);
    const auto desired = runtime->Token({"island", 0, 0, 0});
    runtime->ConsumeFeedback({desired});
    const auto row = TerrainCompositedRequestProbe::Inspect(*runtime, desired, 0);
    EXPECT_EQ(std::string(row.currentGate), "queue-cap-at-observation");
    EXPECT_TRUE(row.needed);
    EXPECT_FALSE(row.page->pageQueued);
    EXPECT_GT(row.throttledRequests, 0u);
    EXPECT_EQ(row.page->freeSlots, 512u);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R10BudgetSelectionCoarsensBeforeAllocationDespiteFreePool)
{
    runtime.reset();
    budget.surface.slots = 8;
    runtime = std::make_unique<TerrainCompositedRuntime>(backend, budget);
    runtime->RegisterPreviewSurface(desc, 42, 1);
    PublishCurrent();
    std::vector<TerrainCompositedVTTaskToken> selected;
    for (uint32_t index = 0; index < 4; ++index)
        selected.push_back(runtime->Token({"island", 0, index % 2, index / 2}));
    runtime->ConsumeFeedback(selected);
    const auto row = TerrainCompositedRequestProbe::Inspect(*runtime, selected[3], 2);
    EXPECT_EQ(row.page->freeSlots, 7u);
    EXPECT_EQ(row.freeReserve, 4u);
    EXPECT_EQ(row.effectiveMipBias, 1u);
    EXPECT_EQ(std::string(row.currentGate), "not-selected");
    EXPECT_FALSE(runtime->ReserveProduction(selected[3], 2));
    EXPECT_EQ(runtime->ProductionDiagnostics(selected[3], 2)->attempts, 0u);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R10OwnerCountAndEpochAreActualAndProbeNeverCollects)
{
    PublishCurrent();
    auto token = runtime->Token(Root());
    auto original = runtime->Binding("island");
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    ASSERT_TRUE(runtime->CommitCandidate());
    auto row = TerrainCompositedRequestProbe::Inspect(*runtime, token, 1);
    ASSERT_TRUE(row.page);
    EXPECT_GT(row.blockedRetiringEpoch, 0u);
    EXPECT_GT(row.blockedRetiringOwner, 0u);
    EXPECT_GE(row.matchingRetirementOwnerReferences, 2u);
    EXPECT_EQ(std::string(row.currentGate), "surface-retired");
    const auto reservation = row.page->reservation;
    const auto after = TerrainCompositedRequestProbe::Inspect(*runtime, token, 100);
    EXPECT_EQ(after.page->reservation, reservation);
    EXPECT_GT(after.blockedRetiringOwner, 0u);
    EXPECT_EQ(after.blockedRetiringEpoch, 0u);
}
}
}
