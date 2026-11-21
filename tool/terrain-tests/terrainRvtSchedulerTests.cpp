#include <gtest/gtest.h>

#include "terrain/terrainRvtScheduler.h"

#include <limits>
#include <set>

namespace VL
{
namespace
{
TerrainRvtSchedulerConfig SchedulerConfig()
{
    return {8, 3, 32, 3, 2, 8, 4, 1};
}

class TerrainRvtSchedulerTest : public testing::Test
{
protected:
    TerrainRvtSurfaceIdentity identity{1, 0, "snapshot", "content", "tile"};
    TerrainRvtScheduler scheduler{SchedulerConfig()};
    uint64_t epoch = 0;

    void SetUp() override
    {
        scheduler.SetSurfaceIdentity(identity);
    }

    TerrainRvtPageIdentity Page(uint32_t mip, uint32_t x = 0) const
    {
        return {identity, mip, x, 0};
    }

    TerrainRvtProductionTask MakeResident(
        const TerrainRvtPageIdentity& page, bool root, uint64_t now = 0)
    {
        EXPECT_TRUE(scheduler.Request(page, root, false, now));
        const auto task = scheduler.Begin(now, epoch);
        EXPECT_TRUE(task);
        if (!task)
            return {};
        EXPECT_TRUE(scheduler.Submitted(*task, ++epoch));
        EXPECT_TRUE(scheduler.Complete(*task, epoch, TerrainRvtProductionResult::Success, now));
        EXPECT_TRUE(scheduler.Publish(*task, ++epoch));
        return *task;
    }
};

TEST_F(TerrainRvtSchedulerTest, RootRequiresFenceStatusAndSeparatePageTableSubmission)
{
    ASSERT_TRUE(scheduler.Request(Page(13), true, false, 0));
    const auto task = scheduler.Begin(0, 0);
    ASSERT_TRUE(task);
    EXPECT_EQ(scheduler.SlotState(task->physicalSlot), TerrainRvtSlotState::Reserved);
    EXPECT_FALSE(scheduler.Publish(*task, 1));
    ASSERT_TRUE(scheduler.Submitted(*task, 1));
    EXPECT_FALSE(scheduler.Complete(*task, 0, TerrainRvtProductionResult::Success, 0));
    ASSERT_TRUE(scheduler.Complete(*task, 1, TerrainRvtProductionResult::Success, 0));
    EXPECT_EQ(scheduler.SlotState(task->physicalSlot), TerrainRvtSlotState::Reserved);
    EXPECT_FALSE(scheduler.Publish(*task, 1));
    EXPECT_TRUE(scheduler.Publish(*task, 2));
    EXPECT_EQ(scheduler.SlotState(task->physicalSlot), TerrainRvtSlotState::Resident);
    EXPECT_EQ(scheduler.Statistics().productionInUse, 0u);
}

TEST_F(TerrainRvtSchedulerTest, FineProductionWaitsForItsOwnSurfaceRoot)
{
    ASSERT_TRUE(scheduler.Request(Page(0), false, false, 0));
    EXPECT_FALSE(scheduler.Begin(0, 0));
    MakeResident(Page(13), true);
    const auto fine = scheduler.Begin(0, epoch);
    ASSERT_TRUE(fine);
    EXPECT_EQ(fine->page, Page(0));
}

TEST_F(TerrainRvtSchedulerTest, SameFrameTasksHaveIndependentPhysicalAndProductionSlots)
{
    MakeResident(Page(13), true);
    for (uint32_t index = 0; index < 3; ++index)
        ASSERT_TRUE(scheduler.Request(Page(0, index), false, false, 0));
    std::set<uint32_t> physical;
    std::set<uint32_t> production;
    std::set<uint64_t> serials;
    for (uint32_t index = 0; index < 3; ++index)
    {
        const auto task = scheduler.Begin(0, epoch);
        ASSERT_TRUE(task);
        physical.insert(task->physicalSlot);
        production.insert(task->productionSlot);
        serials.insert(task->serial);
    }
    EXPECT_EQ(physical.size(), 3u);
    EXPECT_EQ(production.size(), 3u);
    EXPECT_EQ(serials.size(), 3u);
    EXPECT_FALSE(scheduler.Begin(0, epoch));
}

TEST_F(TerrainRvtSchedulerTest, RetryBackoffIsBoundedAndOldSerialCannotCompleteNewAttempt)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto first = scheduler.Begin(0, 0);
    ASSERT_TRUE(first);
    ASSERT_TRUE(scheduler.Submitted(*first, 1));
    ASSERT_TRUE(scheduler.Complete(*first, 1, TerrainRvtProductionResult::RetryableFailure, 0));
    EXPECT_EQ(scheduler.State(Page(13)), TerrainRvtTaskState::FailedBackoff);
    EXPECT_FALSE(scheduler.Begin(1, 1));
    const auto second = scheduler.Begin(2, 1);
    ASSERT_TRUE(second);
    EXPECT_NE(second->serial, first->serial);
    EXPECT_FALSE(scheduler.Complete(*first, 2, TerrainRvtProductionResult::Success, 2));
    ASSERT_TRUE(scheduler.Submitted(*second, 2));
    ASSERT_TRUE(scheduler.Complete(*second, 2, TerrainRvtProductionResult::RetryableFailure, 2));
    EXPECT_FALSE(scheduler.Begin(5, 2));
    const auto third = scheduler.Begin(6, 2);
    ASSERT_TRUE(third);
    ASSERT_TRUE(scheduler.Submitted(*third, 3));
    ASSERT_TRUE(scheduler.Complete(*third, 3, TerrainRvtProductionResult::RetryableFailure, 6));
    EXPECT_EQ(scheduler.State(Page(13)), TerrainRvtTaskState::Failed);
    EXPECT_FALSE(scheduler.Begin(100, 3));
}

TEST_F(TerrainRvtSchedulerTest, PermanentFailureDoesNotRetry)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto task = scheduler.Begin(0, 0);
    ASSERT_TRUE(task);
    ASSERT_TRUE(scheduler.Submitted(*task, 1));
    ASSERT_TRUE(scheduler.Complete(*task, 1, TerrainRvtProductionResult::PermanentFailure, 0));
    EXPECT_EQ(scheduler.State(Page(13)), TerrainRvtTaskState::Failed);
    EXPECT_FALSE(scheduler.Begin(100, 1));
    EXPECT_EQ(scheduler.Statistics().free, 8u);
}

TEST_F(TerrainRvtSchedulerTest, UnsubmittedAbortReleasesSlotsButSubmittedAbortIsRejected)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto first = scheduler.Begin(0, 0);
    ASSERT_TRUE(first);
    ASSERT_TRUE(scheduler.AbortUnsubmitted(*first, 0));
    EXPECT_EQ(scheduler.Statistics().free, 8u);
    const auto second = scheduler.Begin(2, 0);
    ASSERT_TRUE(second);
    ASSERT_TRUE(scheduler.Submitted(*second, 1));
    EXPECT_FALSE(scheduler.AbortUnsubmitted(*second, 2));
    EXPECT_EQ(scheduler.Statistics().reserved, 1u);
}

TEST_F(TerrainRvtSchedulerTest, RevisionReplacementRetainsSubmittedSlotsUntilFence)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto old = scheduler.Begin(0, 0);
    ASSERT_TRUE(old);
    ASSERT_TRUE(scheduler.Submitted(*old, 5));
    ++identity.revision;
    identity.contentIdentity = "edited";
    scheduler.SetSurfaceIdentity(identity);
    EXPECT_EQ(scheduler.SlotState(old->physicalSlot), TerrainRvtSlotState::Retiring);
    scheduler.Collect(4);
    EXPECT_EQ(scheduler.Statistics().productionInUse, 1u);
    EXPECT_FALSE(scheduler.Publish(*old, 6));
    scheduler.Collect(5);
    EXPECT_EQ(scheduler.SlotState(old->physicalSlot), TerrainRvtSlotState::Free);
    EXPECT_EQ(scheduler.Statistics().productionInUse, 0u);
    EXPECT_FALSE(scheduler.Complete(*old, 5, TerrainRvtProductionResult::Success, 0));
}

TEST_F(TerrainRvtSchedulerTest, ReadyResultFromPreviousSnapshotCannotPublish)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto task = scheduler.Begin(0, 0);
    ASSERT_TRUE(task);
    ASSERT_TRUE(scheduler.Submitted(*task, 1));
    ASSERT_TRUE(scheduler.Complete(*task, 1, TerrainRvtProductionResult::Success, 0));
    identity.snapshotIdentity = "new-snapshot";
    scheduler.SetSurfaceIdentity(identity);
    EXPECT_FALSE(scheduler.Publish(*task, 2));
    EXPECT_EQ(scheduler.Statistics().free, 8u);
}

TEST_F(TerrainRvtSchedulerTest, RootRemainsPinnedAndEvictedFineSlotWaitsForUnmapEpoch)
{
    const auto root = MakeResident(Page(13), true);
    const auto fine = MakeResident(Page(0), false);
    EXPECT_FALSE(scheduler.CommitEviction(root.physicalSlot, epoch + 1));
    EXPECT_EQ(scheduler.EvictionCandidate(), fine.physicalSlot);
    ASSERT_TRUE(scheduler.CommitEviction(fine.physicalSlot, epoch + 1));
    EXPECT_EQ(scheduler.SlotState(fine.physicalSlot), TerrainRvtSlotState::Retiring);
    scheduler.Collect(epoch);
    EXPECT_EQ(scheduler.SlotState(fine.physicalSlot), TerrainRvtSlotState::Retiring);
    scheduler.Collect(epoch + 1);
    EXPECT_EQ(scheduler.SlotState(fine.physicalSlot), TerrainRvtSlotState::Free);
}

TEST_F(TerrainRvtSchedulerTest, ReplacedRootCannotRetireBeforeNewRootIsResident)
{
    const auto oldIdentity = identity;
    const auto root = MakeResident(Page(13), true);
    ++identity.revision;
    identity.contentIdentity = "replacement";
    scheduler.SetSurfaceIdentity(identity);
    EXPECT_FALSE(scheduler.RetireReplacedIdentity(oldIdentity, epoch + 1));
    MakeResident(Page(13), true);
    ASSERT_TRUE(scheduler.RetireReplacedIdentity(oldIdentity, epoch + 1));
    EXPECT_EQ(scheduler.SlotState(root.physicalSlot), TerrainRvtSlotState::Retiring);
    scheduler.Collect(epoch + 1);
    EXPECT_EQ(scheduler.SlotState(root.physicalSlot), TerrainRvtSlotState::Free);
}

TEST_F(TerrainRvtSchedulerTest, CoarseAncestorWinsEqualAgeButOldFineDemandEventuallyWins)
{
    MakeResident(Page(13), true);
    scheduler.Request(Page(0), false, false, 0);
    scheduler.Request(Page(4), false, true, 0);
    const auto coarse = scheduler.Begin(0, epoch);
    ASSERT_TRUE(coarse);
    EXPECT_EQ(coarse->page, Page(4));
    scheduler.Request(Page(3), false, true, 20);
    const auto fine = scheduler.Begin(20, epoch);
    ASSERT_TRUE(fine);
    EXPECT_EQ(fine->page, Page(0));
}

TEST_F(TerrainRvtSchedulerTest, SixteenSurfacesCompeteFairlyWithoutExceedingSharedSlots)
{
    TerrainRvtScheduler world({34, 4, 100, 3, 2, 8, 4, 1});
    std::vector<TerrainRvtSurfaceIdentity> surfaces;
    for (uint32_t index = 0; index < 16; ++index)
    {
        auto surface = identity;
        surface.surfaceId = "tile-" + std::to_string(index);
        world.SetSurfaceIdentity(surface);
        surfaces.push_back(surface);
        ASSERT_TRUE(world.Request({surface, 13, 0, 0}, true, false, 0));
    }
    uint64_t submitted = 0;
    for (uint32_t index = 0; index < 16; ++index)
    {
        const auto task = world.Begin(0, submitted);
        ASSERT_TRUE(task);
        ASSERT_TRUE(world.Submitted(*task, ++submitted));
        ASSERT_TRUE(world.Complete(*task, submitted, TerrainRvtProductionResult::Success, 0));
        ASSERT_TRUE(world.Publish(*task, ++submitted));
    }
    for (const auto& surface : surfaces)
        ASSERT_TRUE(world.Request({surface, 0, 0, 0}, false, false, 0));
    std::set<std::string> served;
    for (uint32_t index = 0; index < 16; ++index)
    {
        const auto task = world.Begin(100, submitted);
        ASSERT_TRUE(task);
        served.insert(task->page.surface.surfaceId);
        ASSERT_TRUE(world.Submitted(*task, ++submitted));
        ASSERT_TRUE(world.Complete(*task, submitted, TerrainRvtProductionResult::Success, 100));
        ASSERT_TRUE(world.Publish(*task, ++submitted));
        const auto stats = world.Statistics();
        EXPECT_EQ(stats.free + stats.reserved + stats.resident + stats.retiring, 34u);
    }
    EXPECT_EQ(served.size(), 16u);
}

TEST_F(TerrainRvtSchedulerTest, FreeWatermarkStopsDispatchUntilAnEvictionCompletes)
{
    TerrainRvtScheduler limited({3, 2, 10, 3, 2, 8, 4, 1});
    limited.SetSurfaceIdentity(identity);
    auto produce = [&](const TerrainRvtPageIdentity& page, bool root) {
        EXPECT_TRUE(limited.Request(page, root, false, 0));
        const auto task = limited.Begin(0, epoch);
        EXPECT_TRUE(task);
        if (!task)
            return;
        EXPECT_TRUE(limited.Submitted(*task, ++epoch));
        EXPECT_TRUE(limited.Complete(*task, epoch, TerrainRvtProductionResult::Success, 0));
        EXPECT_TRUE(limited.Publish(*task, ++epoch));
    };
    produce(Page(13), true);
    produce(Page(0), false);
    limited.Request(Page(0, 1), false, false, 0);
    EXPECT_FALSE(limited.Begin(0, epoch));
    const auto victim = limited.EvictionCandidate();
    ASSERT_TRUE(victim);
    ASSERT_TRUE(limited.CommitEviction(*victim, ++epoch));
    EXPECT_FALSE(limited.Begin(0, epoch - 1));
    EXPECT_TRUE(limited.Begin(0, epoch));
}

TEST_F(TerrainRvtSchedulerTest, CancelledInFlightTaskCannotReleaseItsSlotEarly)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto task = scheduler.Begin(0, 0);
    ASSERT_TRUE(task);
    scheduler.Submitted(*task, 8);
    ASSERT_TRUE(scheduler.CancelRequest(task->page));
    scheduler.Collect(7);
    EXPECT_EQ(scheduler.Statistics().retiring, 1u);
    EXPECT_EQ(scheduler.Statistics().productionInUse, 1u);
    scheduler.Collect(8);
    EXPECT_EQ(scheduler.Statistics().free, 8u);
}

TEST_F(TerrainRvtSchedulerTest, ShutdownRetiresRootsAndRejectsNewWork)
{
    const auto root = MakeResident(Page(13), true);
    scheduler.RetireAll(epoch + 1);
    EXPECT_FALSE(scheduler.Request(Page(0), false, false, 0));
    EXPECT_EQ(scheduler.SlotState(root.physicalSlot), TerrainRvtSlotState::Retiring);
    scheduler.Collect(epoch);
    EXPECT_EQ(scheduler.Statistics().retiring, 1u);
    scheduler.Collect(epoch + 1);
    EXPECT_EQ(scheduler.Statistics().free, 8u);
}

TEST_F(TerrainRvtSchedulerTest, IdentityChecksEveryComponentAndRejectsRegression)
{
    auto changed = identity;
    changed.worldGeneration = 2;
    EXPECT_FALSE(scheduler.Request({changed, 13, 0, 0}, true, false, 0));
    changed = identity;
    changed.revision = 1;
    EXPECT_FALSE(scheduler.Request({changed, 13, 0, 0}, true, false, 0));
    changed = identity;
    changed.snapshotIdentity = "other";
    EXPECT_FALSE(scheduler.Request({changed, 13, 0, 0}, true, false, 0));
    changed = identity;
    changed.contentIdentity = "other";
    EXPECT_FALSE(scheduler.Request({changed, 13, 0, 0}, true, false, 0));
    changed = identity;
    changed.surfaceId = "other";
    EXPECT_FALSE(scheduler.Request({changed, 13, 0, 0}, true, false, 0));
    changed = identity;
    changed.revision = 2;
    scheduler.SetSurfaceIdentity(changed);
    EXPECT_THROW(scheduler.SetSurfaceIdentity(identity), std::invalid_argument);
}

TEST_F(TerrainRvtSchedulerTest, ForgedHandleCannotSubmitAnotherTasksSlots)
{
    scheduler.Request(Page(13), true, false, 0);
    const auto task = scheduler.Begin(0, 0);
    ASSERT_TRUE(task);
    auto forged = *task;
    ++forged.physicalSlot;
    EXPECT_FALSE(scheduler.Submitted(forged, 1));
    forged = *task;
    ++forged.productionSlot;
    EXPECT_FALSE(scheduler.Submitted(forged, 1));
    forged = *task;
    ++forged.serial;
    EXPECT_FALSE(scheduler.Submitted(forged, 1));
    EXPECT_TRUE(scheduler.Submitted(*task, 1));
}

TEST(TerrainRvtSchedulerConfigTest, ExplicitLimitsAreRequiredAndTrackedDemandIsBounded)
{
    EXPECT_THROW(TerrainRvtScheduler((TerrainRvtSchedulerConfig{0, 1, 1, 1, 1, 1, 1, 0})),
        std::invalid_argument);
    TerrainRvtScheduler scheduler({2, 1, 1, 1, 1, 1, 1, 0});
    TerrainRvtSurfaceIdentity identity{1, 0, "snapshot", "content", "tile"};
    scheduler.SetSurfaceIdentity(identity);
    EXPECT_TRUE(scheduler.Request({identity, 13, 0, 0}, true, false, 0));
    EXPECT_FALSE(scheduler.Request({identity, 0, 0, 0}, false, false, 0));
    EXPECT_EQ(scheduler.Statistics().tracked, 1u);
}

TEST(TerrainRvtSchedulerConfigTest, RetryArithmeticSaturatesInsteadOfWrapping)
{
    TerrainRvtScheduler scheduler({2, 1, 8, 3, 4, 16, 1, 0});
    TerrainRvtSurfaceIdentity identity{1, 0, "snapshot", "content", "tile"};
    TerrainRvtPageIdentity root{identity, 13, 0, 0};
    scheduler.SetSurfaceIdentity(identity);
    scheduler.Request(root, true, false, 0);
    const auto task = scheduler.Begin(0, 0);
    ASSERT_TRUE(task);
    scheduler.AbortUnsubmitted(*task, std::numeric_limits<uint64_t>::max() - 1);
    EXPECT_FALSE(scheduler.Begin(std::numeric_limits<uint64_t>::max() - 1, 0));
}

TEST(TerrainRvtSchedulerConfigTest, RootsCannotConsumeCapacityReservedForFreeWatermark)
{
    TerrainRvtScheduler scheduler({3, 1, 10, 3, 1, 8, 4, 1});
    for (uint32_t index = 0; index < 3; ++index)
    {
        TerrainRvtSurfaceIdentity identity{1, 0, "snapshot", "content", std::to_string(index)};
        scheduler.SetSurfaceIdentity(identity);
        EXPECT_EQ(scheduler.Request({identity, 13, 0, 0}, true, false, 0), index < 2);
    }
}

TEST(TerrainRvtSchedulerConfigTest, SixteenSurfacesAtPoolCapacityProgressAcrossUnmapEpochs)
{
    TerrainRvtScheduler scheduler({19, 2, 100, 3, 1, 8, 4, 1});
    std::vector<TerrainRvtSurfaceIdentity> identities;
    uint64_t epoch = 0;
    for (uint32_t index = 0; index < 16; ++index)
    {
        TerrainRvtSurfaceIdentity identity{1, 0, "snapshot", "content", std::to_string(index)};
        identities.push_back(identity);
        scheduler.SetSurfaceIdentity(identity);
        ASSERT_TRUE(scheduler.Request({identity, 13, 0, 0}, true, false, 0));
        const auto root = scheduler.Begin(0, epoch);
        ASSERT_TRUE(root);
        ASSERT_TRUE(scheduler.Submitted(*root, ++epoch));
        ASSERT_TRUE(scheduler.Complete(*root, epoch, TerrainRvtProductionResult::Success, 0));
        ASSERT_TRUE(scheduler.Publish(*root, ++epoch));
    }
    for (const auto& identity : identities)
        ASSERT_TRUE(scheduler.Request({identity, 0, 0, 0}, false, false, 0));
    std::set<std::string> served;
    for (uint32_t step = 0; step < 32 && served.size() < 16; ++step)
    {
        auto task = scheduler.Begin(step, epoch);
        if (!task)
        {
            const auto victim = scheduler.EvictionCandidate();
            ASSERT_TRUE(victim);
            const auto unmapEpoch = ++epoch;
            ASSERT_TRUE(scheduler.CommitEviction(*victim, unmapEpoch));
            EXPECT_FALSE(scheduler.Begin(step, unmapEpoch - 1));
            task = scheduler.Begin(step, unmapEpoch);
        }
        ASSERT_TRUE(task);
        served.insert(task->page.surface.surfaceId);
        ASSERT_TRUE(scheduler.Submitted(*task, ++epoch));
        ASSERT_TRUE(scheduler.Complete(*task, epoch, TerrainRvtProductionResult::Success, step));
        ASSERT_TRUE(scheduler.Publish(*task, ++epoch));
        const auto stats = scheduler.Statistics();
        EXPECT_EQ(stats.free + stats.reserved + stats.resident + stats.retiring, 19u);
        EXPECT_EQ(stats.roots, 16u);
    }
    EXPECT_EQ(served.size(), 16u);
}
}
}
