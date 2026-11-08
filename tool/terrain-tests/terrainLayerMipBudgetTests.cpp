// TerrainLayerMipBudget 的纯逻辑测试。
//
// 这个分配器存在的理由是架构约束而不是性能调优：契约要求“选定 mip 后必须
// 请求该 mip 的全部页”，所以每个 (layer, mip) 页集是固定成本；多 tile 场景
// 下若按屏幕像素独立选 mip，总页数会超过 ResidentCapacity，导致每帧
// “请求 -> 淘汰 -> 再请求”永不收敛。这里锁定的性质有：
//   1. 去重页集总页数永不超过预算（收敛性，最重要的不变量）；
//   2. 预算充裕时按优先级（近处）先拿到细 mip；
//   3. 预算紧张时近处需求不会比远处需求更粗；
//   4. 同一层的近处/远处目标可以同时保留不同 mip；
//   5. 分配不会越过 feedback 目标去要 shader 根本不采样的更细页。

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <vector>

#include "terrain/terrainLayerMipBudget.h"

using VL::AllocateTerrainLayerMips;
using VL::TerrainLayerMaxMipLevel;
using VL::TerrainLayerMipBudgetConfig;
using VL::TerrainLayerMipBudgetEntry;
using VL::TerrainLayerMipBudgetResult;
using VL::TerrainLayerMipPageCount;

namespace
{
TerrainLayerMipBudgetEntry MakeEntry(
    uint32_t layerId,
    uint8_t priority,
    uint32_t basePagesPerSide,
    uint32_t maxMip,
    uint32_t targetMip)
{
    TerrainLayerMipBudgetEntry entry;
    entry.layerId = layerId;
    entry.priority = priority;
    entry.basePagesPerSide = basePagesPerSide;
    entry.maxMip = maxMip;
    entry.targetMip = targetMip;
    return entry;
}

uint32_t TotalPagesForSelections(
    const std::vector<TerrainLayerMipBudgetEntry>& entries,
    const TerrainLayerMipBudgetResult& result)
{
    std::array<uint32_t, 256> basePages{};
    for (const TerrainLayerMipBudgetEntry& entry : entries)
    {
        if (entry.basePagesPerSide != 0)
            basePages[entry.layerId] = entry.basePagesPerSide;
    }
    uint32_t total = 0;
    for (const TerrainLayerMipBudgetResult::Selection& selection :
         result.selections)
    {
        total += TerrainLayerMipPageCount(
            basePages[selection.layerId], selection.mip);
    }
    return total;
}

std::vector<uint32_t> SelectedMipsForLayer(
    const TerrainLayerMipBudgetResult& result,
    uint32_t layerId)
{
    std::vector<uint32_t> mips;
    for (const TerrainLayerMipBudgetResult::Selection& selection :
         result.selections)
    {
        if (selection.layerId == layerId)
            mips.push_back(selection.mip);
    }
    return mips;
}

bool HasSelectedMip(
    const TerrainLayerMipBudgetResult& result,
    uint32_t layerId,
    uint32_t mip)
{
    for (const TerrainLayerMipBudgetResult::Selection& selection :
         result.selections)
    {
        if (selection.layerId == layerId && selection.mip == mip)
            return true;
    }
    return false;
}
} // namespace

TEST(TerrainLayerMipBudget, PageCountFollowsMipHalvingFor1KLayer)
{
    // 1K 源 / 128 页粒度 = 每边 8 页：mip0=64、mip1=16、mip2=4、mip3=1。
    EXPECT_EQ(TerrainLayerMipPageCount(8, 0), 64u);
    EXPECT_EQ(TerrainLayerMipPageCount(8, 1), 16u);
    EXPECT_EQ(TerrainLayerMipPageCount(8, 2), 4u);
    EXPECT_EQ(TerrainLayerMipPageCount(8, 3), 1u);
    // 1K 层没有 mip4；越界不能伪装成 1 页。
    EXPECT_EQ(TerrainLayerMipPageCount(8, 4), 0u);
    EXPECT_EQ(TerrainLayerMipPageCount(8, TerrainLayerMaxMipLevel + 1), 0u);
    EXPECT_EQ(TerrainLayerMipPageCount(0, 0), 0u);
}

TEST(TerrainLayerMipBudget, PageCountFollowsMipHalvingFor2KLayer)
{
    // 2K 源 = 每边 16 页：mip0=256、mip4=1。
    EXPECT_EQ(TerrainLayerMipPageCount(16, 0), 256u);
    EXPECT_EQ(TerrainLayerMipPageCount(16, 1), 64u);
    EXPECT_EQ(TerrainLayerMipPageCount(16, 4), 1u);
    EXPECT_EQ(TerrainLayerMipPageCount(16, 5), 0u);
}

TEST(TerrainLayerMipBudget, EmptyInputProducesEmptyAllocation)
{
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 512;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips({}, config);
    EXPECT_EQ(result.allocatedLayers, 0u);
    EXPECT_EQ(result.allocatedSets, 0u);
    EXPECT_EQ(result.totalPages, 0u);
    EXPECT_TRUE(result.selections.empty());
}

TEST(TerrainLayerMipBudget, NeverExceedsPageBudget)
{
    // 7 个 1K tile × 4 层全部想用 mip0 需要 1792 页，超过容量 3.5 倍。
    // 这是实测中触发 600ms/帧的配置，分配后必须收敛到预算内。
    std::vector<TerrainLayerMipBudgetEntry> entries;
    for (uint32_t tile = 0; tile < 7; ++tile)
    {
        for (uint32_t layer = 0; layer < 4; ++layer)
        {
            entries.push_back(MakeEntry(tile * 4 + layer,
                static_cast<uint8_t>(255 - tile * 20), 8, 3, 0));
        }
    }
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 512;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_LE(TotalPagesForSelections(entries, result), 512u);
    EXPECT_LE(result.totalPages, 512u);
    EXPECT_EQ(result.totalPages,
        TotalPagesForSelections(entries, result));
    EXPECT_EQ(result.allocatedLayers, 28u);
    EXPECT_FALSE(result.budgetExceeded);
    // 预算不足以让所有需求都用 mip0，必然发生细化受限。
    EXPECT_GT(result.clampedRefinements, 0u);
}

TEST(TerrainLayerMipBudget, NearerEntriesGetFinerMipsWhenBudgetTight)
{
    // 两个需求竞争有限预算：近处（priority 高）先细化，远处停在粗 mip。
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 64, 8, 3, 0),   // 远处
        MakeEntry(1, 255, 8, 3, 0),  // 近处
    };
    TerrainLayerMipBudgetConfig config;
    // 每层保留 1 页最粗回退，基线两页；近处细化到 mip1 后总页数 18，
    // 再细化到 mip0 需要 66。远处没有足够预算从 mip3 细化到 mip2。
    config.pageBudget = 20;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    const std::vector<uint32_t> nearMips = SelectedMipsForLayer(result, 1);
    const std::vector<uint32_t> farMips = SelectedMipsForLayer(result, 0);
    ASSERT_EQ(nearMips.size(), 2u);
    ASSERT_EQ(farMips.size(), 1u);
    EXPECT_EQ(nearMips[0], 1u);
    EXPECT_EQ(nearMips[1], 3u);
    EXPECT_EQ(farMips[0], 3u);
    EXPECT_LT(nearMips[0], farMips[0])
        << "nearer entry must not end up coarser than the distant one";
    EXPECT_EQ(result.entriesByMip[1], 1u);
    EXPECT_EQ(result.entriesByMip[3], 1u);
    EXPECT_LE(result.totalPages, 20u);
}

TEST(TerrainLayerMipBudget, GenerousBudgetRefinesEveryEntryToTarget)
{
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 200, 8, 3, 0),
        MakeEntry(1, 120, 8, 3, 0),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 4096;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_TRUE(HasSelectedMip(result, 0, 0));
    EXPECT_TRUE(HasSelectedMip(result, 0, 3));
    EXPECT_TRUE(HasSelectedMip(result, 1, 0));
    EXPECT_TRUE(HasSelectedMip(result, 1, 3));
    EXPECT_EQ(result.totalPages, 130u);
    EXPECT_EQ(result.allocatedSets, 4u);
    EXPECT_EQ(result.clampedRefinements, 0u);
}

TEST(TerrainLayerMipBudget, ZeroBudgetDisablesTheCap)
{
    // 0 表示“不限制”，用于与旧行为做画面对照。它仍应选到 feedback 目标。
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 10, 8, 3, 1),
        MakeEntry(1, 250, 8, 3, 0),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 0;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_TRUE(HasSelectedMip(result, 0, 1));
    EXPECT_TRUE(HasSelectedMip(result, 0, 3));
    EXPECT_TRUE(HasSelectedMip(result, 1, 0));
    EXPECT_TRUE(HasSelectedMip(result, 1, 3));
    // layer0: mip1(16)+尾页mip3(1)；layer1: mip0(64)+尾页mip3(1)。
    EXPECT_EQ(result.totalPages, 82u);
}

TEST(TerrainLayerMipBudget, NeverRefinesBeyondFeedbackTarget)
{
    // feedback 只想采 mip2；即使预算无限，也不能把 mip0 的 64 页装进来。
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 255, 8, 3, 2),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 4096;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_TRUE(HasSelectedMip(result, 0, 2));
    EXPECT_TRUE(HasSelectedMip(result, 0, 3));
    EXPECT_EQ(result.totalPages, 5u);
    EXPECT_EQ(result.entriesByMip[2], 1u);
}

TEST(TerrainLayerMipBudget, ClampsEachEntryToItsOwnLayerMipChain)
{
    // 512 源只有 mip0..2；2K 源有 mip0..4。不能用一个全局 coarsestMip。
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 200, 4, 4, 0),
        MakeEntry(1, 100, 16, 4, 2),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 4096;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_TRUE(HasSelectedMip(result, 0, 0));
    EXPECT_TRUE(HasSelectedMip(result, 0, 2));
    EXPECT_TRUE(HasSelectedMip(result, 1, 2));
    EXPECT_TRUE(HasSelectedMip(result, 1, 4));
    EXPECT_EQ(result.totalPages, 17u + 17u);
}

TEST(TerrainLayerMipBudget, KeepsNearAndFarMipsForTheSameLayer)
{
    // 同一个全局层可以同时出现在近处（目标 mip0）和远处（目标 mip3）。
    // 正确结果不是二选一，而是两个完整页集都驻留：64 + 1 页。
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 255, 8, 3, 0),
        MakeEntry(0, 32, 8, 3, 3),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 512;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_EQ(SelectedMipsForLayer(result, 0),
        std::vector<uint32_t>({0, 3}));
    EXPECT_EQ(result.totalPages, 64u + 1u);
    EXPECT_EQ(result.allocatedLayers, 1u);
    EXPECT_EQ(result.allocatedSets, 2u);
    EXPECT_EQ(result.entriesByMip[0], 1u);
    EXPECT_EQ(result.entriesByMip[3], 1u);
}

TEST(TerrainLayerMipBudget, DeduplicatesIdenticalLayerMipDemands)
{
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 255, 8, 3, 3),
        MakeEntry(0, 128, 8, 3, 3),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 512;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_EQ(SelectedMipsForLayer(result, 0),
        std::vector<uint32_t>({3}));
    EXPECT_EQ(result.totalPages, 1u);
    EXPECT_EQ(result.allocatedSets, 1u);
    EXPECT_EQ(result.entriesByMip[3], 2u);
    EXPECT_EQ(result.selectedSetsByMip[3], 1u);
}

TEST(TerrainLayerMipBudget, ReportsBudgetExceededWhenBaselineCannotFit)
{
    // 每个可见层至少一页；3 层放不进 2 页预算时不能继续淘汰追预算，
    // 调用方应保持最粗工作集并接受暂时超预算。
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(0, 255, 8, 3, 0),
        MakeEntry(1, 128, 8, 3, 0),
        MakeEntry(2, 64, 8, 3, 0),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 2;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_TRUE(result.budgetExceeded);
    EXPECT_EQ(result.totalPages, 3u);
    EXPECT_EQ(result.allocatedSets, 3u);
    EXPECT_EQ(result.selectedSetsByMip[3], 3u);
}

TEST(TerrainLayerMipBudget, SkipsLayerWithoutPagesWithoutBreakingBudget)
{
    const std::vector<TerrainLayerMipBudgetEntry> entries = {
        MakeEntry(3, 255, 0, 4, 0),
        MakeEntry(4, 200, 8, 3, 0),
    };
    TerrainLayerMipBudgetConfig config;
    config.pageBudget = 65;
    const TerrainLayerMipBudgetResult result =
        AllocateTerrainLayerMips(entries, config);

    EXPECT_TRUE(SelectedMipsForLayer(result, 3).empty());
    EXPECT_TRUE(HasSelectedMip(result, 4, 0));
    EXPECT_TRUE(HasSelectedMip(result, 4, 3));
    EXPECT_EQ(result.totalPages, 65u);
}
