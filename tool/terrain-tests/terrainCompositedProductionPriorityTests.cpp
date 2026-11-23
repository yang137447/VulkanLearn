#include "terrainCompositedRuntimeCandidateTests.cpp"
#include "render/terrain/terrainCompositedProductionPriority.h"

#include <fstream>
#include <iterator>

namespace VL
{
namespace
{
class TerrainProductionPriorityTest : public TerrainCompositedRuntimeCandidateTest
{
protected:
    void Initialize(uint32_t count = 1)
    {
        runtime.reset();
        desc.width = desc.height = 8192;
        budget.uploadsPerFrame = budget.surface.uploadsPerFrame = 1;
        runtime = std::make_unique<TerrainCompositedRuntime>(backend, budget);
        for (uint32_t index = 0; index < count; ++index)
        {
            auto surface = desc;
            surface.surfaceId = "island" + std::to_string(index);
            runtime->RegisterPreviewSurface(surface, 42, 1);
            ids.push_back(surface.surfaceId);
        }
    }

    bool IsRoot(const TerrainCompositedVTTaskToken& token) const
    {
        return token.key.mip == desc.Layout().MipCount() - 1;
    }

    void Complete(const TerrainCompositedVTTaskToken& token, bool success = true)
    {
        auto page = runtime->ReserveProduction(token, epoch);
        ASSERT_TRUE(page);
        EXPECT_EQ(runtime->CompleteProduction(*page, ++epoch, success), success);
        Record(epoch);
        runtime->Commit(++epoch);
    }

    void PublishRoots()
    {
        for (const auto& id : ids)
            Complete(runtime->Token(Root(id)));
    }

    std::vector<TerrainCompositedVTTaskToken> Demand() const
    {
        std::vector<TerrainCompositedVTTaskToken> result;
        for (const auto& id : ids)
        {
            for (uint32_t x = 0; x < 14; ++x)
                for (uint32_t y = 0; y < 14; ++y)
                    result.push_back(runtime->Token({id, 0, x, y}));
            for (uint32_t x : {31u, 32u})
                for (uint32_t y : {31u, 32u})
                {
                    result.push_back(runtime->Token({id, 0, x, y}));
                    result.push_back(runtime->Token({id, 1, x / 2, y / 2}));
                }
        }
        return result;
    }

    bool Dispatch(TerrainCompositedProductionPriority& selector, bool candidateOnly = false)
    {
        auto requests = runtime->ProductionRequests();
        if (candidateOnly)
            requests.erase(std::remove_if(requests.begin(), requests.end(),
                [&](const auto& token) { return !runtime->CandidateToken(token); }), requests.end());
        selector.Prepare(requests);
        auto decision = selector.Pick(requests, [&](const auto& token) { return IsRoot(token); });
        if (!decision)
            return false;
        const auto token = requests[decision->index];
        Complete(token);
        selector.CommitSuccessful(requests, *decision);
        history.push_back(token);
        return true;
    }

    uint32_t CoveredSamples(const std::string& id) const
    {
        const auto binding = runtime->Binding(id);
        const auto& entries = backend.images.at(binding.compositedPageTable->getImage()).entries;
        uint32_t count = 0;
        for (uint32_t x : {31u, 32u})
            for (uint32_t y : {31u, 32u})
                for (uint32_t mip = 0; mip <= 1; ++mip)
                {
                    const auto found = entries.find({mip, x >> mip, y >> mip});
                    if (found == entries.end() || found->second[0] < 1)
                        continue;
                    const auto slot = runtime->SlotDiagnostics(uint32_t(found->second[0]) - 1, epoch);
                    if (slot && slot->state == TerrainCompositedDiagnosticSlotState::Resident &&
                        slot->publishedToken.Matches(runtime->Token({id, mip, x >> mip, y >> mip})) &&
                        slot->publishEpoch <= epoch)
                    {
                        count += 4096;
                        break;
                    }
                }
        return count;
    }

    uint64_t epoch = 0;
    std::vector<std::string> ids;
    std::vector<TerrainCompositedVTTaskToken> history;
};

TEST_F(TerrainProductionPriorityTest, CoarsePairPublishesByFiveAndEveryFinePageEventuallyDrains)
{
    Initialize();
    PublishRoots();
    runtime->ConsumeFeedback(Demand());
    TerrainCompositedProductionPriority selector(*owner, 512);
    for (uint32_t frame = 0; frame < 5; ++frame)
        ASSERT_TRUE(Dispatch(selector));
    EXPECT_EQ(CoveredSamples(ids.front()), 16384u);
    uint32_t frames = 5;
    while (Dispatch(selector))
        ++frames;
    EXPECT_EQ(frames, 204u);
    EXPECT_EQ(runtime->Statistics().surfaces.at(ids.front()).resident, 205u);
}

TEST_F(TerrainProductionPriorityTest, BothNamespacesReceiveCoarseCoverageAndFineProgress)
{
    Initialize(2);
    PublishRoots();
    runtime->ConsumeFeedback(Demand());
    TerrainCompositedProductionPriority selector(*owner, 512);
    for (uint32_t frame = 0; frame < 12; ++frame)
        ASSERT_TRUE(Dispatch(selector));
    for (const auto& id : ids)
    {
        EXPECT_EQ(CoveredSamples(id), 16384u);
        EXPECT_TRUE(std::any_of(history.begin(), history.end(), [&](const auto& token) {
            return token.key.surfaceId == id && token.key.mip == 0;
        }));
    }
}

TEST_F(TerrainProductionPriorityTest, RootsFirstAndCandidateRequiredPagesStillJointCommit)
{
    Initialize();
    runtime->ConsumeFeedback(Demand());
    TerrainCompositedProductionPriority selector(*owner, 512);
    ASSERT_TRUE(Dispatch(selector));
    ASSERT_TRUE(IsRoot(history.front()));
    auto candidate = desc;
    candidate.surfaceId = ids.front();
    candidate.dependencyHash.bytes[0] = 2;
    runtime->BeginCandidate({candidate}, 42, 2, candidateOwner);
    uint32_t frames = 0;
    while (Dispatch(selector, true))
        ++frames;
    EXPECT_EQ(frames, 205u);
    ASSERT_TRUE(runtime->CandidateReady(epoch));
    ASSERT_TRUE(runtime->PrepareJointSwitch(epoch));
    ASSERT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Token({ids.front(), 0, 31, 31}).revision, 2u);
}

TEST_F(TerrainProductionPriorityTest, ContinuousCoarseArrivalsCannotStarveFineOrLowerPositiveMip)
{
    Initialize();
    PublishRoots();
    auto selected = Demand();
    TerrainCompositedProductionPriority selector(*owner, 512);
    const auto persistent = runtime->Token({ids.front(), 1, 15, 15});
    for (uint32_t frame = 0; frame < 40; ++frame)
    {
        selected.push_back(runtime->Token({ids.front(), 2, frame % 16, frame / 16}));
        runtime->ConsumeFeedback(selected);
        ASSERT_TRUE(Dispatch(selector));
    }
    EXPECT_TRUE(std::any_of(history.begin(), history.end(),
        [&](const auto& token) { return token.Matches(persistent); }));
    EXPECT_GE(std::count_if(history.begin(), history.end(),
        [](const auto& token) { return token.key.mip == 0; }), 10);
}

TEST_F(TerrainProductionPriorityTest, AbandonedPickDoesNotSpendFairTurnAndMetadataReturns)
{
    Initialize();
    PublishRoots();
    runtime->ConsumeFeedback(Demand());
    const auto before = owner->Statistics().current.cpuBytes;
    {
        TerrainCompositedProductionPriority selector(*owner, 512);
        auto requests = runtime->ProductionRequests();
        selector.Prepare(requests);
        {
            auto abandoned = selector.Pick(requests, [&](const auto& token) { return IsRoot(token); });
            ASSERT_TRUE(abandoned);
            const auto first = abandoned->index;
            auto repeated = selector.Pick(requests, [&](const auto& token) { return IsRoot(token); });
            ASSERT_TRUE(repeated);
            EXPECT_EQ(first, repeated->index);
        }
        EXPECT_GT(owner->Statistics().current.cpuBytes, before);
        selector.Prepare({});
    }
    EXPECT_EQ(owner->Statistics().current.cpuBytes, before);
}

TEST_F(TerrainProductionPriorityTest, StatusFailureKeepsRuntimeRetryAndNeverPublishesFailedPage)
{
    Initialize();
    PublishRoots();
    runtime->ConsumeFeedback(Demand());
    TerrainCompositedProductionPriority selector(*owner, 512);
    auto requests = runtime->ProductionRequests();
    selector.Prepare(requests);
    auto decision = selector.Pick(requests, [&](const auto& token) { return IsRoot(token); });
    ASSERT_TRUE(decision);
    const auto token = requests[decision->index];
    auto page = runtime->ReserveProduction(token, epoch);
    ASSERT_TRUE(page);
    selector.CommitSuccessful(requests, *decision);
    runtime->CompleteProduction(*page, ++epoch, false);
    auto status = runtime->ProductionDiagnostics(token, epoch);
    ASSERT_TRUE(status);
    EXPECT_FALSE(runtime->SlotDiagnostics(page->slot, epoch)->publishedToken.Matches(token));
    EXPECT_GT(status->attempts, 0u);
    EXPECT_LT(CoveredSamples(ids.front()), 16384u);
}

TEST_F(TerrainProductionPriorityTest, RecordedAbortPreservesSpentTurnAndOtherNamespaceCanProgress)
{
    Initialize(2);
    PublishRoots();
    runtime->ConsumeFeedback(Demand());
    TerrainCompositedProductionPriority selector(*owner, 512);
    auto requests = runtime->ProductionRequests();
    selector.Prepare(requests);
    auto decision = selector.Pick(requests, [&](const auto& token) { return IsRoot(token); });
    ASSERT_TRUE(decision);
    const auto aborted = requests[decision->index];
    auto page = runtime->ReserveProduction(aborted, epoch);
    ASSERT_TRUE(page);
    selector.CommitSuccessful(requests, *decision);
    runtime->Abort();
    runtime->CompleteProduction(*page, 0, false);
    requests = runtime->ProductionRequests();
    selector.Prepare(requests);
    auto next = selector.Pick(requests, [&](const auto& token) { return IsRoot(token); });
    ASSERT_TRUE(next);
    EXPECT_NE(requests[next->index].key.surfaceId, aborted.key.surfaceId);
    EXPECT_FALSE(runtime->SlotDiagnostics(page->slot, epoch)->publishedToken.Matches(aborted));
}

TEST(TerrainProductionPriorityWiring, ActualServiceConsumesSelectorWithoutLegacyRotation)
{
    std::ifstream input(VL_TERRAIN_SERVICE_SOURCE);
    ASSERT_TRUE(input);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    const auto begin = source.find("void TerrainCompositedService::Impl::RecordProduction(");
    const auto end = source.find("void TerrainCompositedService::Impl::SnapshotStats(", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const auto body = source.substr(begin, end - begin);
    EXPECT_EQ(body.find("std::rotate"), std::string::npos);
    EXPECT_EQ(source.find("productionCursor"), std::string::npos);
    EXPECT_LT(body.find("return !eligible(token)"), body.find("productionPriority->Prepare(requests)"));
    EXPECT_LT(body.find("productionPriority->Prepare(requests)"), body.find("productionPriority->Pick(requests, isRoot)"));
    EXPECT_LT(body.find("task.recorded = true;"), body.find("productionPriority->CommitSuccessful(requests, *decision)"));
}
}
}
