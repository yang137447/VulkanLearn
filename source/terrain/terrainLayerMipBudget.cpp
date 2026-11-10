#include "terrain/terrainLayerMipBudget.h"

#include <algorithm>

namespace VL
{

namespace
{
uint32_t PagesForMip(const TerrainLayerMipBudgetEntry& entry, uint32_t mip)
{
    return TerrainLayerMipPageCount(entry.basePagesPerSide, mip);
}

// base 的有效 mip 上界：1K(8) -> 3、2K(16) -> 4、512(4) -> 2。
uint32_t MaxMipForBase(uint32_t basePagesPerSide)
{
    uint32_t maxMip = 0;
    while (maxMip < TerrainLayerMaxMipLevel &&
           (basePagesPerSide >> (maxMip + 1)) != 0)
    {
        ++maxMip;
    }
    return maxMip;
}

uint32_t PagesForSelectedMask(
    uint32_t basePagesPerSide,
    uint32_t selectedMask)
{
    uint32_t pages = 0;
    for (uint32_t mip = 0; mip <= TerrainLayerMaxMipLevel; ++mip)
    {
        if ((selectedMask & (1u << mip)) != 0)
            pages += TerrainLayerMipPageCount(basePagesPerSide, mip);
    }
    return pages;
}
} // namespace

TerrainLayerMipBudgetResult AllocateTerrainLayerMips(
    const std::vector<TerrainLayerMipBudgetEntry>& entries,
    const TerrainLayerMipBudgetConfig& config)
{
    TerrainLayerMipBudgetResult result;
    if (entries.empty())
        return result;

    struct AllocationState
    {
        size_t entryIndex = 0;
        uint32_t maxMip = TerrainLayerMaxMipLevel;
        uint32_t targetMip = TerrainLayerMaxMipLevel;
        uint32_t currentMip = TerrainLayerMaxMipLevel;
    };
    std::array<uint32_t, 256> layerBasePages{};
    std::array<uint32_t, 256> layerCoarsestMips{};
    std::array<uint32_t, 256> layerSelectedMasks{};
    std::array<std::array<uint32_t, TerrainLayerMaxMipLevel + 1>, 256>
        layerDemandCounts{};

    std::vector<AllocationState> states;
    states.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
    {
        const TerrainLayerMipBudgetEntry& entry = entries[index];
        // 非法源分辨率不参与页记账；给它一个确定的上界，调用方会跳过。
        if (entry.basePagesPerSide == 0)
            continue;
        AllocationState state;
        state.entryIndex = index;
        state.maxMip = std::min(
            {entry.maxMip,
             TerrainLayerMaxMipLevel,
             MaxMipForBase(entry.basePagesPerSide)});
        state.targetMip = std::min(entry.targetMip, state.maxMip);
        state.currentMip = state.maxMip;
        // 同一层的所有需求共享同一个源页布局；生产路径由 catalog 保证。
        if (layerBasePages[entry.layerId] == 0)
            layerBasePages[entry.layerId] = entry.basePagesPerSide;
        layerCoarsestMips[entry.layerId] =
            std::max(layerCoarsestMips[entry.layerId], state.maxMip);
        states.push_back(state);
    }

    // 预算为 0 表示不限制：直接按每层能支持的最细 mip 分配（对照/测试路径）。
    if (config.pageBudget == 0)
    {
        for (AllocationState& state : states)
        {
            state.currentMip = state.targetMip;
            const TerrainLayerMipBudgetEntry& entry =
                entries[state.entryIndex];
            layerSelectedMasks[entry.layerId] |= 1u << state.currentMip;
            if (config.retainCoarsestMipPerLayer)
            {
                layerSelectedMasks[entry.layerId] |=
                    1u << layerCoarsestMips[entry.layerId];
            }
            ++layerDemandCounts[entry.layerId][state.currentMip];
        }
    }
    else
    {
        // 基线：每个需求先落在最粗 mip，同一层的重复页集只计一次。
        for (const AllocationState& state : states)
        {
            const TerrainLayerMipBudgetEntry& entry =
                entries[state.entryIndex];
            layerSelectedMasks[entry.layerId] |= 1u << state.currentMip;
            if (config.retainCoarsestMipPerLayer)
            {
                layerSelectedMasks[entry.layerId] |=
                    1u << layerCoarsestMips[entry.layerId];
            }
            ++layerDemandCounts[entry.layerId][state.currentMip];
        }
        uint64_t used = 0;
        for (uint32_t layerId = 0; layerId < 256; ++layerId)
            used += PagesForSelectedMask(
                layerBasePages[layerId], layerSelectedMasks[layerId]);
        if (used > config.pageBudget)
            result.budgetExceeded = true;

        // 按优先级降序排列“细化候选”：近处层先细化，保证近处更清晰。
        std::sort(states.begin(), states.end(),
            [&entries](const AllocationState& left, const AllocationState& right)
            {
                const TerrainLayerMipBudgetEntry& a =
                    entries[left.entryIndex];
                const TerrainLayerMipBudgetEntry& b =
                    entries[right.entryIndex];
                if (a.priority != b.priority)
                    return a.priority > b.priority;
                // 同优先级先服务目标更细的层；仍相同时按 layerId 稳定排序。
                if (left.targetMip != right.targetMip)
                    return left.targetMip < right.targetMip;
                return left.entryIndex < right.entryIndex;
            });

        // 顺序细化：最高优先级的层先连续细化到目标，剩余预算再给下一层。
        // 这样不会出现“近处层还没到目标，远处层先拿走预算”的倒挂。
        for (AllocationState& state : states)
        {
            const TerrainLayerMipBudgetEntry& entry =
                entries[state.entryIndex];
            uint32_t mip = state.currentMip;
            while (mip > state.targetMip)
            {
                const uint32_t nextMip = mip - 1;
                const uint32_t oldSelectedMask =
                    layerSelectedMasks[entry.layerId];
                const uint32_t newSelectedMask =
                    (oldSelectedMask & ~(1u << mip)) |
                    (1u << nextMip);
                uint32_t projectedMask = newSelectedMask;
                if (config.retainCoarsestMipPerLayer)
                    projectedMask |= 1u << layerCoarsestMips[entry.layerId];
                // 本需求离开 mip 后如果同层还有别的需求停在 mip，页集必须保留。
                if (layerDemandCounts[entry.layerId][mip] > 1)
                    projectedMask |= 1u << mip;
                // 同层已有别的需求选中 nextMip 时，页集已经计入。
                if (layerDemandCounts[entry.layerId][nextMip] > 0)
                    projectedMask |= 1u << nextMip;

                if (TerrainLayerMipPageCount(
                        entry.basePagesPerSide, nextMip) == 0)
                    break;
                const uint64_t projected =
                    used -
                    PagesForSelectedMask(
                        entry.basePagesPerSide, oldSelectedMask) +
                    PagesForSelectedMask(
                        entry.basePagesPerSide, projectedMask);
                if (projected > config.pageBudget)
                {
                    ++result.clampedRefinements;
                    break;
                }
                used = projected;
                --layerDemandCounts[entry.layerId][mip];
                ++layerDemandCounts[entry.layerId][nextMip];
                layerSelectedMasks[entry.layerId] = projectedMask;
                mip = nextMip;
            }
            state.currentMip = mip;
            ++result.entriesByMip[mip];
        }

        result.totalPages = static_cast<uint32_t>(used);
    }

    if (config.pageBudget == 0)
    {
        for (const AllocationState& state : states)
        {
            ++result.entriesByMip[state.targetMip];
        }
        for (uint32_t layerId = 0; layerId < 256; ++layerId)
        {
            result.totalPages += PagesForSelectedMask(
                layerBasePages[layerId], layerSelectedMasks[layerId]);
        }
    }

    for (uint32_t layerId = 0; layerId < 256; ++layerId)
    {
        const uint32_t mask = layerSelectedMasks[layerId];
        if (mask == 0)
            continue;
        ++result.allocatedLayers;
        for (uint32_t mip = 0; mip <= TerrainLayerMaxMipLevel; ++mip)
        {
            if ((mask & (1u << mip)) == 0)
                continue;
            result.selections.push_back({layerId, mip});
            ++result.allocatedSets;
            ++result.selectedSetsByMip[mip];
        }
    }
    std::sort(
        result.selections.begin(),
        result.selections.end(),
        [](const TerrainLayerMipBudgetResult::Selection& left,
           const TerrainLayerMipBudgetResult::Selection& right)
        {
            if (left.layerId != right.layerId)
                return left.layerId < right.layerId;
            return left.mip < right.mip;
        });
    return result;
}

} // namespace VL
