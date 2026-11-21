#include <gtest/gtest.h>

#include "terrain/terrainCompositedResidency.h"

namespace VL
{
namespace
{
class TerrainCompositedCandidateTest : public testing::Test
{
protected:
    void SetUp() override
    {
        desc.surfaceId = "island";
        desc.width = desc.height = 256;
        desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
        desc.encoding = "candidate-cpu-injection";
        limits.slots = 8;
        limits.uploadsPerFrame = 8;
        limits.uploadBytesPerFrame = desc.PageBytes() * 8;
        pool = std::make_shared<TerrainCompositedResidencyPool>(limits.slots, 1);
    }

    std::unique_ptr<TerrainCompositedResidency> Namespace(
        const std::string& id, uint64_t revision = 1)
    {
        return std::make_unique<TerrainCompositedResidency>(
            nullptr, desc, 42, revision, limits, pool, id);
    }

    TerrainCompositedVTPageKey Root(const std::string& id) const
    {
        return {id, desc.Layout().MipCount() - 1, 0, 0};
    }

    TerrainCompositedPageUpload Publish(TerrainCompositedResidency& residency,
        const TerrainCompositedVTPageKey& key, uint64_t statusEpoch, uint64_t publishEpoch)
    {
        const auto reservation = residency.ReserveProduction(residency.Token(key), statusEpoch);
        EXPECT_TRUE(reservation);
        if (!reservation)
            return {};
        EXPECT_TRUE(residency.CompleteProduction(*reservation, statusEpoch, true));
        const auto batch = residency.BeginUploads(statusEpoch);
        EXPECT_EQ(batch.uploads.size(), 1u);
        residency.Commit(batch.id, publishEpoch);
        return *reservation;
    }

    TerrainCompositedVTBuildDesc desc;
    TerrainCompositedResidencyBudget limits;
    std::shared_ptr<TerrainCompositedResidencyPool> pool;
};

TEST_F(TerrainCompositedCandidateTest, SameLogicalSurfaceHasIndependentNamespacesAndOriginalContentHash)
{
    auto current = Namespace("old", 1);
    auto candidate = Namespace("new", 2);
    const auto oldPage = Publish(*current, Root("old"), 1, 2);
    const auto newPage = Publish(*candidate, Root("new"), 3, 4);
    EXPECT_NE(oldPage.slot, newPage.slot);
    EXPECT_EQ(candidate->Token(Root("new")).contentHash, desc.ContentHash());
    EXPECT_EQ(current->Resolve({"old", 0, 0, 0}), oldPage.slot);
    EXPECT_EQ(candidate->Resolve({"new", 0, 0, 0}), newPage.slot);
    EXPECT_FALSE(current->Resolve(Root("new")));
    EXPECT_FALSE(candidate->AllCurrent({Root("old")}));
    EXPECT_TRUE(current->CurrentRoot());
    EXPECT_TRUE(candidate->CurrentRoot());
}

TEST_F(TerrainCompositedCandidateTest, StatusAndIndependentTableFenceGateEverySelectedPage)
{
    auto candidate = Namespace("new", 2);
    const TerrainCompositedVTPageKey selected{"new", 0, 1, 0};
    candidate->ConsumeFeedback({candidate->Token(selected)});
    const auto required = candidate->SelectedPages();
    ASSERT_EQ(required.size(), 2u);
    EXPECT_FALSE(candidate->AllCurrent(required, 100));
    const auto root = candidate->ReserveProduction(candidate->Token(Root("new")), 0);
    ASSERT_TRUE(root);
    auto waiting = candidate->BeginUploads(5);
    EXPECT_TRUE(waiting.uploads.empty());
    candidate->Abort(waiting.id);
    ASSERT_TRUE(candidate->CompleteProduction(*root, 8, true));
    waiting = candidate->BeginUploads(7);
    EXPECT_TRUE(waiting.uploads.empty());
    candidate->Abort(waiting.id);
    const auto table = candidate->BeginUploads(8);
    ASSERT_EQ(table.uploads.size(), 1u);
    EXPECT_FALSE(candidate->AllCurrent(required, 100));
    candidate->Commit(table.id, 9);
    EXPECT_FALSE(candidate->AllCurrent(required, 9));
    Publish(*candidate, selected, 10, 11);
    EXPECT_FALSE(candidate->AllCurrent(required, 10));
    EXPECT_TRUE(candidate->AllCurrent(required, 11));
}

TEST_F(TerrainCompositedCandidateTest, FailedTableSubmissionKeepsCurrentAndRetriesVerifiedOutput)
{
    auto current = Namespace("old", 1);
    auto candidate = Namespace("new", 2);
    Publish(*current, Root("old"), 1, 2);
    const auto reservation = candidate->ReserveProduction(candidate->Token(Root("new")), 2);
    ASSERT_TRUE(reservation);
    ASSERT_TRUE(candidate->CompleteProduction(*reservation, 3, true));
    const auto failed = candidate->BeginUploads(3);
    ASSERT_EQ(failed.uploads.size(), 1u);
    candidate->Abort(failed.id);
    EXPECT_FALSE(candidate->AllCurrent(candidate->SelectedPages(), 100));
    EXPECT_TRUE(current->AllCurrent(current->SelectedPages(), 100));
    const auto retry = candidate->BeginUploads(3);
    ASSERT_EQ(retry.uploads.size(), 1u);
    EXPECT_EQ(retry.uploads.front().reservation, reservation->reservation);
    candidate->Commit(retry.id, 4);
    EXPECT_FALSE(candidate->AllCurrent(candidate->SelectedPages(), 3));
    EXPECT_TRUE(candidate->AllCurrent(candidate->SelectedPages(), 4));
}

TEST_F(TerrainCompositedCandidateTest, FailedStatusNeverPublishesCandidateAndRetainsOldRoot)
{
    auto current = Namespace("old", 1);
    auto candidate = Namespace("new", 2);
    Publish(*current, Root("old"), 1, 2);
    const auto reservation = candidate->ReserveProduction(candidate->Token(Root("new")), 2);
    ASSERT_TRUE(reservation);
    EXPECT_FALSE(candidate->CompleteProduction(*reservation, 7, false));
    const auto batch = candidate->BeginUploads(6);
    EXPECT_TRUE(batch.uploads.empty());
    candidate->Abort(batch.id);
    EXPECT_EQ(candidate->Statistics().retiring, 1u);
    EXPECT_FALSE(candidate->AllCurrent(candidate->SelectedPages(), 100));
    EXPECT_TRUE(current->AllCurrent(current->SelectedPages(), 100));
}

TEST_F(TerrainCompositedCandidateTest, CancelBeforeLateStatusQuarantinesReservationUntilProducerCompletes)
{
    auto current = Namespace("old", 1);
    auto cancelled = Namespace("cancelled", 2);
    Publish(*current, Root("old"), 1, 2);
    const auto outstanding = cancelled->ReserveProduction(cancelled->Token(Root("cancelled")), 2);
    ASSERT_TRUE(outstanding);
    cancelled->RetireAll(3);
    EXPECT_TRUE(cancelled->ProductionRequests().empty());
    EXPECT_FALSE(cancelled->Retired());
    auto next = Namespace("next", 3);
    const auto nextPage = next->ReserveProduction(next->Token(Root("next")), 50);
    ASSERT_TRUE(nextPage);
    EXPECT_NE(nextPage->slot, outstanding->slot);
    EXPECT_FALSE(cancelled->CompleteProduction(*outstanding, 60, true));
    auto beforeFence = next->BeginUploads(59);
    next->Abort(beforeFence.id);
    EXPECT_FALSE(cancelled->Retired());
    auto afterFence = next->BeginUploads(60);
    next->Abort(afterFence.id);
    EXPECT_TRUE(cancelled->Retired());
    EXPECT_TRUE(current->CurrentRoot());
    EXPECT_FALSE(next->CompleteProduction(*outstanding, 60, true));
}

TEST_F(TerrainCompositedCandidateTest, CancelVerifiedOutputWaitsForItsActualStatusEpoch)
{
    auto cancelled = Namespace("cancelled", 2);
    const auto page = cancelled->ReserveProduction(cancelled->Token(Root("cancelled")), 0);
    ASSERT_TRUE(page);
    ASSERT_TRUE(cancelled->CompleteProduction(*page, 15, true));
    cancelled->RetireAll(2);
    auto next = Namespace("next", 3);
    auto batch = next->BeginUploads(14);
    next->Abort(batch.id);
    EXPECT_FALSE(cancelled->Retired());
    batch = next->BeginUploads(15);
    next->Abort(batch.id);
    EXPECT_TRUE(cancelled->Retired());
}

TEST_F(TerrainCompositedCandidateTest, ReusedSlotRejectsOldReservationEvenWhenFullTokenMatches)
{
    auto residency = Namespace("new", 2);
    const auto old = residency->ReserveProduction(residency->Token(Root("new")), 0);
    ASSERT_TRUE(old);
    EXPECT_FALSE(residency->CompleteProduction(*old, 5, false));
    for (uint32_t tick = 0; tick < 4; ++tick)
    {
        const auto batch = residency->BeginUploads(5);
        residency->Abort(batch.id);
    }
    const auto retry = residency->ReserveProduction(residency->Token(Root("new")), 5);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->slot, old->slot);
    EXPECT_NE(retry->reservation, old->reservation);
    EXPECT_FALSE(residency->OwnsProduction(*old));
    EXPECT_FALSE(residency->CompleteProduction(*old, 6, true));
    EXPECT_TRUE(residency->OwnsProduction(*retry));
    EXPECT_TRUE(residency->CompleteProduction(*retry, 6, true));
}

TEST_F(TerrainCompositedCandidateTest, RetiredSlotsWaitForActualOldFrameEpochAndDescriptorReferences)
{
    auto current = Namespace("old", 1);
    const auto oldPage = Publish(*current, Root("old"), 1, 2);
    auto oldDescriptor = std::make_shared<uint32_t>(1);
    current->MarkReferenced(12);
    current->RetireAll(3, oldDescriptor);
    auto next = Namespace("next", 2);
    auto batch = next->BeginUploads(100);
    next->Abort(batch.id);
    EXPECT_FALSE(current->Retired());
    oldDescriptor.reset();
    batch = next->BeginUploads(11);
    next->Abort(batch.id);
    EXPECT_FALSE(current->Retired());
    batch = next->BeginUploads(12);
    next->Abort(batch.id);
    EXPECT_TRUE(current->Retired());
    const auto reused = next->ReserveProduction(next->Token(Root("next")), 12);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->slot, oldPage.slot);
}

TEST_F(TerrainCompositedCandidateTest, NewRootReservationsAndFreeWatermarkSharePhysicalCapacity)
{
    limits.slots = 6;
    pool = std::make_shared<TerrainCompositedResidencyPool>(6, 2);
    auto oldA = Namespace("old-a", 1);
    auto oldB = Namespace("old-b", 1);
    auto newA = Namespace("new-a", 2);
    auto newB = Namespace("new-b", 2);
    Publish(*oldA, Root("old-a"), 1, 2);
    Publish(*oldB, Root("old-b"), 3, 4);
    const TerrainCompositedVTPageKey fine{"old-a", 0, 0, 0};
    oldA->ConsumeFeedback({oldA->Token(fine)});
    EXPECT_FALSE(oldA->ReserveProduction(oldA->Token(fine), 4));
    Publish(*newA, Root("new-a"), 5, 6);
    Publish(*newB, Root("new-b"), 7, 8);
    EXPECT_EQ(pool->Capacity(), 6u);
    EXPECT_FALSE(oldA->ReserveProduction(oldA->Token(fine), 8));
    EXPECT_THROW(Namespace("extra", 3), std::runtime_error);
}

TEST_F(TerrainCompositedCandidateTest, MultipleDirtySurfacesRequireJointReadinessAndKeepImmutableTokens)
{
    auto oldA = Namespace("old-a", 1);
    auto oldB = Namespace("old-b", 1);
    Publish(*oldA, Root("old-a"), 1, 2);
    Publish(*oldB, Root("old-b"), 1, 2);
    const auto immutableToken = oldA->Token(Root("old-a"));
    const auto immutableDesc = desc;
    desc.dependencyHash.bytes[0] ^= 0x7f;
    auto newA = Namespace("new-a", 2);
    auto newB = Namespace("new-b", 2);
    Publish(*newA, Root("new-a"), 3, 4);
    const auto jointReady = [&](uint64_t completedEpoch) {
        return newA->AllCurrent(newA->SelectedPages(), completedEpoch) &&
            newB->AllCurrent(newB->SelectedPages(), completedEpoch);
    };
    EXPECT_FALSE(jointReady(100));
    EXPECT_TRUE(immutableToken.Matches(oldA->Token(Root("old-a"))));
    EXPECT_EQ(immutableToken.contentHash, immutableDesc.ContentHash());
    Publish(*newB, Root("new-b"), 5, 6);
    EXPECT_FALSE(jointReady(5));
    EXPECT_TRUE(jointReady(6));
    EXPECT_TRUE(oldA->CurrentRoot());
    EXPECT_TRUE(oldB->CurrentRoot());
    EXPECT_FALSE(newA->ReserveProduction(immutableToken, 6));
}

TEST_F(TerrainCompositedCandidateTest, Full512PoolCountsActiveCandidateRetiringAndFourFreeSlotsTogether)
{
    limits.slots = 512;
    pool = std::make_shared<TerrainCompositedResidencyPool>(512, 4);
    desc.width = desc.height = 8192;
    auto old = Namespace("old", 1);
    auto candidate = Namespace("new", 2);
    Publish(*old, Root("old"), 1, 2);
    std::vector<TerrainCompositedVTTaskToken> requests;
    for (uint32_t index = 0; index < 507; ++index)
        requests.push_back(old->Token({"old", 0, index % 64, index / 64}));
    old->ConsumeFeedback(requests);
    uint64_t epoch = 2;
    for (uint32_t index = 0; index < 506; ++index)
    {
        Publish(*old, requests[index].key, epoch, epoch + 1);
        ++epoch;
    }
    EXPECT_FALSE(old->ReserveProduction(requests.back(), epoch));
    const auto root = Publish(*candidate, Root("new"), epoch, epoch + 1);
    EXPECT_EQ(root.slot, 507u);
    EXPECT_FALSE(old->ReserveProduction(requests.back(), epoch + 1));
    old->RetireAll(epoch + 10);
    auto batch = candidate->BeginUploads(epoch + 9);
    candidate->Abort(batch.id);
    EXPECT_EQ(old->Statistics().retiring, 507u);
    EXPECT_EQ(candidate->Statistics().resident, 1u);
    EXPECT_EQ(pool->Capacity(), 512u);
    batch = candidate->BeginUploads(epoch + 10);
    candidate->Abort(batch.id);
    EXPECT_TRUE(old->Retired());
}
}
}
