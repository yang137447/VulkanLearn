#include "terrainCompositedRuntimeCandidateTests.cpp"

#include <set>

namespace VL
{
namespace
{
TEST_F(TerrainCompositedRuntimeCandidateTest, NativeFailureDiagnosticsReportActualAttemptAndBackoffWithoutMutation)
{
    const auto token = runtime->Token(Root());
    const auto reservation = runtime->ReserveProduction(token, 1);
    ASSERT_TRUE(reservation);
    EXPECT_FALSE(runtime->CompleteProduction(*reservation, 2, false));
    const auto first = runtime->ProductionDiagnostics(token, 1);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->attempts, 1u);
    EXPECT_EQ(first->maxAttempts, budget.surface.maxReadAttempts);
    EXPECT_EQ(first->backoffTicks, 2u);
    EXPECT_EQ(first->slot, reservation->slot);
    EXPECT_EQ(first->retireEpoch, 2u);
    EXPECT_FALSE(first->retirementReusable);
    const auto statistics = runtime->Statistics();
    const auto second = runtime->ProductionDiagnostics(token, 2);
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->retirementReusable);
    EXPECT_EQ(second->attempts, first->attempts);
    EXPECT_EQ(second->backoffTicks, first->backoffTicks);
    EXPECT_EQ(runtime->Statistics().surfaces.at("island").readFailures,
        statistics.surfaces.at("island").readFailures);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, NativeSlotDiagnosticsKeepOldDescriptorOwnerUntilLastReference)
{
    PublishCurrent();
    const auto oldToken = runtime->Token(Root());
    auto oldBinding = runtime->Binding("island");
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    ASSERT_TRUE(runtime->CommitCandidate());
    const auto held = runtime->ProductionDiagnostics(oldToken, 100);
    ASSERT_TRUE(held);
    EXPECT_GT(held->retireEpoch, 0u);
    EXPECT_TRUE(held->retirementOwnerAlive);
    EXPECT_FALSE(held->retirementReusable);
    oldBinding = {};
    const auto released = runtime->ProductionDiagnostics(oldToken, 100);
    ASSERT_TRUE(released);
    EXPECT_FALSE(released->retirementOwnerAlive);
    EXPECT_TRUE(released->retirementReusable);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, Full16Selected216NeedsMoreThan180FramesThenActuallyJointCommits)
{
    runtime.reset();
    budget.uploadsPerFrame = budget.surface.uploadsPerFrame = 1;
    runtime = std::make_unique<TerrainCompositedRuntime>(backend, budget);
    std::vector<TerrainCompositedVTBuildDesc> descriptions;
    for (uint32_t index = 0; index < 16; ++index)
    {
        auto surface = desc;
        surface.surfaceId = "terrain|Landscape_" + std::to_string(index);
        surface.width = surface.height = 8192;
        runtime->RegisterPreviewSurface(surface, 42, 1);
        descriptions.push_back(surface);
    }
    uint64_t completedEpoch = 0;
    auto produceFrame = [&](bool candidate) {
        const auto requests = runtime->ProductionRequests();
        const auto found = std::find_if(requests.begin(), requests.end(), [&](const auto& request) {
            return runtime->CandidateToken(request) == candidate;
        });
        if (found == requests.end())
            return std::optional<TerrainCompositedVTPageKey>{};
        const auto reservation = runtime->ReserveProduction(*found, completedEpoch);
        if (!reservation)
            return std::optional<TerrainCompositedVTPageKey>{};
        EXPECT_TRUE(runtime->CompleteProduction(*reservation, ++completedEpoch, true));
        TerrainCompositedFrameUploadBudget allowance;
        allowance.remainingPages = 1;
        allowance.remainingBytes = 4 * 1024 * 1024;
        allowance.remainingSchedulingMicroseconds = 1000000;
        runtime->RecordUploads(vk::CommandBuffer(reinterpret_cast<VkCommandBuffer>(uintptr_t(1))),
            0, completedEpoch, allowance);
        EXPECT_LE(1 - allowance.remainingPages, 1u);
        runtime->Commit(++completedEpoch);
        return std::optional<TerrainCompositedVTPageKey>{found->key};
    };
    for (uint32_t frame = 0; frame < 16; ++frame)
        ASSERT_TRUE(produceFrame(false));
    ASSERT_TRUE(runtime->RootsCurrent());
    std::vector<TerrainCompositedVTTaskToken> selected;
    for (uint32_t index = 0; index < 16; ++index)
        for (uint32_t page = 0; page < (index < 8 ? 13u : 12u); ++page)
            selected.push_back(runtime->Token({descriptions[index].surfaceId, 3, page % 8, page / 8}));
    runtime->ConsumeFeedback(selected);
    for (uint32_t frame = 0; frame < 200; ++frame)
        ASSERT_TRUE(produceFrame(false));
    ASSERT_EQ(runtime->SelectedPages().size(), 216u);
    std::vector<TerrainCompositedMaterialBinding> oldBindings;
    for (const auto& surface : descriptions)
        oldBindings.push_back(runtime->Binding(surface.surfaceId));
    for (auto& surface : descriptions)
        surface.dependencyHash.bytes[0] = 2;
    runtime->BeginCandidate(descriptions, 42, 2, candidateOwner);
    const auto requests = runtime->ProductionRequests();
    ASSERT_EQ(std::count_if(requests.begin(), requests.end(),
        [&](const auto& token) { return runtime->CandidateToken(token); }), 216);
    std::set<TerrainCompositedVTPageKey> produced;
    uint32_t frames = 0;
    for (; frames < 432 && !runtime->CandidateReady(completedEpoch); ++frames)
    {
        const auto page = produceFrame(true);
        ASSERT_TRUE(page) << "Candidate stalled at frame " << frames;
        ASSERT_TRUE(produced.insert(*page).second) << "Candidate repeated an already produced key";
        if (frames == 179)
        {
            EXPECT_FALSE(runtime->CandidateReady(completedEpoch));
            EXPECT_FALSE(runtime->PrepareJointSwitch(completedEpoch));
            for (size_t index = 0; index < descriptions.size(); ++index)
                EXPECT_EQ(runtime->Binding(descriptions[index].surfaceId).compositedPageTable,
                    oldBindings[index].compositedPageTable);
        }
    }
    ASSERT_TRUE(runtime->CandidateReady(completedEpoch));
    EXPECT_EQ(produced.size(), 216u);
    EXPECT_EQ(frames, 216u);
    ASSERT_TRUE(runtime->PrepareJointSwitch(completedEpoch));
    ASSERT_TRUE(runtime->CommitCandidate());
    for (size_t index = 0; index < descriptions.size(); ++index)
    {
        EXPECT_EQ(runtime->Token({descriptions[index].surfaceId, 0, 0, 0}).revision, 2u);
        EXPECT_NE(runtime->Binding(descriptions[index].surfaceId).compositedPageTable,
            oldBindings[index].compositedPageTable);
        EXPECT_EQ(runtime->Binding(descriptions[index].surfaceId).compositedAlbedo,
            oldBindings[index].compositedAlbedo);
    }
}
}
}
