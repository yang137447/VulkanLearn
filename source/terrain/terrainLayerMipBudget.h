#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

// 一个“层 × 目标 mip”的可见需求：某层若选用 mip，需要 PagesPerSide² 页常驻。
//
// 背景：契约要求“选定 mip 后必须请求该 mip 的全部页”，否则 shader 的平铺
// UV 会采样到未请求页。页数随 mip 指数下降：
//   1K 层 mip0=64、mip1=16、mip2=4、mip3=1
//   2K 层 mip0=256、mip1=64、mip2=16、mip3=4、mip4=1
// 同一个全局层会在近处和远处同时出现，shader 对不同像素会给出不同目标
// mip；因此预算输入是 (layer, targetMip) 需求，而不是“每层一个目标”。
// 每个需求最终可以保留在目标 mip，或被预算压到更粗 mip；所有需求选中的
// (layer, mip) 去重后，每个页集都必须完整驻留。多 tile 场景下若所有需求
// 都按屏幕像素独立选 mip，总页数可以轻易超过 ResidentCapacity（默认 512），
// 于是每帧“请求 -> 淘汰 -> 再请求”永不收敛。实测 7 个 1K tile × 4 层全部
// 选 mip0 需要 1792 页，超过容量 3.5 倍，每帧 CPU 600 ms、帧率掉到 10。
//
// 本分配器把“每个可见需求选哪个 mip”变成**全局容量约束下的分配问题**：
// 从最粗 mip 起步（此时页数最少，必然装得下），按“近处优先”逐级细化，
// 直到该层到达 feedback 给出的目标 mip，或再加入下一级就会超过容量预算。
// 精确 feedback 的 priority 来自层页占用的屏幕像素数；透视投影下近处地形
// 覆盖更多屏幕像素，因此高 priority 就是“离相机更近、需要更清晰”的排序键。
// 这样：
//   1. 总页数始终 <= budget，系统必然收敛，不会反复淘汰；
//   2. 近处（屏幕覆盖大）的层先拿到细 mip，符合“越近越清晰”的画质要求；
//   3. 远处层保留粗 mip，仍有内容可采（而不是 avgColor）；
//   4. 分配结果不会比 feedback 目标更细——shader 只采目标 mip，给更细的页
//      只会白占容量，并不会让画面更清晰；
//   5. 同一层的多个目标 mip 可以同时保留；近处像素用细页集，远处像素用
//      粗页集，不会因为“整层只准一个 mip”而让远处页缺失。
struct TerrainLayerMipBudgetEntry
{
    uint32_t layerId = 0;
    // 8-bit 屏幕覆盖优先级（越大表示占屏幕越多，也就是越近）。
    uint8_t priority = 0;
    // 该层源分辨率决定的页边长基数（resolution / TerrainPageResolution）。
    uint32_t basePagesPerSide = 0;
    // 该层允许使用的最粗 mip（含）。它由层自身 mip 链与 sourceSet 决定；
    // 分配从这一级开始，因此它必须能装下。
    uint32_t maxMip = TerrainLayerMaxMipLevel;
    // shader 本帧想采的最细 mip（feedback 给出的目标）。分配只会向这个值
    // 细化，不会越过它；目标比 maxMip 粗时按 maxMip clamp。
    uint32_t targetMip = 0;
};

struct TerrainLayerMipBudgetConfig
{
    // 页预算。分配结果的总页数不会超过它；0 表示不限制（退化为按优先级
    // 独立选 mip 的旧行为，仅用于对照与测试）。
    uint32_t pageBudget = 0;
    // 每个参与分配的层始终保留其最粗 mip 的完整页集（512/1K 为 1 页，
    // 2K 也是 1 页）。这样 128×128 feedback 漏掉某个远处目标 mip 时，
    // shader 仍能沿 mip 链回退到有内容的尾页，而不是 avgColor。成本是
    // 每个可见层 1 页，远小于漏页造成的整块平均色。
    bool retainCoarsestMipPerLayer = true;
};

struct TerrainLayerMipBudgetResult
{
    // 去重后的最终 (layer, mip) 页集。调用方必须为每一项请求该 mip 的全部页。
    struct Selection
    {
        uint32_t layerId = 0;
        uint32_t mip = 0;
    };
    std::vector<Selection> selections;
    // 参与分配的层数、去重页集数、总页数，以及“想细化但预算不足”的次数。
    uint32_t allocatedLayers = 0;
    uint32_t allocatedSets = 0;
    uint32_t totalPages = 0;
    uint32_t clampedRefinements = 0;
    // 所有层都落在最粗 mip 仍超过预算。此时无法再通过降 mip 收敛；调用方
    // 应保持最粗工作集、停止细化；已驻留选中页不淘汰，空槽不足的请求
    // 自然 deferred，不能继续淘汰选中页来“凑”预算。
    bool budgetExceeded = false;
    // 每级 mip 最终承载的“需求数”与去重页集数，控制台据此确认近处是否
    // 真的拿到了细 mip，以及同一层是否同时保留了远近两级。
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> entriesByMip{};
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> selectedSetsByMip{};
};

// 在页预算内为每个可见需求分配 mip。分配顺序是“近处优先”：
//   1. 所有层先落到各自的 maxMip（页数最少）；
//   2. 按 priority 降序依次把每个需求细化到 targetMip；
//   3. 某层细化一级的代价 = 新 mip 页数 - 原 mip 页数，剩余预算不足就
//      停在当前 mip，继续处理下一需求。多个需求选中同一 (layer, mip) 时
//      页集只记账一次。
// 这样最高优先级的近处层先拿满清晰度；预算有剩余时再照顾较远层。
// 纯函数，不依赖任何 GPU 状态，可直接单测。
TerrainLayerMipBudgetResult AllocateTerrainLayerMips(
    const std::vector<TerrainLayerMipBudgetEntry>& entries,
    const TerrainLayerMipBudgetConfig& config);

// 指定 basePagesPerSide 与 mip 的页数（side²）；越界 mip 返回 0。
inline uint32_t TerrainLayerMipPageCount(uint32_t basePagesPerSide, uint32_t mip)
{
    if (basePagesPerSide == 0 || mip > TerrainLayerMaxMipLevel)
        return 0;
    // base 是源分辨率/页粒度的商（1K -> 8），右移 mip 得到该级边长。
    // 边长降到 0 说明该 mip 已越过尾页（1K 只有 mip0..3），不能把越界
    // mip 伪装成 1 页，否则分配器会给出 shader 永远不会采样的级别。
    const uint32_t side = basePagesPerSide >> mip;
    if (side == 0)
        return 0;
    return side * side;
}

} // namespace VL
