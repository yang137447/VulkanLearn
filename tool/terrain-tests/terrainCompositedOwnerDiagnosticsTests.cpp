#include "terrainCompositedWorldCandidateProgressTests.cpp"

namespace VL
{
namespace
{
TEST_F(TerrainCompositedRuntimeCandidateTest, R9CandidateDtoCountsActualPagesWithoutAdvancingReadiness)
{
    auto first = runtime->CandidateDiagnostics("island", 0);
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->current.present);
    EXPECT_FALSE(first->candidate.present);
    EXPECT_EQ(first->current.pages.requiredPages, 1u);
    EXPECT_EQ(first->current.pages.currentPages, 0u);
    EXPECT_FALSE(first->current.pages.rootCurrent);
    PublishCurrent();
    BeginCandidate();
    auto waiting = runtime->CandidateDiagnostics("island", 2);
    ASSERT_TRUE(waiting);
    EXPECT_EQ(waiting->current.tablePublishEpoch, 2u);
    EXPECT_EQ(waiting->current.pages.currentPages, 1u);
    EXPECT_EQ(waiting->candidate.pages.requiredPages, 1u);
    EXPECT_EQ(waiting->candidate.pages.currentPages, 0u);
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    const auto tooEarly = runtime->CandidateDiagnostics("island", 3);
    ASSERT_TRUE(tooEarly);
    EXPECT_EQ(tooEarly->candidate.tablePublishEpoch, 4u);
    EXPECT_EQ(tooEarly->candidate.pages.currentPages, 0u);
    std::optional<TerrainCompositedCandidateDiagnostics> completed;
    TerrainRuntimeAllocationCountdown = 0;
    try { completed = runtime->CandidateDiagnostics("island", 4); }
    catch (...) { TerrainRuntimeAllocationCountdown = -1; throw; }
    TerrainRuntimeAllocationCountdown = -1;
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->candidate.pages.currentPages, 1u);
    EXPECT_TRUE(completed->candidate.pages.rootCurrent);
    EXPECT_EQ(completed->candidate.pages.revision, 2u);
    EXPECT_EQ(runtime->CandidateBinding("island").u_compositedEnabled, 0.f);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    ASSERT_TRUE(runtime->CommitCandidate());
    const auto published = runtime->CandidateDiagnostics("island", 4);
    ASSERT_TRUE(published);
    EXPECT_FALSE(published->candidate.present);
    EXPECT_EQ(published->current.pages.revision, 2u);
    EXPECT_EQ(published->current.tablePublishEpoch, 4u);
    EXPECT_FALSE(runtime->CandidateDiagnostics("missing", 4));
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R9PageDtoKeepsActualTokensAndRetryClockReadOnly)
{
    const auto token = runtime->Token(Root());
    const auto page = runtime->ReserveProduction(token, 0);
    ASSERT_TRUE(page);
    ASSERT_FALSE(runtime->CompleteProduction(*page, 5, false));
    const auto first = runtime->ProductionDiagnostics(token, 4);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->reservation, page->reservation);
    EXPECT_TRUE(first->reservedToken.Matches(page->token));
    EXPECT_TRUE(first->slotTokenMatches);
    EXPECT_EQ(first->completedEpoch, 4u);
    EXPECT_TRUE(first->pageQueued);
    EXPECT_EQ(first->attempts, 1u);
    EXPECT_EQ(first->retryAfterTick - first->schedulingTick, first->backoffTicks);
    EXPECT_EQ(first->backoffTicks, 2u);
    const auto second = runtime->ProductionDiagnostics(token, 5);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->attempts, first->attempts);
    EXPECT_EQ(second->retryAfterTick, first->retryAfterTick);
    EXPECT_EQ(second->schedulingTick, first->schedulingTick);
    EXPECT_TRUE(second->retirementReusable);
    EXPECT_FALSE(runtime->SlotReuseDiagnostics(5));
}

TEST(TerrainCompositedOwnerDiagnostics, R9ActualSlotBoundaryBlocksEpochAndOwnerThenObservesDifferentReservation)
{
    TerrainCompositedVTBuildDesc desc;
    desc.surfaceId = "old";
    desc.width = desc.height = 256;
    desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
    TerrainCompositedResidencyBudget caps;
    caps.slots = 2;
    auto pool = std::make_shared<TerrainCompositedResidencyPool>(2);
    TerrainCompositedResidency previous({}, desc, 7, 1, caps, pool);
    auto token = previous.ProductionRequests().front();
    const auto upload = previous.ReserveProduction(token, 0);
    ASSERT_TRUE(upload);
    ASSERT_TRUE(previous.CompleteProduction(*upload, 5, true));
    const auto batch = previous.BeginUploads(5);
    previous.Commit(batch.id, 6);
    auto sentinelDesc = desc;
    sentinelDesc.surfaceId = "sentinel";
    TerrainCompositedResidency sentinel({}, sentinelDesc, 7, 1, caps, pool);
    const auto sentinelToken = sentinel.ProductionRequests().front();
    const auto sentinelUpload = sentinel.ReserveProduction(sentinelToken, 6);
    ASSERT_TRUE(sentinelUpload);
    ASSERT_TRUE(sentinel.CompleteProduction(*sentinelUpload, 6, true));
    const auto sentinelBatch = sentinel.BeginUploads(6);
    sentinel.Commit(sentinelBatch.id, 7);
    TerrainRvtBudgetCaps rejectedCaps;
    rejectedCaps.totalCpuBytes = rejectedCaps.workspaceCpuBytes = 1;
    TerrainRvtBudgetOwner rejectedOwner(rejectedCaps);
    EXPECT_FALSE(pool->ArmSlotReuseDiagnostics(upload->slot, rejectedOwner));
    EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
    const auto unchanged = pool->SlotDiagnostics(upload->slot, 7);
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->reservation, upload->reservation);
    EXPECT_EQ(rejectedOwner.Statistics().current.cpuBytes, 0u);
    TerrainRvtBudgetCaps budgetCaps;
    budgetCaps.totalCpuBytes = budgetCaps.workspaceCpuBytes = 1024 * 1024;
    TerrainRvtBudgetOwner owner(budgetCaps);
    const auto before = owner.Statistics().current.cpuBytes;
    EXPECT_FALSE(pool->SlotReuseDiagnostics(0));
    ASSERT_TRUE(pool->ArmSlotReuseDiagnostics(upload->slot, owner));
    EXPECT_GT(owner.Statistics().current.cpuBytes, before);
    EXPECT_FALSE(pool->ArmSlotReuseDiagnostics(upload->slot, owner));
    auto descriptorLeaf = std::make_shared<int>(1);
    previous.RetireAll(9, descriptorLeaf);
    previous.MarkReferenced(12);
    desc.surfaceId = "replacement";
    TerrainCompositedResidency replacement({}, desc, 7, 2, caps, pool);
    const auto next = replacement.ProductionRequests().front();
    EXPECT_FALSE(replacement.ReserveProduction(next, 10));
    auto held = pool->SlotReuseDiagnostics(10);
    ASSERT_TRUE(held);
    EXPECT_GT(held->blockedByEpoch, 0u);
    EXPECT_GT(held->blockedByOwner, 0u);
    EXPECT_FALSE(held->releasedAtBoundary);
    EXPECT_FALSE(replacement.ReserveProduction(next, 12));
    descriptorLeaf.reset();
    const auto newUpload = replacement.ReserveProduction(next, 12);
    ASSERT_TRUE(newUpload);
    EXPECT_EQ(newUpload->slot, upload->slot);
    EXPECT_NE(newUpload->reservation, upload->reservation);
    const auto reused = pool->SlotReuseDiagnostics(12);
    ASSERT_TRUE(reused);
    EXPECT_TRUE(reused->retirementObserved);
    EXPECT_TRUE(reused->releasedAtBoundary);
    EXPECT_EQ(reused->releaseReferenceEpoch, 12u);
    EXPECT_EQ(reused->releaseRetireEpoch, 12u);
    EXPECT_EQ(reused->releaseCompletedEpoch, 12u);
    EXPECT_EQ(reused->replacementCompletedEpoch, 12u);
    EXPECT_TRUE(reused->replacementObserved);
    EXPECT_TRUE(reused->replacementStillPresent);
    EXPECT_TRUE(reused->original.publishedToken.Matches(token));
    EXPECT_TRUE(reused->current.reservedToken.Matches(next));
    EXPECT_EQ(reused->original.reservation, upload->reservation);
    EXPECT_EQ(reused->current.reservation, newUpload->reservation);
    pool->CancelSlotReuseDiagnostics();
    EXPECT_FALSE(pool->SlotReuseDiagnostics(12));
    EXPECT_EQ(owner.Statistics().current.cpuBytes, before);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R9RuntimeCollectRetiredReleasesOwnTableBeforeActualSlotReuse)
{
    PublishCurrent();
    const auto oldToken = runtime->Token(Root());
    const auto oldSlot = runtime->ProductionDiagnostics(oldToken, 2);
    ASSERT_TRUE(oldSlot);
    ASSERT_TRUE(runtime->ArmSlotReuseDiagnostics(oldSlot->slot));
    auto oldBinding = runtime->Binding("island");
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    ASSERT_TRUE(runtime->CommitCandidate());
    Record(100);
    runtime->Commit(101);
    const auto descriptorHeld = runtime->SlotReuseDiagnostics(100);
    ASSERT_TRUE(descriptorHeld);
    EXPECT_TRUE(descriptorHeld->current.retirementOwnerAlive);
    EXPECT_GT(descriptorHeld->blockedByOwner, 0u);
    EXPECT_FALSE(descriptorHeld->releasedAtBoundary);
    oldBinding = {};
    Record(101);
    runtime->Commit(102);
    const auto released = runtime->SlotReuseDiagnostics(101);
    ASSERT_TRUE(released);
    EXPECT_TRUE(released->releasedAtBoundary);
    EXPECT_FALSE(released->replacementObserved);
    EXPECT_FALSE(runtime->ProductionDiagnostics(oldToken, 101));
    const auto next = runtime->Token({"island", 0, 0, 0});
    runtime->ConsumeFeedback({next});
    const auto newPage = runtime->ReserveProduction(next, 102);
    ASSERT_TRUE(newPage);
    EXPECT_EQ(newPage->slot, oldSlot->slot);
    const auto reused = runtime->SlotReuseDiagnostics(102);
    ASSERT_TRUE(reused);
    EXPECT_TRUE(reused->replacementObserved);
    EXPECT_TRUE(reused->replacementStillPresent);
    EXPECT_TRUE(reused->current.reservedToken.Matches(next));
    EXPECT_TRUE(reused->original.publishedToken.Matches(oldToken));
    runtime->CancelSlotReuseDiagnostics();
}

TEST_F(TerrainCompositedRuntimeCandidateTest, R9UnarmedAndFailedWatchAdmissionLeaveSlotAndSchedulingUntouched)
{
    const auto token = runtime->Token(Root());
    const auto page = runtime->ReserveProduction(token, 0);
    ASSERT_TRUE(page);
    std::optional<TerrainCompositedSlotReuseDiagnostics> unarmed;
    TerrainRuntimeAllocationCountdown = 0;
    try { unarmed = runtime->SlotReuseDiagnostics(0); }
    catch (...) { TerrainRuntimeAllocationCountdown = -1; throw; }
    TerrainRuntimeAllocationCountdown = -1;
    EXPECT_FALSE(unarmed);
    EXPECT_FALSE(runtime->ArmSlotReuseDiagnostics(UINT32_MAX));
    const auto before = runtime->ProductionDiagnostics(token, 0);
    ASSERT_TRUE(before);
    const auto after = runtime->ProductionDiagnostics(token, 100);
    ASSERT_TRUE(after);
    EXPECT_EQ(before->reservation, after->reservation);
    EXPECT_EQ(before->attempts, after->attempts);
    EXPECT_EQ(before->schedulingTick, after->schedulingTick);
    EXPECT_TRUE(after->reservedToken.Matches(token));
}
}
}
