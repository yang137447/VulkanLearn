#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "terrain/terrainFeedback.h"
#include "terrain/terrainLayerFeedback.h"
#include "terrain/terrainLayerMipBudget.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerSources.h"
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
    // S2：按层所在 sourceSet 取各自的源 mip 上限；非空时优先于
    // sourceMipLevels，避免不同 set 的 mip 数不同导致多请求/少请求。
    const TerrainLayerSourceSets* sourceSets = nullptr;
    // 每帧最多发出的层页请求数，以及本帧最多重建多少张非驻留覆盖页。
    uint32_t maxRequestsPerFrame = 16;
    uint32_t maxCoverPageRebuildsPerFrame = 4;
    // 层页离开 feedback 视野后，超过这么多 epoch 才允许 idle 淘汰。
    uint64_t idleEvictionEpochs = 180;
    // 层仍在视野内、但该 mip 没被本帧选中（且不在相邻一档）时，用更短的
    // idle 窗口淘汰。否则“可见层所有已驻留 mip 续命”会让 capacity 对
    // 可见集完全失效（实测 capacity 64 时 resident 长期停在 248）。
    uint64_t unselectedMipIdleEvictionEpochs = 16;
    // debug console `terrainlayer preload` 的对照路径会关闭 idle 淘汰，
    // 让全量预取集合保持驻留，便于和 L3 基线做画面对比。
    bool idleEvictionEnabled = true;
    // L4b：精确 feedback 每命中一页，额外把该页周围 radius 圈页作为 prefetch
    // 候选。128×128 feedback 对细长/远处的层页会欠采样，邻域扩展避免
    // 永远不请求漏掉的页。
    uint32_t exactFeedbackNeighborhoodRadius = 1;
    // L4c：目标 mip 的页未驻留时，额外把更粗 1..N 级 mip 的同一 UV 区域页
    // 加入候选（优先级递减）。shader 在目标页缺页时会向更粗 mip 回退
    // （L4a 已有行为），但只有粗页也在驻留集里才真的有内容可采，否则回退
    // 之后仍然是 catalog 的 avgColor。0 关闭该行为；生产路径取 2。
    uint32_t fallbackPrefetchMipLevels = 2;
    // 饥饿保护：有效 texel 少于该值就认为“本帧没有任何可信可见性”，
    // 不推进 idle epoch、不改 lastSeen、不做预算淘汰。默认 1 表示只有
    // 完全空的精确 feedback 才算饥饿；调大可以覆盖“只剩几个有效 texel”
    // 的欠采样帧。
    uint32_t minValidFeedbackTexels = 1;
    // 连续饥饿帧数上限。饥饿期间 idle 窗口按“等效 epoch 补偿”保持不老化，
    // 补偿每秒最多增长到该帧数；超过后不再增长，驻留集开始按正常窗口
    // 慢速降级，保证长时间没有 feedback 时不会永久锁死显存。
    uint32_t starvationGraceFrames = 480;
    // 恢复精确 feedback 后，每个有效帧抵消多少补偿 epoch（>=1）。一次
    // 长时间饥饿不会永久放大 idle 窗口。
    uint64_t starvationCreditDecayPerFrame = 1;
    // 饥饿超过宽限期后，每帧最多淘汰多少页。补偿封顶后所有页会同时越过
    // idle 阈值，不设速率上限就会一次性清空；2 页/帧让降级是渐进的。
    // 0 表示不限速（退化为普通 idle 淘汰）。
    uint32_t starvationEvictionPagesPerFrame = 2;
    // L4e：相机前向预取。相机本帧沿前向的位移超过阈值时，把可见层页在
    // 细一级 mip 的 2×2 子页提前请求。使用独立于 feedback 的每帧名额，
    // 不挤占本帧可见页请求；每个请求仍走 RequestPageAtEpoch 的容量/安全窗。
    bool prefetchFinerMipEnabled = false;
    uint32_t prefetchPagesPerFrame = 4;
    // 全局页预算：把“每个可见需求选哪个 mip”从按屏幕像素独立决定，改为
    // 在容量约束下统一分配（见 terrainLayerMipBudget.h）。契约要求“选定
    // mip 后必须请求该 mip 的全部页”，因此每个 (layer, mip) 页集是固定
    // 成本；多 tile 场景若各自按像素选 mip，总页数可达容量的数倍，每帧
    // “请求->淘汰->再请求”永不收敛（实测 7 个 1K tile × 4 层需要 1792 页
    // vs 容量 512，每帧 CPU 600 ms）。分配器按屏幕覆盖优先级（近处优先）
    // 细化，并对去重后的 (layer, mip) 页集记账，保证总页数 <= 预算；
    // 同一层的近处和远处可以各保留一个 mip，避免整层只有一个级别。
    // 0 表示关闭预算（退化为按 record.mip 直接使用反馈的目标 mip）。
    uint32_t layerPageBudget = 0;
};

struct TerrainLayerVisibilityStats
{
    uint32_t coverPages = 0;
    uint32_t resolvedCoverPages = 0;
    uint32_t rebuiltCoverPages = 0;
    uint32_t skippedCoverPages = 0;
    uint32_t visibleLayers = 0;
    uint32_t candidatePages = 0;
    // 本帧 feedback 命中且已驻留的页数（真正的 feedback hit）。
    uint32_t markedPages = 0;
    uint32_t attemptedRequests = 0;
    uint32_t acceptedRequests = 0;
    uint32_t deferredRequests = 0;
    uint32_t pendingRequests = 0;
    uint32_t evictedPages = 0;
    // L4c：目标页缺页、shader 会向更粗 mip 回退的候选页数。
    uint32_t fallbackTargetPages = 0;
    // +1/+2/+3 级粗 mip 页数；覆盖页路径也保留这一遥测。
    std::array<uint32_t, 3> fallbackLevelPages{};
    // 本帧是“精确 feedback 饥饿”触发的覆盖页回退估计。只有回退时才有意义：
    // 它表示 visibleLayers/candidates 来自覆盖页推导，而不是精确层页。
    bool usedCoverFallback = false;
    // 本帧成功请求的页按 mip 分布，用于确认 LOD/预算策略符合预期。
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> mipRequestCounts{};
};

struct TerrainLayerExactFeedbackStats
{
    uint32_t validSlots = 0;
    uint32_t distinctPages = 0;
    uint32_t visibleLayers = 0;
    uint32_t markedPages = 0;
    // pageIndex 超出该层该 mip 的实际页数：格式损坏或层配置在运行期变了。
    uint32_t outOfRangePages = 0;
    uint32_t candidatePages = 0;
    uint32_t attemptedRequests = 0;
    uint32_t acceptedRequests = 0;
    uint32_t deferredRequests = 0;
    uint32_t pendingRequests = 0;
    uint32_t evictedPages = 0;
    // L4c：需要回退的 feedback texel 数（用于估算回退像素占比）、目标页
    // 缺页的 distinct 页数，以及额外加入的 +1/+2/+3 级粗 mip 页数。
    uint32_t fallbackSlots = 0;
    uint32_t fallbackTargetPages = 0;
    std::array<uint32_t, 3> fallbackLevelPages{};
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> mipRequestCounts{};
    // 有效 feedback texel 按“目标 mip”的分布：确认 shader 实际想采哪一级，
    // 以及回退是否集中在某几级（回退级分布）。
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> targetMipSlots{};
    // L4e：目标页缺页时是否存在可采的更粗驻留页。
    //   fallbackBlendPages = 有粗页，shader 按 mip 差做 avgColor 渐变；
    //   avgColorPages      = 目标与所有更粗 mip 都缺页，整页 avgColor。
    uint32_t fallbackBlendPages = 0;
    uint32_t avgColorPages = 0;
    // L4e：相机前向预取的可见页数（产生过细一级子页的 feedback 页）、
    // 请求尝试数与成功数。不计入 mipRequestCounts（那是可见页请求）。
    uint32_t prefetchPages = 0;
    uint32_t prefetchRequests = 0;
    uint32_t prefetchAccepted = 0;
    // 本帧精确 feedback 饥饿：没有（或低于阈值）有效 texel。此时驻留集
    // 保持上一帧状态，调用方应回退到覆盖页推导继续估计可见层。
    bool starved = false;
    // 本帧生效的 idle 窗口补偿 epoch（饥饿期间随饥饿帧数增长并封顶）。
    uint64_t idleGraceEpochs = 0;
    // 全局 mip 预算分配的产物：本帧分配出的总页数、去重页集数、因预算
    // 不足被拒绝的细化次数、最粗工作集仍超预算的标记，以及各 mip 承载的
    // 可见需求/页集分布。
    // layerPageBudget=0 时全为 0。
    // 控制台据此确认“近处是否真的拿到了细 mip”。
    uint32_t budgetTotalPages = 0;
    uint32_t budgetAllocatedSets = 0;
    uint32_t budgetClampedRefinements = 0;
    bool budgetExceeded = false;
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> budgetEntriesByMip{};
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> budgetSetsByMip{};
};

// 层页 feedback 的健康状态，跨帧保存在 resolver 里。它只描述“精确 feedback
// 这一路是否可信”，不改变页本身的 lastSeen/lastTouched 语义。
struct TerrainLayerFeedbackHealth
{
    // 最近一次收到可信精确 feedback 的 epoch；饥饿期间保持不变。
    uint64_t lastValidFeedbackEpoch = 0;
    // 连续饥饿帧数；任何一帧有效 feedback 立即清零。
    uint32_t starvedFrames = 0;
    // 加到 idle 窗口上的补偿 epoch。饥饿时随 starvedFrames 增长并封顶，
    // 恢复后按 starvationCreditDecayPerFrame 逐帧衰减。
    uint64_t starvationCreditEpochs = 0;
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

    // L4b：层号 + 层页 + mip 的精确 feedback。命中页推进 lastSeen/lastTouched；
    // 未驻留页进入同一跨帧队列。优先级由屏幕覆盖率（sampleCount）给出，
    // 不再依赖覆盖页 priority 的 mip 近似。
    TerrainLayerExactFeedbackStats ApplyExact(
        const TerrainLayerFeedbackFrameSummary& summary,
        TerrainLayerPageManager& layerPages,
        std::vector<TerrainLayerPageRequest>& pendingRequests,
        uint64_t frame,
        uint64_t completedEpoch,
        const TerrainLayerVisibilityConfig& config);

    static uint32_t MipForPriority(uint8_t priority);

    const TerrainLayerFeedbackHealth& FeedbackHealth() const noexcept
    {
        return feedbackHealth;
    }

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

    // 两条推导路径共用的尾部：续命 -> 入队 -> 排序 -> 限量请求 -> idle/预算淘汰。
    // 抽出共享实现是为了让 L4a（覆盖页）与 L4b（精确层页）永远不会在预算、
    // quarantine 或淘汰规则上分叉。
    struct CandidateCommitResult
    {
        uint32_t attemptedRequests = 0;
        uint32_t acceptedRequests = 0;
        uint32_t deferredRequests = 0;
        uint32_t evictedPages = 0;
        std::array<uint32_t, TerrainLayerMaxMipLevel + 1> mipRequestCounts{};
    };

    CandidateCommitResult CommitCandidates(
        const std::map<TerrainLayerPageKey, uint32_t>& candidates,
        const std::array<bool, 256>& layerSeen,
        TerrainLayerPageManager& layerPages,
        std::vector<TerrainLayerPageRequest>& pendingRequests,
        uint64_t frame,
        uint64_t completedEpoch,
        uint64_t seenEpoch,
        uint64_t extraIdleGraceEpochs,
        uint32_t maxIdleEvictions,
        bool allowBudgetEviction,
        const TerrainLayerVisibilityConfig& config);

    // L4c：目标页缺页 -> 把同一 UV 区域的更粗 mip 页加入候选。粗页优先级
    // 略低（+1 级 3/4、+2 级 1/2、再往粗 1/4），避免粗页抢占细页的请求
    // 名额；请求总量仍由 maxRequestsPerFrame 限制，扩容不会绕过预算。
    static void ExpandCoarserFallbackCandidates(
        std::map<TerrainLayerPageKey, uint32_t>& candidates,
        const TerrainLayerPageManager& layerPages,
        const TerrainLayerVisibilityConfig& config,
        uint32_t& fallbackTargetPages,
        std::array<uint32_t, 3>& fallbackLevelPages);

    // 该层允许请求的最大 mip：层自身 mip 链与源数组 mip 数取小。
    static uint32_t MaxMipForLayer(
        const TerrainLayerPageManager& layerPages,
        uint32_t layerId,
        const TerrainLayerVisibilityConfig& config);

    struct PrefetchCandidate
    {
        TerrainLayerPageKey key;
        uint32_t priority = 1;
    };

    // L4e：相机前向移动时，为本帧精确 feedback 的可见层页预取细一级 mip
    // 的 2×2 子页。收集必须在可见页请求提交**之前**：以“本帧开始时该页
    // 是否驻留”判定，否则同帧刚请求的目标页会被误认为已驻留。实际请求
    // 在可见页提交之后发出，保证不挤占本帧采样页的物理槽位。
    static std::vector<PrefetchCandidate> CollectFinerMipPrefetch(
        const TerrainLayerFeedbackFrameSummary& summary,
        const TerrainLayerPageManager& layerPages,
        const TerrainLayerVisibilityConfig& config,
        TerrainLayerExactFeedbackStats& stats);

    // 页内容是静态的，因此一个覆盖页的层集合只需烘焙/统计一次；缓存按
    // (placementId, tileX, tileZ) 索引，数量上限就是 World 的虚拟页数。
    std::map<std::tuple<std::string, uint32_t, uint32_t>, CoverPageLayerSet>
        coverPageCache;

    // 精确 feedback 的饥饿状态。只在渲染线程的 feedback 消费点更新。
    TerrainLayerFeedbackHealth feedbackHealth;
};

} // namespace VL
