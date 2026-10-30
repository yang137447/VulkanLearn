#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "terrain/terrainFeedback.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainPageManager.h"

namespace VL
{

// 一条待处理的层页请求。层页身份是 (layerId, mip, pageX, pageY)，与覆盖页的
// (placementId, tile) 完全不同，因此单独维护一条跨帧队列。
struct TerrainLayerPageRequest
{
    TerrainLayerPageKey key;
    uint32_t priority = 0;
    // 连续请求失败次数：同 priority 下按它轮转，避免高优先级页长期抢不到
    // 安全槽位时把其他页饿死。
    uint32_t deferredRequestCount = 0;
};

struct TerrainLayerVisibilityConfig
{
    // 覆盖页 top-4 中平均覆盖率低于该值的层不参与驻留（0..1）。
    float minLayerWeight = 0.05f;
    // TR.layers 的实际数量；越界 layerId 视为未绑定层并跳过。
    uint32_t layerCount = 0;
    // 三张源数组的最小 mip 级数；层自身 mip 链也会参与 clamp。
    uint32_t sourceMipLevels = 1;
    // 每帧最多发出的层页请求数，以及本帧最多重建多少张非驻留覆盖页。
    uint32_t maxRequestsPerFrame = 16;
    uint32_t maxCoverPageRebuildsPerFrame = 4;
    // 层页离开 feedback 视野后，超过这么多 epoch 才允许 idle 淘汰。
    uint64_t idleEvictionEpochs = 180;
    // debug console `terrainlayer preload` 的对照路径会关闭 idle 淘汰，
    // 让全量预取集合保持驻留，便于和 L3 基线做画面对比。
    bool idleEvictionEnabled = true;
};

struct TerrainLayerVisibilityStats
{
    uint32_t coverPages = 0;
    uint32_t resolvedCoverPages = 0;
    uint32_t rebuiltCoverPages = 0;
    uint32_t skippedCoverPages = 0;
    uint32_t visibleLayers = 0;
    uint32_t candidatePages = 0;
    uint32_t attemptedRequests = 0;
    uint32_t acceptedRequests = 0;
    uint32_t deferredRequests = 0;
    uint32_t pendingRequests = 0;
    uint32_t evictedPages = 0;
    // 本帧成功请求的页按 mip 分布，用于确认 LOD/预算策略符合预期。
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> mipRequestCounts{};
};

// 覆盖页 feedback -> 可见层 -> mip -> 该 mip 的全部层页。纯 CPU，绝不读 GPU
// atlas：常驻覆盖页用 CPU 镜像，非驻留页按需重建并缓存（地形内容静态）。
//
// mip 规则：覆盖页 feedback 的 priority 是“每页占多少屏幕像素”的对数压缩，
// 越高表示越近，因此 >=192 取 mip0、>=128 取 mip1、>=64 取 mip2，否则 mip3；
// 再按层自身 mip 链与源数组 mip 数 clamp。层页是平铺资产，选定 mip 后必须
// 请求该 mip 的**全部**页，避免 shader 的 UV 采样落到未请求页。
class TerrainLayerVisibilityResolver
{
public:
    void Reset();

    TerrainLayerVisibilityStats Apply(
        const TerrainFeedbackFrameSummary& summary,
        const TerrainPageManager& coverPages,
        TerrainLayerPageManager& layerPages,
        std::vector<TerrainLayerPageRequest>& pendingRequests,
        uint64_t frame,
        uint64_t completedEpoch,
        const TerrainLayerVisibilityConfig& config);

    static uint32_t MipForPriority(uint8_t priority);

private:
    struct CoverPageLayerSet
    {
        std::array<uint8_t, 4> layerIds{};
        std::array<float, 4> weights{};
        uint32_t count = 0;
        bool valid = false;
    };

    // 覆盖页 RGBA8 ID/权重 -> 按覆盖率排序的 top-4 层集合。
    static CoverPageLayerSet ExtractCoverPageLayers(
        const std::vector<uint8_t>& ids,
        const std::vector<uint8_t>& weights,
        float minLayerWeight);

    // 页内容是静态的，因此一个覆盖页的层集合只需烘焙/统计一次；缓存按
    // (placementId, tileX, tileZ) 索引，数量上限就是 World 的虚拟页数。
    std::map<std::tuple<std::string, uint32_t, uint32_t>, CoverPageLayerSet>
        coverPageCache;
};

} // namespace VL
