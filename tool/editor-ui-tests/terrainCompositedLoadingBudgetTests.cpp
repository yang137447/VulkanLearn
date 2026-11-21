#include <gtest/gtest.h>

#include "render/frame/terrainCompositedLoading.h"

namespace VL
{
namespace
{
TEST(TerrainCompositedLoadingBudget, QuoteIncludesAllEightEntriesAndCacheStorage)
{
    const auto cost = TerrainCompositedLoading::ReservationCost({16384, 4096, 0, 0});
    EXPECT_EQ(cost.gpuBytes, 131072u);
    EXPECT_EQ(cost.cpuBytes, 32768u + sizeof(TerrainCompositedLoading));
    EXPECT_EQ(cost.physicalSlots, 0u);
    EXPECT_EQ(cost.productionTasks, 0u);
}

TEST(TerrainCompositedLoadingBudget, MissingOrOverflowingMetadataCannotBypassAdmission)
{
    EXPECT_THROW(TerrainCompositedLoading::ReservationCost({0, 4096, 0, 0}),
        std::invalid_argument);
    EXPECT_THROW(TerrainCompositedLoading::ReservationCost({16384, 0, 0, 0}),
        std::invalid_argument);
    EXPECT_THROW(TerrainCompositedLoading::ReservationCost({16384, 4096, 1, 0}),
        std::invalid_argument);
    EXPECT_THROW(TerrainCompositedLoading::ReservationCost(
        {std::numeric_limits<uint64_t>::max(), 4096, 0, 0}), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedLoading::ReservationCost(
        {16384, std::numeric_limits<uint64_t>::max(), 0, 0}), std::invalid_argument);
}

TEST(TerrainCompositedLoadingBudget, LoadingCannotExceedFrozenWorkspaceCap)
{
    TerrainRvtBudgetCaps caps;
    caps.workspaceGpuBytes = 32 * 1024 * 1024;
    caps.totalGpuBytes = caps.workspaceGpuBytes;
    caps.totalCpuBytes = caps.workspaceCpuBytes = 1024 * 1024;
    TerrainRvtBudgetOwner budget(caps);
    const auto quote = TerrainCompositedLoading::ReservationCost({16384, 4096, 0, 0});
    auto existing = budget.TryReserve({caps.workspaceGpuBytes - quote.gpuBytes + 1, 0, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(existing);
    EXPECT_FALSE(budget.TryReserve(quote, TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Active));
    existing.reset();
    EXPECT_TRUE(budget.TryReserve(quote, TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Active));
}

TEST(TerrainCompositedLoadingBudget, PendingFrameLeaseKeepsParentChargeUntilLastOwner)
{
    TerrainRvtBudgetCaps caps;
    caps.workspaceGpuBytes = caps.totalGpuBytes = 32 * 1024 * 1024;
    caps.workspaceCpuBytes = caps.totalCpuBytes = 1024 * 1024;
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(caps);
    TerrainRvtBudgetOwner world(caps, parent);
    const auto quote = TerrainCompositedLoading::ReservationCost({16384, 4096, 0, 0});
    auto cacheLease = world.TryReserve(quote, TerrainRvtCostClass::Workspace,
        TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(cacheLease);
    auto frameLease = cacheLease;
    cacheLease.reset();
    EXPECT_EQ(parent->Statistics().current.gpuBytes, quote.gpuBytes);
    EXPECT_EQ(parent->Statistics().current.cpuBytes, quote.cpuBytes);
    frameLease.reset();
    EXPECT_EQ(parent->Statistics().current.gpuBytes, 0u);
    EXPECT_EQ(parent->Statistics().current.cpuBytes, 0u);
}
}
}
