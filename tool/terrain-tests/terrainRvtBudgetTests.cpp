#include <gtest/gtest.h>

#include "terrain/terrainRvtBudget.h"

#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace VL
{
namespace
{
TerrainRvtBudgetCaps BudgetCaps()
{
    return {600, 500, 200, 1000, 16384, 20, 4, 16384, 16384, 16384};
}

TEST(TerrainRvtBudgetTest, SharedLeaseChargesActualAllocationOnceUntilFinalReference)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto first = owner.TryReserve({400, 100, 16, 0}, TerrainRvtCostClass::PhysicalPool,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(first);
    auto retained = first;
    first.reset();
    EXPECT_EQ(owner.Statistics().current.gpuBytes, 400u);
    EXPECT_EQ(owner.Statistics().allocations, 1u);
    retained.reset();
    EXPECT_EQ(owner.Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(owner.Statistics().peak.gpuBytes, 400u);
}

TEST(TerrainRvtBudgetTest, WorldPhaseTransitionsIncludeRetainedAliasesAndFutureAllocations)
{
    auto peak = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner world(BudgetCaps(), peak);
    auto lease = world.TryReserve({100, 10, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    auto alias = *lease;
    world.SetPhase(TerrainRvtResourcePhase::Active);
    EXPECT_EQ(alias.Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_EQ(peak->Statistics().byPhase[1].gpuBytes, 100u);
    auto later = world.TryReserve({50, 10, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(later);
    EXPECT_EQ(later->Phase(), TerrainRvtResourcePhase::Active);
    world.SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(peak->Statistics().byPhase[2].gpuBytes, 150u);
    lease.reset();
    later.reset();
    EXPECT_EQ(peak->Statistics().byPhase[2].gpuBytes, 100u);
}

TEST(TerrainRvtBudgetTest, ParentRejectionRollsBackAllEarlierReservations)
{
    auto peak = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    auto outerCaps = BudgetCaps();
    outerCaps.totalGpuBytes = 200;
    auto outer = std::make_shared<TerrainRvtBudgetOwner>(outerCaps);
    TerrainRvtBudgetOwner world(BudgetCaps(), peak, outer);
    EXPECT_FALSE(world.TryReserve({201, 10, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_EQ(world.Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(peak->Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(outer->Statistics().current, TerrainRvtCost{});
    auto lease = world.TryReserve({200, 10, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    lease->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(peak->Statistics().byPhase[2].gpuBytes, 200u);
    EXPECT_EQ(outer->Statistics().byPhase[2].gpuBytes, 200u);
    auto retained = lease;
    lease.reset();
    EXPECT_EQ(peak->Statistics().current.gpuBytes, 200u);
    retained.reset();
    EXPECT_EQ(world.Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(peak->Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(outer->Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, WorldsSharePeakWhileGeometrySharesOnlyOuterTerrainCap)
{
    auto peakCaps = BudgetCaps();
    peakCaps.inputGpuBytes = 1000;
    auto peak = std::make_shared<TerrainRvtBudgetOwner>(peakCaps);
    auto outer = std::make_shared<TerrainRvtBudgetOwner>(peakCaps);
    auto terrainWorld = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps(), outer);
    TerrainRvtBudgetOwner rvt(BudgetCaps(), peak, terrainWorld);
    TerrainRvtBudgetOwner geometry(BudgetCaps(), terrainWorld);
    auto input = rvt.TryReserve({450, 10, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Active);
    auto mesh = geometry.TryReserve({180, 10, 0, 0}, TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(input);
    ASSERT_TRUE(mesh);
    TerrainRvtBudgetOwner candidate(BudgetCaps(), peak, outer);
    EXPECT_FALSE(candidate.TryReserve({400, 10, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_EQ(peak->Statistics().current.gpuBytes, 450u);
    EXPECT_EQ(outer->Statistics().current.gpuBytes, 630u);
}

TEST(TerrainRvtBudgetTest, ActiveCandidateAndRetiringInputsShareOneAdmissionLimit)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto active = owner.TryReserve({300, 100, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(active);
    EXPECT_FALSE(owner.TryReserve({201, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    active->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_FALSE(owner.TryReserve({201, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    auto candidate = owner.TryReserve({200, 50, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(candidate);
    candidate->SetPhase(TerrainRvtResourcePhase::Active);
    EXPECT_EQ(owner.Statistics().byPhase[2].gpuBytes, 300u);
    EXPECT_EQ(owner.Statistics().byPhase[1].gpuBytes, 200u);
    active.reset();
    EXPECT_EQ(owner.Statistics().current.gpuBytes, 200u);
}

TEST(TerrainRvtBudgetTest, FailedCandidateReleasesOnlyItsOwnReservation)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto active = owner.TryReserve({300, 100, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(active);
    {
        auto candidate = owner.TryReserve({200, 50, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Candidate);
        ASSERT_TRUE(candidate);
        EXPECT_FALSE(owner.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Candidate));
    }
    EXPECT_EQ(owner.Statistics().current,
        (TerrainRvtCost{300, 100 + TerrainRvtBudgetOwner::MetadataBytesPerLease, 0, 0}));
}

TEST(TerrainRvtBudgetTest, TotalBudgetIncludesPoolInputsAndProductionWorkspace)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto pool = owner.TryReserve({600, 0, 16, 0}, TerrainRvtCostClass::PhysicalPool,
        TerrainRvtResourcePhase::Active);
    auto inputs = owner.TryReserve({350, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(pool);
    ASSERT_TRUE(inputs);
    EXPECT_FALSE(owner.TryReserve({51, 0, 0, 1}, TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_EQ(owner.Statistics().current.gpuBytes, 950u);
}

TEST(TerrainRvtBudgetTest, CpuPhysicalSlotsAndProductionTasksHaveIndependentCaps)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto ring = owner.TryReserve(
        {0, owner.Caps().totalCpuBytes - TerrainRvtBudgetOwner::MetadataBytesPerLease, 20, 4},
        TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(ring);
    EXPECT_FALSE(owner.TryReserve({0, 1, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_FALSE(owner.TryReserve({0, 0, 1, 0}, TerrainRvtCostClass::PhysicalPool,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_FALSE(owner.TryReserve({0, 0, 0, 1}, TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Candidate));
}

TEST(TerrainRvtBudgetTest, RetiringPoolStillChargesAllRootReservedAndRetiringCapacity)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto pool = owner.TryReserve({500, 0, 20, 0}, TerrainRvtCostClass::PhysicalPool,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(pool);
    pool->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_FALSE(owner.TryReserve({1, 0, 1, 0}, TerrainRvtCostClass::PhysicalPool,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_EQ(owner.Statistics().current.physicalSlots, 20u);
}

TEST(TerrainRvtBudgetTest, ReservationArithmeticDoesNotWrap)
{
    auto caps = BudgetCaps();
    caps.totalGpuBytes = caps.inputGpuBytes = std::numeric_limits<uint64_t>::max();
    TerrainRvtBudgetOwner owner(caps);
    auto first = owner.TryReserve({std::numeric_limits<uint64_t>::max() - 1, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(first);
    EXPECT_FALSE(owner.TryReserve({2, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
}

TEST(TerrainRvtBudgetTest, ZeroCapsDisableAllocationWithoutImplicitDefaults)
{
    TerrainRvtBudgetOwner owner({0, 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(owner.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
}

TEST(TerrainRvtBudgetTest, SharedLeasePhaseChangesAreVisibleAndCannotRegress)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    auto first = owner.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(first);
    auto retained = *first;
    first->SetPhase(TerrainRvtResourcePhase::Active);
    EXPECT_EQ(retained.Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_THROW(retained.SetPhase(TerrainRvtResourcePhase::Candidate), std::logic_error);
    EXPECT_EQ(owner.Statistics().byPhase[1].gpuBytes, 100u);
}

TEST(TerrainRvtBudgetTest, LeaseCanOutliveBudgetFacade)
{
    std::optional<TerrainRvtBudgetLease> retained;
    {
        TerrainRvtBudgetOwner owner(BudgetCaps());
        retained = owner.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Active);
    }
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->Cost().gpuBytes, 100u);
    retained->SetPhase(TerrainRvtResourcePhase::Retiring);
    retained.reset();
}

TEST(TerrainRvtBudgetTest, ConcurrentWorldAdmissionCannotExceedTotalCap)
{
    TerrainRvtBudgetOwner owner({100, 100, 100, 100, 1124, 4, 4, 1124, 1124, 1124});
    std::atomic<uint32_t> arrived{0};
    std::atomic<uint32_t> admitted{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> workers;
    for (uint32_t index = 0; index < 8; ++index)
        workers.emplace_back([&] {
            auto lease = owner.TryReserve({25, 25, 1, 1}, TerrainRvtCostClass::Input,
                TerrainRvtResourcePhase::Candidate);
            if (lease)
                ++admitted;
            ++arrived;
            while (!release.load())
                std::this_thread::yield();
        });
    while (arrived.load() != 8)
        std::this_thread::yield();
    const auto stats = owner.Statistics();
    release = true;
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(admitted.load(), 4u);
    EXPECT_EQ(stats.current.gpuBytes, 100u);
    EXPECT_EQ(owner.Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, TenThousandShortLeasesLeaveNoChildOrParentMetadata)
{
    const TerrainRvtBudgetCaps caps{600, 500, 200, 1000, 800, 20, 4, 800, 800, 800};
    auto peak = std::make_shared<TerrainRvtBudgetOwner>(caps);
    TerrainRvtBudgetOwner world(caps, peak);
    world.SetPhase(TerrainRvtResourcePhase::Active);
    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        auto lease = world.TryReserve({0, 0, 0, 1}, TerrainRvtCostClass::Workspace,
            TerrainRvtResourcePhase::Active);
        ASSERT_TRUE(lease) << iteration;
        EXPECT_EQ(world.Statistics().metadataBytes,
            TerrainRvtBudgetOwner::MetadataBytesPerLease);
        EXPECT_EQ(peak->Statistics().current.cpuBytes,
            2 * TerrainRvtBudgetOwner::MetadataBytesPerLease);
    }
    for (const auto* owner : {&world, peak.get()})
    {
        const auto stats = owner->Statistics();
        EXPECT_EQ(stats.allocations, 0u);
        EXPECT_EQ(stats.current, TerrainRvtCost{});
        EXPECT_EQ(stats.metadataBytes, 0u);
        EXPECT_EQ(stats.peakMetadataBytes, TerrainRvtBudgetOwner::MetadataBytesPerLease);
    }
}

TEST(TerrainRvtBudgetTest, MetadataAdmissionUsesTotalClassAndParentCpuCaps)
{
    auto caps = BudgetCaps();
    caps.totalCpuBytes = 2 * TerrainRvtBudgetOwner::MetadataBytesPerLease - 1;
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(caps);
    TerrainRvtBudgetOwner child(BudgetCaps(), parent);
    EXPECT_FALSE(child.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_EQ(child.Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(parent->Statistics().metadataBytes, 0u);
    caps.totalCpuBytes = 16384;
    caps.inputCpuBytes = TerrainRvtBudgetOwner::MetadataBytesPerLease - 1;
    TerrainRvtBudgetOwner category(caps);
    EXPECT_FALSE(category.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Active));
    caps.inputCpuBytes = caps.totalCpuBytes = TerrainRvtBudgetOwner::MetadataBytesPerLease;
    TerrainRvtBudgetOwner exact(caps);
    auto lease = exact.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->Cost(), (TerrainRvtCost{1, 0, 0, 0}));
    EXPECT_FALSE(exact.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    lease->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(exact.Statistics().byPhase[2].cpuBytes,
        TerrainRvtBudgetOwner::MetadataBytesPerLease);
}

TEST(TerrainRvtBudgetTest, ActiveRegistryHasHardEntryLimitAndReusesReleasedEntries)
{
    auto caps = BudgetCaps();
    caps.totalGpuBytes = caps.inputGpuBytes = 100000;
    caps.totalCpuBytes = caps.inputCpuBytes = 10000000;
    auto peak = std::make_shared<TerrainRvtBudgetOwner>(caps);
    TerrainRvtBudgetOwner world(caps, peak);
    std::vector<TerrainRvtBudgetLease> leases;
    for (uint64_t index = 0; index < TerrainRvtBudgetOwner::MaxRegistryEntries; ++index)
    {
        auto lease = world.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Active);
        ASSERT_TRUE(lease) << index;
        leases.push_back(*lease);
    }
    for (int attempt = 0; attempt < 10000; ++attempt)
        EXPECT_FALSE(world.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Active));
    EXPECT_EQ(world.Statistics().allocations, TerrainRvtBudgetOwner::MaxRegistryEntries);
    EXPECT_EQ(peak->Statistics().allocations, TerrainRvtBudgetOwner::MaxRegistryEntries);
    EXPECT_EQ(world.Statistics().metadataBytes, TerrainRvtBudgetOwner::MaxRegistryEntries *
        TerrainRvtBudgetOwner::MetadataBytesPerLease);
    TerrainRvtBudgetOwner contender(caps, peak);
    EXPECT_FALSE(contender.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    EXPECT_EQ(contender.Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(contender.Statistics().metadataBytes, 0u);
    leases.erase(leases.begin() + leases.size() / 2);
    auto replacement = world.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(replacement);
    world.SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(peak->Statistics().byPhase[2].gpuBytes,
        TerrainRvtBudgetOwner::MaxRegistryEntries);
    leases.clear();
    replacement.reset();
    EXPECT_EQ(world.Statistics().metadataBytes, 0u);
    EXPECT_EQ(peak->Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, RepeatedSecondParentRejectionLeavesNoRegistryOrCpuCharge)
{
    auto first = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    auto rejectingCaps = BudgetCaps();
    rejectingCaps.totalCpuBytes = TerrainRvtBudgetOwner::MetadataBytesPerLease;
    auto second = std::make_shared<TerrainRvtBudgetOwner>(rejectingCaps);
    TerrainRvtBudgetOwner child(BudgetCaps(), first, second);
    for (int iteration = 0; iteration < 10000; ++iteration)
        ASSERT_FALSE(child.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Candidate));
    for (const auto* owner : {&child, first.get(), second.get()})
    {
        EXPECT_EQ(owner->Statistics().allocations, 0u);
        EXPECT_EQ(owner->Statistics().metadataBytes, 0u);
        EXPECT_EQ(owner->Statistics().current, TerrainRvtCost{});
    }
}

TEST(TerrainRvtBudgetTest, ConcurrentPhaseReserveAndFinalAliasRelease)
{
    auto caps = BudgetCaps();
    caps.totalGpuBytes = caps.inputGpuBytes = 100000;
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(caps);
    TerrainRvtBudgetOwner world(caps, parent);
    std::atomic<bool> start{false};
    std::atomic<uint32_t> admitted{0};
    std::vector<std::thread> workers;
    for (int workerIndex = 0; workerIndex < 4; ++workerIndex)
        workers.emplace_back([&] {
            while (!start.load())
                std::this_thread::yield();
            for (int iteration = 0; iteration < 10000; ++iteration)
            {
                auto lease = world.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
                    TerrainRvtResourcePhase::Candidate);
                if (lease)
                {
                    ++admitted;
                    auto alias = lease;
                    lease.reset();
                    alias->SetPhase(TerrainRvtResourcePhase::Retiring);
                }
            }
        });
    for (int workerIndex = 0; workerIndex < 2; ++workerIndex)
        workers.emplace_back([&, workerIndex] {
            while (!start.load())
                std::this_thread::yield();
            for (int iteration = 0; iteration < 10000; ++iteration)
            {
                world.SetPhase(workerIndex == 0 ? TerrainRvtResourcePhase::Active :
                    TerrainRvtResourcePhase::Retiring);
                parent->SetPhase(TerrainRvtResourcePhase::Retiring);
            }
        });
    start = true;
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(admitted.load(), 40000u);
    auto later = world.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(later);
    EXPECT_EQ(later->Phase(), TerrainRvtResourcePhase::Retiring);
    later.reset();
    EXPECT_EQ(world.Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(parent->Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(world.Statistics().metadataBytes, 0u);
    EXPECT_EQ(parent->Statistics().metadataBytes, 0u);
}

TEST(TerrainRvtBudgetTest, ParentMetadataSurvivesFacadeUntilLastAlias)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    std::optional<TerrainRvtBudgetLease> retained;
    {
        TerrainRvtBudgetOwner child(BudgetCaps(), parent);
        auto lease = child.TryReserve({1, 7, 0, 0}, TerrainRvtCostClass::Input,
            TerrainRvtResourcePhase::Candidate);
        ASSERT_TRUE(lease);
        retained = lease;
    }
    ASSERT_TRUE(retained);
    EXPECT_EQ(parent->Statistics().current.cpuBytes,
        7 + 2 * TerrainRvtBudgetOwner::MetadataBytesPerLease);
    retained->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(parent->Statistics().byPhase[2].gpuBytes, 1u);
    auto alias = retained;
    retained.reset();
    EXPECT_EQ(parent->Statistics().allocations, 1u);
    alias.reset();
    EXPECT_EQ(parent->Statistics().metadataBytes, 0u);
    EXPECT_EQ(parent->Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, CpuMetadataArithmeticDoesNotWrap)
{
    auto caps = BudgetCaps();
    caps.totalCpuBytes = caps.inputCpuBytes = std::numeric_limits<uint64_t>::max();
    TerrainRvtBudgetOwner owner(caps);
    EXPECT_FALSE(owner.TryReserve({0, std::numeric_limits<uint64_t>::max(), 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate));
    auto lease = owner.TryReserve(
        {0, caps.totalCpuBytes - TerrainRvtBudgetOwner::MetadataBytesPerLease, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    EXPECT_EQ(owner.Statistics().current.cpuBytes, caps.totalCpuBytes);
    EXPECT_FALSE(owner.TryReserve({1, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate));
    lease.reset();
    EXPECT_EQ(owner.Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, IdentityTokensShareStateWithoutChargingOrChangingPhase)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    const auto before = owner.Statistics();
    auto first = owner.IdentityToken();
    auto second = owner.IdentityToken();
    const std::owner_less<std::shared_ptr<const void>> less;
    EXPECT_FALSE(less(first, second));
    EXPECT_FALSE(less(second, first));
    const std::shared_ptr<const void> alias(first, reinterpret_cast<const void*>(uintptr_t(1)));
    EXPECT_NE(alias.get(), first.get());
    EXPECT_FALSE(less(alias, second));
    EXPECT_FALSE(less(second, alias));
    EXPECT_EQ(owner.Statistics().current, before.current);
    EXPECT_EQ(owner.Statistics().allocations, before.allocations);
    EXPECT_EQ(owner.Statistics().byPhase, before.byPhase);
}

TEST(TerrainRvtBudgetTest, IdentityOutlivesFacadeAndSameAddressReplacementHasDifferentOwner)
{
    alignas(TerrainRvtBudgetOwner) unsigned char storage[sizeof(TerrainRvtBudgetOwner)];
    auto* first = new (storage) TerrainRvtBudgetOwner(BudgetCaps());
    auto oldToken = first->IdentityToken();
    std::weak_ptr<const void> oldState = oldToken;
    first->~TerrainRvtBudgetOwner();
    EXPECT_FALSE(oldState.expired());
    auto* replacement = new (storage) TerrainRvtBudgetOwner(BudgetCaps());
    auto newToken = replacement->IdentityToken();
    EXPECT_EQ(first, replacement);
    const std::owner_less<std::shared_ptr<const void>> less;
    EXPECT_TRUE(less(oldToken, newToken) || less(newToken, oldToken));
    EXPECT_NE(oldToken.get(), newToken.get());
    replacement->~TerrainRvtBudgetOwner();
    EXPECT_FALSE(oldState.expired());
    oldToken.reset();
    EXPECT_TRUE(oldState.expired());
}

TEST(TerrainRvtBudgetTest, PhaseBatchCancelLeavesChildBothParentsAndAliasesUnchanged)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    auto second = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner child(BudgetCaps(), parent, second);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = child.TryReserve({100, 1, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    auto alias = *lease;
    const auto childBefore = child.Statistics();
    const auto parentBefore = parent->Statistics();
    const auto secondBefore = second->Statistics();
    {
        auto batch = TerrainRvtBudgetPhaseBatch::Prepare(
            {{*lease, TerrainRvtResourcePhase::Active}}, metadata);
        ASSERT_TRUE(batch);
        EXPECT_EQ(batch->ChargeCount(), 3u);
        EXPECT_GT(batch->MetadataBytes(), 0u);
        EXPECT_GT(metadata.Statistics().current.cpuBytes, batch->MetadataBytes());
        EXPECT_EQ(alias.Phase(), TerrainRvtResourcePhase::Candidate);
        EXPECT_EQ(child.Statistics().byPhase, childBefore.byPhase);
        EXPECT_EQ(parent->Statistics().byPhase, parentBefore.byPhase);
        EXPECT_EQ(second->Statistics().byPhase, secondBefore.byPhase);
    }
    EXPECT_EQ(child.Statistics().byPhase, childBefore.byPhase);
    EXPECT_EQ(parent->Statistics().byPhase, parentBefore.byPhase);
    EXPECT_EQ(second->Statistics().byPhase, secondBefore.byPhase);
    EXPECT_EQ(metadata.Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, PhaseBatchDeduplicatesAliasesAndMergesTargetsThroughBothParentBranches)
{
    auto grandparent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps(), grandparent);
    auto second = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps(), grandparent);
    TerrainRvtBudgetOwner child(BudgetCaps(), parent, second);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = child.TryReserve({100, 1, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    const auto childCost = child.Statistics().current;
    const auto parentCost = parent->Statistics().current;
    const auto secondCost = second->Statistics().current;
    const auto grandparentCost = grandparent->Statistics().current;
    auto batch = TerrainRvtBudgetPhaseBatch::Prepare({
        {*lease, TerrainRvtResourcePhase::Candidate},
        {*lease, TerrainRvtResourcePhase::Retiring},
        {*lease, TerrainRvtResourcePhase::Active}}, metadata);
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->ChargeCount(), 5u);
    ASSERT_TRUE(batch->TryCommitNoThrow());
    EXPECT_EQ(lease->Phase(), TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(child.Statistics().byPhase[2], childCost);
    EXPECT_EQ(parent->Statistics().byPhase[2], parentCost);
    EXPECT_EQ(second->Statistics().byPhase[2], secondCost);
    EXPECT_EQ(grandparent->Statistics().byPhase[2], grandparentCost);
    EXPECT_EQ(grandparentCost.gpuBytes, 200u);
    EXPECT_FALSE(batch->TryCommitNoThrow());
}

TEST(TerrainRvtBudgetTest, PhaseBatchBadSecondTargetAndMovedLeaseCannotPartiallyPrepare)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto first = owner.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    auto second = owner.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Retiring);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto before = owner.Statistics();
    EXPECT_THROW(TerrainRvtBudgetPhaseBatch::Prepare({
        {*first, TerrainRvtResourcePhase::Active},
        {*second, TerrainRvtResourcePhase::Active}}, metadata), std::logic_error);
    EXPECT_THROW(TerrainRvtBudgetPhaseBatch::Prepare({
        {*first, TerrainRvtResourcePhase::Active},
        {*second, TerrainRvtResourcePhase::Count}}, metadata), std::invalid_argument);
    auto retained = std::move(*second);
    EXPECT_THROW(TerrainRvtBudgetPhaseBatch::Prepare({
        {*first, TerrainRvtResourcePhase::Active},
        {*second, TerrainRvtResourcePhase::Retiring}}, metadata), std::invalid_argument);
    EXPECT_EQ(owner.Statistics().byPhase, before.byPhase);
    EXPECT_EQ(metadata.Statistics().current, TerrainRvtCost{});
}

TEST(TerrainRvtBudgetTest, PhaseBatchRevalidatesSecondParentAtCommitWithZeroOtherChanges)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    auto second = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner child(BudgetCaps(), parent, second);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto first = child.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    auto later = child.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(first);
    ASSERT_TRUE(later);
    auto batch = TerrainRvtBudgetPhaseBatch::Prepare({
        {*first, TerrainRvtResourcePhase::Active},
        {*later, TerrainRvtResourcePhase::Active}}, metadata);
    ASSERT_TRUE(batch);
    second->SetPhase(TerrainRvtResourcePhase::Retiring);
    const auto childBefore = child.Statistics();
    const auto parentBefore = parent->Statistics();
    const auto secondBefore = second->Statistics();
    EXPECT_FALSE(batch->TryCommitNoThrow());
    EXPECT_EQ(first->Phase(), TerrainRvtResourcePhase::Candidate);
    EXPECT_EQ(later->Phase(), TerrainRvtResourcePhase::Candidate);
    EXPECT_EQ(child.Statistics().byPhase, childBefore.byPhase);
    EXPECT_EQ(parent->Statistics().byPhase, parentBefore.byPhase);
    EXPECT_EQ(second->Statistics().byPhase, secondBefore.byPhase);
}

TEST(TerrainRvtBudgetTest, PhaseBatchContendedParentReturnsImmediatelyAndCanRetryWithoutPartialEffects)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner child(BudgetCaps(), parent);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = child.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    auto batch = TerrainRvtBudgetPhaseBatch::Prepare(
        {{*lease, TerrainRvtResourcePhase::Active}}, metadata);
    ASSERT_TRUE(batch);
    const auto before = child.Statistics();
    const auto parentBefore = parent->Statistics();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread blocker([&] {
        TerrainRvtBudgetTestAccess::WithStateLock(*parent, [&] {
            held = true;
            while (!release.load())
                std::this_thread::yield();
        });
    });
    while (!held.load())
        std::this_thread::yield();
    const bool committed = batch->TryCommitNoThrow();
    release = true;
    blocker.join();
    EXPECT_FALSE(committed);
    EXPECT_EQ(child.Statistics().byPhase, before.byPhase);
    EXPECT_EQ(parent->Statistics().byPhase, parentBefore.byPhase);
    ASSERT_TRUE(batch->TryCommitNoThrow());
    EXPECT_EQ(child.Statistics().byPhase[1].gpuBytes, 100u);
    EXPECT_EQ(parent->Statistics().byPhase[1].gpuBytes, 100u);
}

TEST(TerrainRvtBudgetTest, PhaseBatchConcurrentAttemptsOnOneTicketCommitExactlyOnce)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner child(BudgetCaps(), parent);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = child.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    auto batch = TerrainRvtBudgetPhaseBatch::Prepare(
        {{*lease, TerrainRvtResourcePhase::Active}}, metadata);
    ASSERT_TRUE(batch);
    std::atomic<uint32_t> successes{0};
    std::vector<std::thread> workers;
    for (uint32_t index = 0; index < 8; ++index)
        workers.emplace_back([&] {
            for (uint32_t attempt = 0; attempt < 100; ++attempt)
                if (batch->TryCommitNoThrow())
                    ++successes;
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(successes.load(), 1u);
    EXPECT_EQ(lease->Phase(), TerrainRvtResourcePhase::Active);
    EXPECT_EQ(parent->Statistics().byPhase[1].gpuBytes, 100u);
}

TEST(TerrainRvtBudgetTest, PhaseBatchLockExceptionReleasesEarlierLocksWithoutChangingAnyPhase)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    auto second = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner child(BudgetCaps(), parent, second);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = child.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    auto batch = TerrainRvtBudgetPhaseBatch::Prepare(
        {{*lease, TerrainRvtResourcePhase::Active}}, metadata);
    ASSERT_TRUE(batch);
    const auto before = child.Statistics();
    const auto parentBefore = parent->Statistics();
    const auto secondBefore = second->Statistics();
    TerrainRvtBudgetTestAccess::FailTryLockAfter(1);
    EXPECT_FALSE(batch->TryCommitNoThrow());
    EXPECT_EQ(child.Statistics().byPhase, before.byPhase);
    EXPECT_EQ(parent->Statistics().byPhase, parentBefore.byPhase);
    EXPECT_EQ(second->Statistics().byPhase, secondBefore.byPhase);
    ASSERT_TRUE(batch->TryCommitNoThrow());
    EXPECT_EQ(child.Statistics().byPhase[1].gpuBytes, 100u);
    EXPECT_EQ(parent->Statistics().byPhase[1].gpuBytes, 100u);
    EXPECT_EQ(second->Statistics().byPhase[1].gpuBytes, 100u);
}

TEST(TerrainRvtBudgetTest, PhaseBatchConcurrentTicketsPreserveMonotonicParentAndChildPhases)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(BudgetCaps());
    TerrainRvtBudgetOwner child(BudgetCaps(), parent);
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = child.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    auto active = TerrainRvtBudgetPhaseBatch::Prepare(
        {{*lease, TerrainRvtResourcePhase::Active}}, metadata);
    auto retiring = TerrainRvtBudgetPhaseBatch::Prepare(
        {{*lease, TerrainRvtResourcePhase::Retiring}}, metadata);
    ASSERT_TRUE(active);
    ASSERT_TRUE(retiring);
    std::atomic<bool> start{false};
    std::thread first([&] {
        while (!start.load())
            std::this_thread::yield();
        for (uint32_t attempt = 0; attempt < 100; ++attempt)
            active->TryCommitNoThrow();
    });
    start = true;
    bool retired = false;
    while (!retired)
    {
        retired = retiring->TryCommitNoThrow();
        std::this_thread::yield();
    }
    first.join();
    EXPECT_EQ(lease->Phase(), TerrainRvtResourcePhase::Retiring);
    EXPECT_EQ(child.Statistics().byPhase[2].gpuBytes, 100u);
    EXPECT_EQ(parent->Statistics().byPhase[2].gpuBytes, 100u);
    EXPECT_FALSE(active->TryCommitNoThrow());
}

TEST(TerrainRvtBudgetTest, PhaseBatchMetadataIsAdmittedBoundedAndReleasedOnCancellation)
{
    TerrainRvtBudgetOwner owner(BudgetCaps());
    TerrainRvtBudgetOwner metadata(BudgetCaps());
    auto lease = owner.TryReserve({100, 0, 0, 0}, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    std::vector<TerrainRvtBudgetPhaseBatch> retained;
    bool rejected = false;
    for (uint32_t index = 0; index < 100; ++index)
    {
        auto batch = TerrainRvtBudgetPhaseBatch::Prepare(
            {{*lease, TerrainRvtResourcePhase::Active}}, metadata);
        if (!batch)
        {
            rejected = true;
            break;
        }
        retained.push_back(std::move(*batch));
    }
    ASSERT_TRUE(rejected);
    EXPECT_LE(metadata.Statistics().current.cpuBytes, metadata.Caps().totalCpuBytes);
    EXPECT_EQ(lease->Phase(), TerrainRvtResourcePhase::Candidate);
    retained.clear();
    EXPECT_EQ(metadata.Statistics().current, TerrainRvtCost{});
    std::vector<TerrainRvtBudgetPhaseChange> excessive(
        TerrainRvtBudgetPhaseBatch::MaxCharges + 1, {*lease, TerrainRvtResourcePhase::Active});
    EXPECT_THROW(TerrainRvtBudgetPhaseBatch::Prepare(excessive, metadata), std::invalid_argument);
}
}
}
