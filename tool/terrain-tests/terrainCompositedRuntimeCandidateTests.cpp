#include <gtest/gtest.h>

#include "render/terrain/terrainCompositedRuntime.h"
#include "render/backend/rendererBackendVulkan.h"
#include "texture.h"

#include <cstdlib>
#include <new>
#include <atomic>
#include <thread>

thread_local int64_t TerrainRuntimeAllocationCountdown = -1;

void* operator new(std::size_t bytes)
{
    if (TerrainRuntimeAllocationCountdown == 0)
        throw std::bad_alloc();
    if (TerrainRuntimeAllocationCountdown > 0)
        --TerrainRuntimeAllocationCountdown;
    if (void* memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
    uint32_t, const VkImageMemoryBarrier*)
{
}

namespace VL
{
struct TerrainCompositedRuntimeTestAccess
{
    static void RetireSecondCandidatePhase(TerrainCompositedRuntime& runtime)
    {
        auto& surface = runtime.surfaces.at(runtime.candidateNamespaces.rbegin()->second);
        surface.tableLease->SetPhase(TerrainRvtResourcePhase::Retiring);
    }
    static TerrainRvtResourcePhase TablePhase(
        TerrainCompositedRuntime& runtime, const std::string& logicalId, bool candidate)
    {
        const auto& namespaces = candidate ? runtime.candidateNamespaces : runtime.activeNamespaces;
        return runtime.surfaces.at(namespaces.at(logicalId)).tableLease->Phase();
    }
    static bool HasPreparedJointSwitch(const TerrainCompositedRuntime& runtime)
    {
        return runtime.jointSwitchPrepared || !runtime.preparedNamespaces.empty() ||
            !runtime.preparedRetirements.empty() || runtime.preparedPhaseBatch.has_value();
    }
};

namespace
{
class TerrainCompositedRuntimeCandidateTest : public testing::Test
{
protected:
    void SetUp() override
    {
        TerrainRvtBudgetCaps caps;
        caps.physicalPoolGpuBytes = caps.inputGpuBytes = caps.workspaceGpuBytes = uint64_t(1) << 30;
        caps.totalGpuBytes = caps.totalCpuBytes = uint64_t(2) << 30;
        caps.physicalPoolCpuBytes = caps.inputCpuBytes = caps.workspaceCpuBytes = caps.totalCpuBytes;
        caps.physicalSlots = 512;
        caps.productionTasks = 4;
        peakOwner = std::make_shared<TerrainRvtBudgetOwner>(caps);
        owner = std::make_shared<TerrainRvtBudgetOwner>(caps, peakOwner);
        candidateOwner = std::make_shared<TerrainRvtBudgetOwner>(caps, peakOwner);
        budget.owner = owner;
        budget.surface.slots = 512;
        budget.surface.cpuBytes = 8 * 1024 * 1024;
        budget.surface.uploadsPerFrame = 16;
        budget.surface.uploadBytesPerFrame = 4 * 1024 * 1024;
        budget.frameRingSize = 2;
        budget.uploadsPerFrame = 16;
        budget.uploadBytesPerFrame = 4 * 1024 * 1024;
        budget.schedulingMicroseconds = 1000000;
        budget.freeSlotReserve = 4;
        budget.cpuBytes = 512 * 1024 * 1024;
        budget.gpuBytes = 1024 * 1024 * 1024;
        runtime = std::make_unique<TerrainCompositedRuntime>(backend, budget);
        desc.surfaceId = "island";
        desc.width = desc.height = 256;
        desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
            TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
        runtime->RegisterPreviewSurface(desc, 42, 1);
    }

    TerrainCompositedVTPageKey Root(const std::string& id = "island") const
    {
        return {id, desc.Layout().MipCount() - 1, 0, 0};
    }

    TerrainCompositedPageUpload Produce(const TerrainCompositedVTTaskToken& token,
        uint64_t epoch, bool success = true)
    {
        const auto page = runtime->ReserveProduction(token, epoch);
        EXPECT_TRUE(page);
        if (!page)
            return {};
        EXPECT_EQ(runtime->CompleteProduction(*page, epoch, success), success);
        return *page;
    }

    void Record(uint64_t completedEpoch)
    {
        runtime->RecordUploads(
            vk::CommandBuffer(reinterpret_cast<VkCommandBuffer>(uintptr_t(1))), 0, completedEpoch);
    }

    void PublishCurrent()
    {
        Produce(runtime->Token(Root()), 1);
        Record(1);
        runtime->Commit(2);
    }

    void BeginCandidate(uint64_t revision = 2)
    {
        auto candidate = desc;
        candidate.dependencyHash.bytes[0] = uint8_t(revision);
        runtime->BeginCandidate({candidate}, 42, revision, candidateOwner);
    }

    TerrainCompositedVTTaskToken CandidateRequest()
    {
        for (const auto& request : runtime->ProductionRequests())
            if (runtime->CandidateToken(request))
                return request;
        ADD_FAILURE() << "Missing candidate production request";
        return {};
    }

    RendererBackendVulkan backend;
    TerrainCompositedRuntimeBudget budget;
    std::shared_ptr<TerrainRvtBudgetOwner> peakOwner;
    std::shared_ptr<TerrainRvtBudgetOwner> owner;
    std::shared_ptr<TerrainRvtBudgetOwner> candidateOwner;
    std::unique_ptr<TerrainCompositedRuntime> runtime;
    TerrainCompositedVTBuildDesc desc;
};

TEST_F(TerrainCompositedRuntimeCandidateTest, IndependentTablesShareAtlasRingAndPoolAccounting)
{
    PublishCurrent();
    auto old = runtime->Binding("island");
    const auto images = backend.imageAllocations;
    const auto buffers = backend.bufferAllocations;
    const auto slots = peakOwner->Statistics().current.physicalSlots;
    BeginCandidate();
    const auto candidate = runtime->CandidateBinding("island");
    EXPECT_NE(old.compositedPageTable, candidate.compositedPageTable);
    EXPECT_EQ(old.compositedAlbedo, candidate.compositedAlbedo);
    EXPECT_EQ(old.compositedNormal, candidate.compositedNormal);
    EXPECT_EQ(old.compositedOrm, candidate.compositedOrm);
    EXPECT_EQ(backend.imageAllocations, images + 1);
    EXPECT_EQ(backend.bufferAllocations, buffers);
    EXPECT_EQ(slots, 512u);
    EXPECT_EQ(peakOwner->Statistics().current.physicalSlots, slots);
    EXPECT_EQ(peakOwner->Statistics().current.productionTasks, 0u);
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, old.compositedPageTable);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, RealRuntimeRejectsSwitchUntilStatusAndTableFenceComplete)
{
    PublishCurrent();
    BeginCandidate();
    const auto token = CandidateRequest();
    const auto reservation = runtime->ReserveProduction(token, 2);
    ASSERT_TRUE(reservation);
    EXPECT_FALSE(runtime->CandidateReady(100));
    EXPECT_FALSE(runtime->CommitCandidate());
    ASSERT_TRUE(runtime->CompleteProduction(*reservation, 8, true));
    Record(7);
    runtime->Commit(8);
    EXPECT_FALSE(runtime->CandidateReady(8));
    Record(8);
    runtime->Commit(9);
    EXPECT_FALSE(runtime->CandidateReady(8));
    EXPECT_FALSE(runtime->CommitCandidate());
    ASSERT_TRUE(runtime->CandidateReady(9));
    const auto candidate = runtime->CandidateBinding("island");
    ASSERT_TRUE(runtime->PrepareJointSwitch(9));
    EXPECT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, candidate.compositedPageTable);
    EXPECT_EQ(runtime->Token(Root()).revision, 2u);
    EXPECT_TRUE(runtime->RootsCurrent());
}

TEST_F(TerrainCompositedRuntimeCandidateTest, AbortAndLateOldStatusCannotReplaceCurrentOrPublishNewTable)
{
    PublishCurrent();
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    BeginCandidate();
    const auto oldToken = CandidateRequest();
    const auto oldPage = runtime->ReserveProduction(oldToken, 2);
    ASSERT_TRUE(oldPage);
    runtime->CancelCandidate();
    BeginCandidate(3);
    EXPECT_FALSE(runtime->CompleteProduction(*oldPage, 7, true));
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_FALSE(runtime->CandidateReady(7));
    Produce(CandidateRequest(), 7);
    Record(7);
    runtime->Abort();
    EXPECT_FALSE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    Record(7);
    runtime->Commit(8);
    EXPECT_FALSE(runtime->CandidateReady(7));
    ASSERT_TRUE(runtime->CandidateReady(8));
    ASSERT_TRUE(runtime->PrepareJointSwitch(8));
    EXPECT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Token(Root()).revision, 3u);
    EXPECT_FALSE(runtime->CompleteProduction(*oldPage, 9, true));
}

TEST_F(TerrainCompositedRuntimeCandidateTest, RequiredVisibleSelectionSurvivesLaterOldFeedback)
{
    PublishCurrent();
    const TerrainCompositedVTPageKey selected{"island", 0, 1, 1};
    runtime->ConsumeFeedback({runtime->Token(selected)});
    BeginCandidate();
    runtime->ConsumeFeedback({});
    std::vector<TerrainCompositedVTTaskToken> candidateRequests;
    for (const auto& token : runtime->ProductionRequests())
        if (runtime->CandidateToken(token))
            candidateRequests.push_back(token);
    ASSERT_EQ(candidateRequests.size(), 2u);
    for (const auto& token : candidateRequests)
        Produce(token, 3);
    Record(3);
    runtime->Commit(4);
    EXPECT_FALSE(runtime->CandidateReady(3));
    EXPECT_TRUE(runtime->CandidateReady(4));
}

TEST_F(TerrainCompositedRuntimeCandidateTest, RecordedOwnerDictionaryPreservesActualTokensAcrossJointSwitch)
{
    PublishCurrent();
    const auto before = runtime->RecordedOwners();
    auto oldTable = runtime->Binding("island").compositedPageTable;
    BeginCandidate();
    const auto newPage = Produce(CandidateRequest(), 3);
    Record(3);
    auto recording = runtime->RecordedOwners();
    EXPECT_FALSE(recording.at(newPage.slot).valid);
    runtime->Commit(4);
    const auto oldDraw = runtime->RecordedOwners();
    uint32_t oldSlot = 0;
    for (const auto& ownerEntry : before)
        if (ownerEntry.valid)
        {
            oldSlot = ownerEntry.slot;
            ASSERT_TRUE(oldDraw.at(oldSlot).valid);
            EXPECT_TRUE(oldDraw.at(oldSlot).token.Matches(ownerEntry.token));
        }
    ASSERT_TRUE(runtime->CandidateReady(4));
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    EXPECT_TRUE(runtime->CommitCandidate());
    const auto newDraw = runtime->RecordedOwners();
    EXPECT_TRUE(newDraw.at(newPage.slot).valid);
    EXPECT_EQ(newDraw.at(newPage.slot).token.key.surfaceId, "island");
    EXPECT_EQ(newDraw.at(newPage.slot).token.revision, 2u);
    EXPECT_FALSE(newDraw.at(oldSlot).valid);
    EXPECT_TRUE(oldDraw.at(oldSlot).valid);
    EXPECT_EQ(oldDraw.at(oldSlot).token.revision, 1u);
    Record(100);
    runtime->Commit(101);
    EXPECT_NE(backend.images.count(oldTable->getImage()), 0u);
    BeginCandidate(3);
    const auto another = runtime->ReserveProduction(CandidateRequest(), 101);
    ASSERT_TRUE(another);
    EXPECT_NE(another->slot, oldSlot);
    oldTable.reset();
    Record(101);
    runtime->Commit(102);
    EXPECT_TRUE(oldDraw.at(oldSlot).valid);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, AllocationFailureRollsBackCandidateAndPreservesOldBindings)
{
    PublishCurrent();
    auto current = runtime->Binding("island");
    const auto charged = peakOwner->Statistics().current;
    backend.failImageAllocation = true;
    EXPECT_THROW(BeginCandidate(), std::runtime_error);
    backend.failImageAllocation = false;
    EXPECT_EQ(peakOwner->Statistics().current, charged);
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, current.compositedPageTable);
    EXPECT_TRUE(runtime->RootsCurrent());
    EXPECT_FALSE(runtime->CandidateReady(100));
    BeginCandidate();
    EXPECT_TRUE(runtime->CandidateToken(CandidateRequest()));
}

TEST_F(TerrainCompositedRuntimeCandidateTest, MultiSurfaceWorldDirtyCannotSwitchAfterOnlyOneRootPublishes)
{
    auto second = desc;
    second.surfaceId = "neighbor";
    runtime->RegisterPreviewSurface(second, 42, 1);
    Produce(runtime->Token(Root()), 1);
    Produce(runtime->Token(Root("neighbor")), 1);
    Record(1);
    runtime->Commit(2);
    const auto oldFirst = runtime->Binding("island").compositedPageTable;
    const auto oldSecond = runtime->Binding("neighbor").compositedPageTable;
    auto firstChanged = desc;
    firstChanged.dependencyHash.bytes[0] = 1;
    second.dependencyHash.bytes[0] = 2;
    runtime->BeginCandidate({firstChanged, second}, 42, 2, candidateOwner);
    std::vector<TerrainCompositedVTTaskToken> requests;
    for (const auto& token : runtime->ProductionRequests())
        if (runtime->CandidateToken(token))
            requests.push_back(token);
    ASSERT_EQ(requests.size(), 2u);
    Produce(requests.front(), 3);
    Record(3);
    runtime->Commit(4);
    EXPECT_FALSE(runtime->CandidateReady(4));
    EXPECT_FALSE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldFirst);
    EXPECT_EQ(runtime->Binding("neighbor").compositedPageTable, oldSecond);
    Produce(requests.back(), 5);
    Record(5);
    runtime->Commit(6);
    EXPECT_FALSE(runtime->CandidateReady(5));
    ASSERT_TRUE(runtime->CandidateReady(6));
    ASSERT_TRUE(runtime->PrepareJointSwitch(6));
    EXPECT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Token(Root()).revision, 2u);
    EXPECT_EQ(runtime->Token(Root("neighbor")).revision, 2u);
    EXPECT_NE(runtime->Binding("island").compositedPageTable, oldFirst);
    EXPECT_NE(runtime->Binding("neighbor").compositedPageTable, oldSecond);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, SecondSurfaceInvalidPhaseFailsPreflightWithoutRetiringOldPages)
{
    auto second = desc;
    second.surfaceId = "neighbor";
    runtime->RegisterPreviewSurface(second, 42, 1);
    Produce(runtime->Token(Root()), 1);
    Produce(runtime->Token(Root("neighbor")), 1);
    Record(1);
    runtime->Commit(2);
    const auto firstTable = runtime->Binding("island").compositedPageTable;
    const auto secondTable = runtime->Binding("neighbor").compositedPageTable;
    const auto oldOwners = runtime->RecordedOwners();
    runtime->BeginCandidate({desc, second}, 42, 2, candidateOwner);
    for (const auto& request : runtime->ProductionRequests())
        if (runtime->CandidateToken(request))
            Produce(request, 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->CandidateReady(4));
    TerrainCompositedRuntimeTestAccess::RetireSecondCandidatePhase(*runtime);
    EXPECT_THROW(runtime->PrepareJointSwitch(4), std::logic_error);
    EXPECT_FALSE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, firstTable);
    EXPECT_EQ(runtime->Binding("neighbor").compositedPageTable, secondTable);
    EXPECT_TRUE(runtime->RootsCurrent());
    const auto retained = runtime->RecordedOwners();
    for (const auto& oldOwner : oldOwners)
        if (oldOwner.valid)
        {
            EXPECT_TRUE(retained.at(oldOwner.slot).valid);
            EXPECT_TRUE(retained.at(oldOwner.slot).token.Matches(oldOwner.token));
        }
}

TEST_F(TerrainCompositedRuntimeCandidateTest, EveryPreflightAllocationFailurePreservesBothSurfacesAndCommitAllocatesNothing)
{
    auto second = desc;
    second.surfaceId = "neighbor";
    runtime->RegisterPreviewSurface(second, 42, 1);
    Produce(runtime->Token(Root()), 1);
    Produce(runtime->Token(Root("neighbor")), 1);
    Record(1);
    runtime->Commit(2);
    const auto firstTable = runtime->Binding("island").compositedPageTable;
    const auto secondTable = runtime->Binding("neighbor").compositedPageTable;
    runtime->BeginCandidate({desc, second}, 42, 2, candidateOwner);
    for (const auto& request : runtime->ProductionRequests())
        if (runtime->CandidateToken(request))
            Produce(request, 3);
    Record(3);
    runtime->Commit(4);
    bool prepared = false;
    uint32_t failures = 0;
    for (int64_t allocation = 0; allocation < 32 && !prepared; ++allocation)
    {
        TerrainRuntimeAllocationCountdown = allocation;
        try
        {
            prepared = runtime->PrepareJointSwitch(4);
        }
        catch (const std::bad_alloc&)
        {
            ++failures;
        }
        TerrainRuntimeAllocationCountdown = -1;
        EXPECT_EQ(runtime->Binding("island").compositedPageTable, firstTable);
        EXPECT_EQ(runtime->Binding("neighbor").compositedPageTable, secondTable);
        EXPECT_TRUE(runtime->RootsCurrent());
        if (!prepared)
            EXPECT_FALSE(runtime->CommitCandidate());
    }
    ASSERT_TRUE(prepared);
    EXPECT_GE(failures, 2u);
    static_assert(noexcept(runtime->CommitCandidate()));
    TerrainRuntimeAllocationCountdown = 0;
    const bool committed = runtime->CommitCandidate();
    TerrainRuntimeAllocationCountdown = -1;
    EXPECT_TRUE(committed);
    EXPECT_EQ(runtime->Token(Root()).revision, 2u);
    EXPECT_EQ(runtime->Token(Root("neighbor")).revision, 2u);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, CancelAfterPrepareKeepsOldBudgetPhaseActive)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", true),
        TerrainRvtResourcePhase::Candidate);
    runtime->CancelCandidate();
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_TRUE(runtime->RootsCurrent());
    EXPECT_FALSE(runtime->CommitCandidate());
}

TEST_F(TerrainCompositedRuntimeCandidateTest, ContendedJointCommitKeepsOldDrawAndAllInputTablePhasesThenRetries)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    auto oldInput = owner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Active);
    auto newInput = candidateOwner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(oldInput);
    ASSERT_TRUE(newInput);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4, {
        {*oldInput, TerrainRvtResourcePhase::Retiring},
        {*newInput, TerrainRvtResourcePhase::Active}}));
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread blocker([&] {
        TerrainRvtBudgetTestAccess::WithStateLock(*peakOwner, [&] {
            held = true;
            while (!release.load())
                std::this_thread::yield();
        });
    });
    while (!held.load())
        std::this_thread::yield();
    const bool committed = runtime->CommitCandidate();
    release = true;
    blocker.join();
    EXPECT_FALSE(committed);
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_EQ(oldInput->Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_EQ(newInput->Phase(), TerrainRvtResourcePhase::Candidate);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", true),
        TerrainRvtResourcePhase::Candidate);
    Record(4);
    EXPECT_FALSE(runtime->CommitCandidate());
    runtime->Commit(5);
    EXPECT_TRUE(runtime->RootsCurrent());
    ASSERT_TRUE(runtime->PrepareJointSwitch(5, {
        {*oldInput, TerrainRvtResourcePhase::Retiring},
        {*newInput, TerrainRvtResourcePhase::Active}}));
    TerrainRuntimeAllocationCountdown = 0;
    const bool retried = runtime->CommitCandidate();
    TerrainRuntimeAllocationCountdown = -1;
    ASSERT_TRUE(retried);
    EXPECT_NE(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_EQ(oldInput->Phase(), TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(newInput->Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, SecondInputPhaseChangesAfterPrepareCannotPartiallySwitchTables)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    auto oldInput = owner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Active);
    auto newInput = candidateOwner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(oldInput);
    ASSERT_TRUE(newInput);
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    ASSERT_TRUE(runtime->PrepareJointSwitch(4, {
        {*oldInput, TerrainRvtResourcePhase::Retiring},
        {*newInput, TerrainRvtResourcePhase::Active}}));
    newInput->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_FALSE(runtime->CommitCandidate());
    EXPECT_EQ(oldInput->Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", true),
        TerrainRvtResourcePhase::Candidate);
    runtime->CancelCandidate();
    EXPECT_TRUE(runtime->RootsCurrent());
}

TEST_F(TerrainCompositedRuntimeCandidateTest, IndependentReprepareAfterStaleAggregateMustRebuildBatch)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    auto extra = owner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(extra);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    const auto candidateTable = runtime->CandidateBinding("island").compositedPageTable;
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Active}}));
    extra->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_FALSE(runtime->CommitCandidate());
    EXPECT_FALSE(TerrainCompositedRuntimeTestAccess::HasPreparedJointSwitch(*runtime));
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_EQ(runtime->CandidateBinding("island").compositedPageTable, candidateTable);
    EXPECT_TRUE(runtime->CandidateReady(4));
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Retiring}}));
    EXPECT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, candidateTable);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, ExplicitCancelPreparedPreservesReadyCandidateAndIsIdempotentWithoutAllocation)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    const auto candidateTable = runtime->CandidateBinding("island").compositedPageTable;
    const auto before = runtime->Statistics().surfaces.at("island");
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    static_assert(noexcept(runtime->CancelPreparedJointSwitch()));
    TerrainRuntimeAllocationCountdown = 0;
    runtime->CancelPreparedJointSwitch();
    runtime->CancelPreparedJointSwitch();
    TerrainRuntimeAllocationCountdown = -1;
    EXPECT_FALSE(TerrainCompositedRuntimeTestAccess::HasPreparedJointSwitch(*runtime));
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_EQ(runtime->CandidateBinding("island").compositedPageTable, candidateTable);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", true),
        TerrainRvtResourcePhase::Candidate);
    EXPECT_EQ(runtime->Statistics().surfaces.at("island").resident, before.resident);
    EXPECT_TRUE(runtime->CandidateReady(4));
    EXPECT_FALSE(runtime->CommitCandidate());
    ASSERT_TRUE(runtime->PrepareJointSwitch(4));
    EXPECT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, candidateTable);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, RepeatedPrepareReplacesAdditionalChangesBeforeAnyCommit)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    auto extra = owner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(extra);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Active}}));
    extra->SetPhase(TerrainRvtResourcePhase::Retiring);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Retiring}}));
    EXPECT_TRUE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Token(Root()).revision, 2u);
}

TEST_F(TerrainCompositedRuntimeCandidateTest, ReprepareExceptionDropsOldMetadataAndLeavesCandidateAvailableForRetry)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    auto extra = owner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(extra);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    const auto oldTable = runtime->Binding("island").compositedPageTable;
    const auto candidateTable = runtime->CandidateBinding("island").compositedPageTable;
    const auto before = candidateOwner->Statistics().current;
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Active}}));
    EXPECT_GT(candidateOwner->Statistics().current.cpuBytes, before.cpuBytes);
    EXPECT_THROW(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Count}}), std::invalid_argument);
    EXPECT_FALSE(TerrainCompositedRuntimeTestAccess::HasPreparedJointSwitch(*runtime));
    EXPECT_EQ(candidateOwner->Statistics().current, before);
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    EXPECT_EQ(runtime->CandidateBinding("island").compositedPageTable, candidateTable);
    EXPECT_EQ(extra->Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", false),
        TerrainRvtResourcePhase::Active);
    EXPECT_EQ(TerrainCompositedRuntimeTestAccess::TablePhase(*runtime, "island", true),
        TerrainRvtResourcePhase::Candidate);
    EXPECT_TRUE(runtime->CandidateReady(4));
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Active}}));
    EXPECT_TRUE(runtime->CommitCandidate());
}
}
}
