#include "terrain/terrainLayerVisibility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "terrain/terrainLayerVT.h"

namespace VL
{
namespace
{

// 覆盖页 priority 反映“近/远”，层权重反映“这层在该页占多少”。两者相乘
// 后压回 0..255，避免低权重层和大面积层拿到同样驻留优先级。
uint32_t ScaleLayerPriority(uint8_t coverPriority, float weight)
{
    const float clampedWeight = std::clamp(weight, 0.0f, 1.0f);
    const float scaled =
        static_cast<float>(coverPriority) * (0.5f + 0.5f * clampedWeight);
    return static_cast<uint32_t>(std::lround(std::clamp(scaled, 0.0f, 255.0f)));
}

// 粗 mip 预取页的优先级衰减：+1 级 3/4、+2 级 1/2、其余 1/4。
// 粗页只是“让 shader 的回退有内容可采”，不该抢细页的请求名额。
uint32_t ScaleFallbackPriority(uint32_t priority, uint32_t level)
{
    uint32_t scaled = priority;
    if (level <= 1)
        scaled = priority * 3u / 4u;
    else if (level == 2)
        scaled = priority / 2u;
    else
        scaled = priority / 4u;
    return std::max(1u, scaled);
}

} // namespace

void TerrainLayerVisibilityResolver::Reset()
{
    coverPageCache.clear();
    feedbackHealth = TerrainLayerFeedbackHealth{};
}

uint32_t TerrainLayerVisibilityResolver::MipForPriority(uint8_t priority)
{
    // 五档阶梯：2K 层的 mip4 只有 1 页，成本很低，远景必须也能请求，
    // 否则会永久停在 avgColor。
    if (priority >= 192) return 0;
    if (priority >= 128) return 1;
    if (priority >= 64) return 2;
    if (priority >= 32) return 3;
    return 4;
}

uint32_t TerrainLayerVisibilityResolver::MaxMipForLayer(
    const TerrainLayerPageManager& layerPages,
    uint32_t layerId,
    const TerrainLayerVisibilityConfig& config)
{
    const uint32_t mipCount =
        TerrainLayerMipCount(layerPages.LayerSourceResolution(layerId));
    uint32_t maxMip = mipCount == 0 ? 0u : mipCount - 1;
    if (config.sourceMipLevels != 0)
        maxMip = std::min(maxMip, config.sourceMipLevels - 1);
    return maxMip;
}

void TerrainLayerVisibilityResolver::ExpandCoarserFallbackCandidates(
    std::map<TerrainLayerPageKey, uint32_t>& candidates,
    const TerrainLayerPageManager& layerPages,
    const TerrainLayerVisibilityConfig& config,
    uint32_t& fallbackTargetPages,
    std::array<uint32_t, 3>& fallbackLevelPages)
{
    // fallbackLevelPages 固定 3 项；超出部分没有统计槽位，也远超回退收益。
    const uint32_t fallbackLevels = std::min(
        config.fallbackPrefetchMipLevels, 3u);
    if (fallbackLevels == 0 || candidates.empty())
        return;

    // 先快照当前候选再插入粗页：粗页本身也可能是缺页，但不能因为
    // “刚插入的粗页又缺页”而继续向更粗级联，否则遍历中扩容且级数失控。
    std::vector<std::pair<TerrainLayerPageKey, uint32_t>> targets;
    targets.reserve(candidates.size());
    for (const auto& entry : candidates)
        targets.push_back(entry);

    for (const auto& [key, priority] : targets)
    {
        if (key.layerId >= config.layerCount)
            continue;
        // 只对“会真的触发 shader 回退”的缺页页扩容；已驻留页不需要粗页。
        if (layerPages.IsResident(key))
            continue;

        const uint32_t maxMip =
            MaxMipForLayer(layerPages, key.layerId, config);
        ++fallbackTargetPages;
        const uint32_t levels = std::min(
            fallbackLevels,
            maxMip > key.mip ? maxMip - key.mip : 0u);
        for (uint32_t level = 1; level <= levels; ++level)
        {
            TerrainLayerPageKey coarser;
            coarser.layerId = key.layerId;
            coarser.mip = key.mip + level;
            // 每粗一级页数减半：同一 UV 区域落在 (pageX >> 1, pageY >> 1)。
            coarser.pageX = key.pageX >> level;
            coarser.pageY = key.pageY >> level;
            const uint32_t coarserPriority =
                ScaleFallbackPriority(priority, level);
            auto inserted =
                candidates.emplace(coarser, coarserPriority);
            if (inserted.second)
            {
                ++fallbackLevelPages[level - 1];
            }
            else if (inserted.first->second < coarserPriority)
            {
                inserted.first->second = coarserPriority;
            }
        }
    }
}

TerrainLayerVisibilityStats TerrainLayerVisibilityResolver::Apply(
    const TerrainFeedbackFrameSummary& summary,
    const TerrainPageManager& coverPages,
    TerrainLayerPageManager& layerPages,
    std::vector<TerrainLayerPageRequest>& pendingRequests,
    uint64_t frame,
    uint64_t completedEpoch,
    const TerrainLayerVisibilityConfig& config)
{
    TerrainLayerVisibilityStats stats;
    stats.coverPages = static_cast<uint32_t>(summary.pages.size());
    // 精确路径饥饿时，这一帧的可见层估计完全来自覆盖页推导。标记出来，
    // 让统计能区分“正常的 L4a 启动/prefetch”与“精确 feedback 断流后的回退”。
    stats.usedCoverFallback = feedbackHealth.starvedFrames != 0;
    if (config.layerCount == 0 || config.sourceMipLevels == 0)
        return stats;

    // 1. 覆盖页 -> 可见层集合（带缓存）。非驻留页的重建是高度图烘焙，
    //    每帧限流，避免一次 feedback 触发上百次全页采样。
    std::map<TerrainLayerPageKey, uint32_t> candidates;
    std::array<bool, 256> layerSeen{};
    uint32_t rebuildBudget = config.maxCoverPageRebuildsPerFrame;

    for (const TerrainFeedbackPageRecord& record : summary.pages)
    {
        const TerrainPageManager::PageState* page =
            coverPages.FindPageByVirtual(
                record.virtualPageX, record.virtualPageY);
        if (page == nullptr)
        {
            ++stats.skippedCoverPages;
            continue;
        }
        const auto cacheKey = std::make_tuple(
            page->key.placementId, page->key.tileX, page->key.tileZ);
        auto cached = coverPageCache.find(cacheKey);
        if (cached == coverPageCache.end())
        {
            std::vector<uint8_t> ids;
            std::vector<uint8_t> weights;
            const bool allowRebuild = rebuildBudget != 0 && !page->resident;
            if (!coverPages.ReadPageLayers(
                    page->key, allowRebuild, ids, weights))
            {
                // 非驻留且本帧重建预算用尽：不缓存失败，下一帧再试。
                ++stats.skippedCoverPages;
                continue;
            }
            if (allowRebuild)
            {
                ++stats.rebuiltCoverPages;
                --rebuildBudget;
            }
            cached = coverPageCache
                         .emplace(
                             cacheKey,
                             TerrainLayerVisibilityResolver::ExtractCoverPageLayers(
                                 ids,
                                 weights,
                                 config.minLayerWeight))
                         .first;
        }
        const CoverPageLayerSet& layerSet = cached->second;
        if (!layerSet.valid)
        {
            ++stats.skippedCoverPages;
            continue;
        }
        ++stats.resolvedCoverPages;

        const uint32_t coverMip = MipForPriority(record.priority);
        for (uint32_t slot = 0; slot < layerSet.count; ++slot)
        {
            const uint32_t layerId = layerSet.layerIds[slot];
            if (layerId >= config.layerCount) continue;
            const uint32_t mipCount = TerrainLayerMipCount(
                layerPages.LayerSourceResolution(layerId));
            uint32_t mip = std::min(
                coverMip,
                mipCount == 0 ? 0u : mipCount - 1);
            mip = std::min(mip, config.sourceMipLevels - 1);

            const uint32_t priority =
                ScaleLayerPriority(record.priority, layerSet.weights[slot]);
            const uint32_t side = layerPages.PagesPerSide(layerId, mip);
            for (uint32_t pageY = 0; pageY < side; ++pageY)
            {
                for (uint32_t pageX = 0; pageX < side; ++pageX)
                {
                    TerrainLayerPageKey key;
                    key.layerId = layerId;
                    key.mip = mip;
                    key.pageX = pageX;
                    key.pageY = pageY;
                    auto inserted = candidates.emplace(key, priority);
                    if (!inserted.second)
                    {
                        inserted.first->second = std::max(
                            inserted.first->second, priority);
                    }
                }
            }
            if (!layerSeen[layerId])
            {
                layerSeen[layerId] = true;
                ++stats.visibleLayers;
            }
        }
    }
    // L4c：目标 mip 缺页时，把同一 UV 区域的更粗 mip 页一起入队，保证
    // shader 的回退 mip 有内容可采（否则回退后仍是 avgColor）。
    // 只把扩展前的候选计入 feedback hit；粗页是预取，不是本帧可见页。
    const std::vector<std::pair<TerrainLayerPageKey, uint32_t>>
        visibleCandidates(candidates.begin(), candidates.end());
    ExpandCoarserFallbackCandidates(
        candidates,
        layerPages,
        config,
        stats.fallbackTargetPages,
        stats.fallbackLevelPages);
    stats.candidatePages = static_cast<uint32_t>(candidates.size());

    const uint64_t seenEpoch =
        summary.copiedEpoch != 0 ? summary.copiedEpoch : completedEpoch;
    // 候选页里已经驻留的那部分就是本帧真正的 feedback hit。
    for (const auto& [key, priority] : visibleCandidates)
    {
        if (layerPages.MarkPageUsedAtEpoch(key, seenEpoch, priority))
            ++stats.markedPages;
    }
    const CandidateCommitResult committed = CommitCandidates(
        candidates,
        layerSeen,
        layerPages,
        pendingRequests,
        frame,
        completedEpoch,
        seenEpoch,
        // 饥饿期间延长 idle 窗口，避免“没有 feedback”被当成“所有页都不可见”。
        feedbackHealth.starvationCreditEpochs,
        // 饥饿超过宽限期后限速淘汰；正常恢复后不限速。
        (feedbackHealth.starvedFrames != 0 &&
         feedbackHealth.starvedFrames > config.starvationGraceFrames)
            ? config.starvationEvictionPagesPerFrame
            : 0,
        // 饥饿宽限期内不做预算淘汰；保持上一帧驻留集。超过宽限期后才允许
        // 慢速降级，避免长时间无 feedback 时永久锁死显存。
        !(feedbackHealth.starvedFrames != 0 &&
          feedbackHealth.starvedFrames <= config.starvationGraceFrames),
        config);
    stats.attemptedRequests = committed.attemptedRequests;
    stats.acceptedRequests = committed.acceptedRequests;
    stats.deferredRequests = committed.deferredRequests;
    stats.evictedPages = committed.evictedPages;
    stats.mipRequestCounts = committed.mipRequestCounts;
    stats.pendingRequests =
        static_cast<uint32_t>(pendingRequests.size());
    return stats;
}

TerrainLayerExactFeedbackStats TerrainLayerVisibilityResolver::ApplyExact(
    const TerrainLayerFeedbackFrameSummary& summary,
    TerrainLayerPageManager& layerPages,
    std::vector<TerrainLayerPageRequest>& pendingRequests,
    uint64_t frame,
    uint64_t completedEpoch,
    const TerrainLayerVisibilityConfig& config)
{
    TerrainLayerExactFeedbackStats stats;
    stats.validSlots = summary.validTexelCount;
    stats.distinctPages = summary.distinctPageCount;
    if (config.layerCount == 0)
        return stats;

    const uint64_t seenEpoch =
        summary.copiedEpoch != 0 ? summary.copiedEpoch : completedEpoch;

    // 饥饿保护：精确反馈本帧没有（或少于阈值的）有效 texel。典型场景是
    // 相机贴地/极端陡视角、地形被近裁剪面裁掉，或覆盖页整帧缺页导致
    // shader 只能写 invalid。此时绝不能把“没有反馈”当成“全部不可见”：
    //   - 不续命、不入队、不淘汰，驻留集保持上一帧；
    //   - lastSeen/lastTouched 一律不动（不伪造成当前帧）；
    //   - 调用方回退到覆盖页推导（L4a）继续估计可见层。
    // 连续饥饿超过 starvationGraceFrames 后补偿封顶，idle 窗口恢复推进，
    // 驻留集按正常规则慢速降级，不会永久锁死。
    if (summary.validTexelCount < config.minValidFeedbackTexels)
    {
        ++feedbackHealth.starvedFrames;
        feedbackHealth.starvationCreditEpochs = std::min<uint64_t>(
            feedbackHealth.starvedFrames, config.starvationGraceFrames);
        stats.starved = true;
        stats.idleGraceEpochs = feedbackHealth.starvationCreditEpochs;
        // pending 队列保持原样：恢复后按原优先级继续请求，不做丢弃。
        stats.pendingRequests = static_cast<uint32_t>(pendingRequests.size());
        // 宽限期外允许降级：补偿已经封顶，idle 窗口重新随时间推进。这里仍然
        // 走同一份 CommitCandidates（空候选），保证即使调用方没有覆盖页回退
        // 可用，驻留集也会按限速规则缓慢缩减，不会永久锁死。
        if (feedbackHealth.starvedFrames > config.starvationGraceFrames)
        {
            const std::map<TerrainLayerPageKey, uint32_t> noCandidates;
            const std::array<bool, 256> noLayersSeen{};
            const CandidateCommitResult committed = CommitCandidates(
                noCandidates,
                noLayersSeen,
                layerPages,
                pendingRequests,
                frame,
                completedEpoch,
                seenEpoch,
                feedbackHealth.starvationCreditEpochs,
                config.starvationEvictionPagesPerFrame,
                true,
                config);
            stats.attemptedRequests = committed.attemptedRequests;
            stats.acceptedRequests = committed.acceptedRequests;
            stats.deferredRequests = committed.deferredRequests;
            stats.evictedPages = committed.evictedPages;
            stats.pendingRequests =
                static_cast<uint32_t>(pendingRequests.size());
        }
        return stats;
    }

    // 恢复：饥饿计数清零，最近有效 epoch 前进；补偿按配置逐帧衰减，
    // 让一次长时间饥饿留下的宽限窗口平滑退回正常值。
    feedbackHealth.starvedFrames = 0;
    feedbackHealth.lastValidFeedbackEpoch = seenEpoch;
    const uint64_t creditDecay = config.starvationCreditDecayPerFrame;
    if (creditDecay == 0)
    {
        // 0 表示不衰减：保留补偿，直到下一次饥饿重新覆盖。
    }
    else if (feedbackHealth.starvationCreditEpochs > creditDecay)
    {
        feedbackHealth.starvationCreditEpochs -= creditDecay;
    }
    else
    {
        feedbackHealth.starvationCreditEpochs = 0;
    }
    stats.idleGraceEpochs = feedbackHealth.starvationCreditEpochs;

    std::map<TerrainLayerPageKey, uint32_t> candidates;
    std::array<bool, 256> layerSeen{};

    // 1. 逐条精确页记录：pageIndex 还原成 (pageX,pageY)。只有中心页**缺页**
    //    时才按 radius 扩一圈邻域作为 prefetch：128×128 feedback 会漏采样
    //    细长/远处的页，邻域能补上；已驻留页不需要邻域，否则候选集会凭空
    //    膨胀到可见页的数倍，capacity 又会被“假可见”撑爆。
    //    中心页保留原 priority，邻域降档，避免邻域抢占请求名额。
    for (const TerrainLayerFeedbackRecord& record : summary.pages)
    {
        const uint32_t layerId = record.layerId;
        if (layerId >= config.layerCount)
        {
            ++stats.outOfRangePages;
            continue;
        }
        if (record.mip > TerrainLayerMaxMipLevel ||
            record.mip >= config.sourceMipLevels)
        {
            ++stats.outOfRangePages;
            continue;
        }
        const uint32_t side = layerPages.PagesPerSide(layerId, record.mip);
        if (side == 0 || record.pageIndex >= side * side)
        {
            ++stats.outOfRangePages;
            continue;
        }
        const uint32_t pageX = record.pageIndex % side;
        const uint32_t pageY = record.pageIndex / side;
        const uint32_t priority = std::max<uint32_t>(1, record.priority);
        const uint32_t prefetchPriority = std::max<uint32_t>(1, priority / 2);

        layerSeen[layerId] = true;

        TerrainLayerPageKey centerKey;
        centerKey.layerId = layerId;
        centerKey.mip = record.mip;
        centerKey.pageX = pageX;
        centerKey.pageY = pageY;
        // L4c：目标页缺页就是 shader 会走更粗 mip 混合的 texel。按 texel
        // 计数（不按 distinct 页），控制台据此估算“本帧多少像素在混合”。
        if (!layerPages.IsResident(centerKey))
        {
            ++stats.fallbackSlots;
            // L4e：区分“有粗页可混”和“只能纯 avgColor”。shader 从
            // target+1 逐级向粗找第一个有效页，因此任一更粗祖先驻留即
            // 可产生渐变。
            bool hasCoarser = false;
            const uint32_t maxMip = MaxMipForLayer(layerPages, layerId, config);
            for (uint32_t coarser = record.mip + 1;
                 coarser <= maxMip && !hasCoarser;
                 ++coarser)
            {
                const uint32_t ancestorSide =
                    layerPages.PagesPerSide(layerId, coarser);
                if (ancestorSide == 0) continue;
                const uint32_t shift = coarser - record.mip;
                TerrainLayerPageKey ancestorKey;
                ancestorKey.layerId = layerId;
                ancestorKey.mip = coarser;
                ancestorKey.pageX = pageX >> shift;
                ancestorKey.pageY = pageY >> shift;
                if (ancestorKey.pageX >= ancestorSide ||
                    ancestorKey.pageY >= ancestorSide)
                    continue;
                hasCoarser = layerPages.IsResident(ancestorKey);
            }
            if (hasCoarser) ++stats.fallbackBlendPages;
            else ++stats.avgColorPages;
        }
        stats.targetMipSlots[record.mip] += record.sampleCount;
        const uint32_t radius = layerPages.IsResident(centerKey)
            ? 0u
            : config.exactFeedbackNeighborhoodRadius;
        const uint32_t minX = radius > pageX ? 0 : pageX - radius;
        const uint32_t minY = radius > pageY ? 0 : pageY - radius;
        const uint32_t maxX = std::min(pageX + radius, side - 1);
        const uint32_t maxY = std::min(pageY + radius, side - 1);
        for (uint32_t y = minY; y <= maxY; ++y)
        {
            for (uint32_t x = minX; x <= maxX; ++x)
            {
                TerrainLayerPageKey key;
                key.layerId = layerId;
                key.mip = record.mip;
                key.pageX = x;
                key.pageY = y;
                const bool center = x == pageX && y == pageY;
                const uint32_t candidatePriority =
                    center ? priority : prefetchPriority;
                auto inserted = candidates.emplace(key, candidatePriority);
                if (!inserted.second)
                {
                    inserted.first->second = std::max(
                        inserted.first->second, candidatePriority);
                }
            }
        }
    }
    // L4c：缺页的反馈页同时把同一 UV 区域的更粗 mip 页加入候选。粗页优先
    // 级略低，且仍受 maxRequestsPerFrame 限制。
    // 只把扩展前的候选计入 feedback hit；粗页是预取，不是本帧可见页。
    const std::vector<std::pair<TerrainLayerPageKey, uint32_t>>
        visibleCandidates(candidates.begin(), candidates.end());
    // L4e：预取候选必须在可见页请求提交之前收集：此时目标页还没被本帧
    // 请求成驻留页，“当前页是否已驻留”才是真正的前置条件。
    const std::vector<PrefetchCandidate> prefetchCandidates =
        CollectFinerMipPrefetch(summary, layerPages, config, stats);
    ExpandCoarserFallbackCandidates(
        candidates,
        layerPages,
        config,
        stats.fallbackTargetPages,
        stats.fallbackLevelPages);
    stats.candidatePages = static_cast<uint32_t>(candidates.size());
    for (uint32_t layerId = 0; layerId < layerSeen.size(); ++layerId)
    {
        if (layerSeen[layerId]) ++stats.visibleLayers;
    }

    for (const auto& [key, priority] : visibleCandidates)
    {
        if (layerPages.MarkPageUsedAtEpoch(key, seenEpoch, priority))
            ++stats.markedPages;
    }

    const CandidateCommitResult committed = CommitCandidates(
        candidates,
        layerSeen,
        layerPages,
        pendingRequests,
        frame,
        completedEpoch,
        seenEpoch,
        // 恢复后的残留补偿只影响“离开视野”的页，不影响本帧命中页续命。
        feedbackHealth.starvationCreditEpochs,
        0,
        true,
        config);
    stats.attemptedRequests = committed.attemptedRequests;
    stats.acceptedRequests = committed.acceptedRequests;
    stats.deferredRequests = committed.deferredRequests;
    stats.evictedPages = committed.evictedPages;
    stats.mipRequestCounts = committed.mipRequestCounts;
    stats.pendingRequests = static_cast<uint32_t>(pendingRequests.size());
    // L4e：可见页提交之后再发预取请求，使用独立名额且不挤占本帧采样页。
    for (const PrefetchCandidate& candidate : prefetchCandidates)
    {
        if (layerPages.IsResident(candidate.key))
            continue;
        ++stats.prefetchRequests;
        if (layerPages.RequestPageAtEpoch(
                candidate.key,
                frame,
                candidate.priority,
                completedEpoch))
        {
            ++stats.prefetchAccepted;
        }
    }
    return stats;
}

std::vector<TerrainLayerVisibilityResolver::PrefetchCandidate>
TerrainLayerVisibilityResolver::CollectFinerMipPrefetch(
    const TerrainLayerFeedbackFrameSummary& summary,
    const TerrainLayerPageManager& layerPages,
    const TerrainLayerVisibilityConfig& config,
    TerrainLayerExactFeedbackStats& stats)
{
    std::vector<PrefetchCandidate> result;
    if (!config.prefetchFinerMipEnabled || config.prefetchPagesPerFrame == 0)
        return result;
    std::set<TerrainLayerPageKey> seen;
    for (const TerrainLayerFeedbackRecord& record : summary.pages)
    {
        if (result.size() >= config.prefetchPagesPerFrame)
            break;
        const uint32_t layerId = record.layerId;
        if (layerId >= config.layerCount)
            continue;
        // mip0 已经是最细一级；越界 mip 由可见页路径统计。
        if (record.mip == 0 || record.mip > TerrainLayerMaxMipLevel)
            continue;
        if (record.mip >= config.sourceMipLevels)
            continue;
        const uint32_t side = layerPages.PagesPerSide(layerId, record.mip);
        if (side == 0 || record.pageIndex >= side * side)
            continue;
        const uint32_t pageX = record.pageIndex % side;
        const uint32_t pageY = record.pageIndex / side;
        TerrainLayerPageKey centerKey;
        centerKey.layerId = layerId;
        centerKey.mip = record.mip;
        centerKey.pageX = pageX;
        centerKey.pageY = pageY;
        // 当前页缺页时不预取更细一级：本帧已经在向粗回退，细页会先占掉
        // 目标页/粗页需要的物理槽位，反而拉长缺页窗口。
        if (!layerPages.IsResident(centerKey))
            continue;
        const uint32_t finerMip = record.mip - 1;
        const uint32_t finerSide =
            layerPages.PagesPerSide(layerId, finerMip);
        if (finerSide == 0)
            continue;
        // shader 的 mip 是“每像素纹素数”的连续函数，相机靠近时只会往
        // 更细一级走；预取 2×2 子页正好覆盖该页在下一级上的全部区域。
        const std::array<VL::TerrainLayerVT::LayerChildPage, 4> children =
            VL::TerrainLayerVT::FinerMipChildren(pageX, pageY);
        bool prefetchedAny = false;
        for (const VL::TerrainLayerVT::LayerChildPage& child : children)
        {
            if (result.size() >= config.prefetchPagesPerFrame)
                break;
            if (child.pageX >= finerSide || child.pageY >= finerSide)
                continue;
            TerrainLayerPageKey key;
            key.layerId = layerId;
            key.mip = finerMip;
            key.pageX = child.pageX;
            key.pageY = child.pageY;
            if (!seen.insert(key).second)
                continue;
            prefetchedAny = true;
            if (layerPages.IsResident(key))
                continue;
            // 预取优先级低于任何可见页：同一帧里可见页先拿槽位，
            // 预取只填剩余。
            result.push_back(PrefetchCandidate{
                key, std::max<uint32_t>(1, record.priority / 3)});
        }
        if (prefetchedAny)
            ++stats.prefetchPages;
    }
    return result;
}

TerrainLayerVisibilityResolver::CandidateCommitResult
TerrainLayerVisibilityResolver::CommitCandidates(
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
    const TerrainLayerVisibilityConfig& config)
{
    CandidateCommitResult result;

    // 2. 续命只覆盖本帧 feedback 真正命中的页（调用方已在 CommitCandidates
    //    之前对 candidates 调用 MarkPageUsedAtEpoch）。覆盖页路径里 candidates
    //    就是选中 mip 的全部页，因此这条规则等价于“选中的 mip”；精确路径里
    //    它是屏幕真正采样到的层页。不再刷新“可见层的所有历史 mip”，否则
    //    每帧都会把 248 页刷成最新，capacity 永远压不下来。
    // 3. 新候选合并进跨帧 pending 队列；已驻留页不需要再请求。
    std::map<TerrainLayerPageKey, size_t> pendingIndex;
    for (size_t index = 0; index < pendingRequests.size(); ++index)
        pendingIndex[pendingRequests[index].key] = index;
    for (const auto& [key, priority] : candidates)
    {
        auto found = pendingIndex.find(key);
        if (found != pendingIndex.end())
        {
            TerrainLayerPageRequest& request = pendingRequests[found->second];
            request.priority = std::max(request.priority, priority);
            continue;
        }
        if (layerPages.IsResident(key)) continue;
        pendingIndex.emplace(key, pendingRequests.size());
        TerrainLayerPageRequest request;
        request.key = key;
        request.priority = priority;
        pendingRequests.push_back(request);
    }

    // 4. 排序：priority 降序；同优先级按连续失败次数轮转，再按页身份稳定排序。
    std::sort(
        pendingRequests.begin(),
        pendingRequests.end(),
        [](const TerrainLayerPageRequest& lhs,
           const TerrainLayerPageRequest& rhs)
        {
            if (lhs.priority != rhs.priority)
                return lhs.priority > rhs.priority;
            if (lhs.deferredRequestCount != rhs.deferredRequestCount)
                return lhs.deferredRequestCount < rhs.deferredRequestCount;
            return lhs.key < rhs.key;
        });

    // 5. 每帧限量请求。失败项留在队列里并累加 deferred，不会丢失。
    std::vector<TerrainLayerPageRequest> survivors;
    survivors.reserve(pendingRequests.size());
    uint32_t attempted = 0;
    for (TerrainLayerPageRequest& request : pendingRequests)
    {
        if (layerPages.IsResident(request.key))
            continue;
        if (attempted < config.maxRequestsPerFrame)
        {
            ++attempted;
            ++result.attemptedRequests;
            if (layerPages.RequestPageAtEpoch(
                    request.key,
                    frame,
                    request.priority,
                    completedEpoch))
            {
                ++result.acceptedRequests;
                ++result.mipRequestCounts[request.key.mip];
                // 新驻留页的 lastSeenEpoch 来自 GPU epoch 上下文；如果帧首
                // 上下文落后于本帧 feedback epoch（加载期或测试里会发生），
                // 它会在同一帧的 idle 淘汰里被当成过期页。请求的理由就是
                // “本帧看到它”，这里直接按 seenEpoch 续命。
                layerPages.MarkPageUsedAtEpoch(
                    request.key, seenEpoch, request.priority);
                continue;
            }
            ++result.deferredRequests;
            ++request.deferredRequestCount;
        }
        survivors.push_back(request);
    }
    pendingRequests.swap(survivors);

    // 6. 淘汰：先清掉离开视野很久且已过 GPU 安全窗的层页，再按预算做 LRU。
    //    idle 淘汰使用 lastSeenEpoch（feedback 真正看到它的 epoch），而不是
    //    只升不降的 priority，避免近景页离开视野后永远压住新页。
    //    maxIdleEvictions 是饥饿降级的限速器：0 表示不限速（正常路径），
    //    非 0 时 idle 淘汰与预算淘汰共享同一份每帧名额。
    uint32_t evictionBudget =
        maxIdleEvictions == 0 ? 0xFFFFFFFFu : maxIdleEvictions;
    std::vector<TerrainLayerPageKey> idlePages;
    if (config.idleEvictionEnabled)
    {
        for (const auto& entry : layerPages.Pages())
        {
            if (evictionBudget == 0) break;
            const TerrainLayerPageManager::PageState& page = entry.second;
            if (!page.resident) continue;
            const uint32_t layerId = entry.first.layerId;
            uint64_t idleWindow = config.idleEvictionEpochs;
            const bool candidate =
                candidates.find(entry.first) != candidates.end();
            // 可见层里不是本帧候选的页（同层其它 mip、同 mip 未被采样的页）
            // 走短 idle，让容量预算真正生效；完全离开视野的层仍走长 idle，
            // 避免相机小幅移动就丢掉整层。
            if (layerId < layerSeen.size() && layerSeen[layerId] && !candidate)
            {
                idleWindow = config.unselectedMipIdleEvictionEpochs;
            }
            // 饥饿补偿等价于“把 idle 时钟暂停”：窗口随饥饿帧数增长，
            // 因此没有 feedback 的帧不会把页推到淘汰阈值。
            if (page.lastSeenEpoch + idleWindow + extraIdleGraceEpochs >=
                seenEpoch)
                continue;
            if (!layerPages.CanEvictPageAt(entry.first, completedEpoch))
                continue;
            idlePages.push_back(entry.first);
            --evictionBudget;
        }
    }
    for (const TerrainLayerPageKey& key : idlePages)
    {
        if (layerPages.EvictPage(key)) ++result.evictedPages;
    }
    while (allowBudgetEviction && evictionBudget != 0 &&
           layerPages.ResidentCount() > layerPages.ResidentCapacity())
    {
        if (!layerPages.EvictLeastRecentlyUsed(completedEpoch)) break;
        ++result.evictedPages;
        --evictionBudget;
    }

    return result;
}

TerrainLayerVisibilityResolver::CoverPageLayerSet
TerrainLayerVisibilityResolver::ExtractCoverPageLayers(
    const std::vector<uint8_t>& ids,
    const std::vector<uint8_t>& weights,
    float minLayerWeight)
{
    CoverPageLayerSet result;
    const size_t texelCount =
        static_cast<size_t>(TerrainPageResolution) * TerrainPageResolution;
    if (ids.size() < texelCount * 4 || weights.size() < texelCount * 4)
        return result;

    // 同一层可能出现在不同 texel 的不同 top-4 槽位，先按层号累加覆盖率。
    std::array<double, 256> coverage{};
    for (size_t texel = 0; texel < texelCount; ++texel)
    {
        for (size_t slot = 0; slot < 4; ++slot)
        {
            const uint8_t weight = weights[texel * 4 + slot];
            if (weight == 0) continue;
            const uint8_t layerId = ids[texel * 4 + slot];
            coverage[layerId] += static_cast<double>(weight) / 255.0;
        }
    }
    // 一个层只覆盖半页时平均覆盖率约 0.5；低于阈值直接丢弃。
    const double texelCountInverse = 1.0 / static_cast<double>(texelCount);
    std::vector<std::pair<float, uint32_t>> sorted;
    sorted.reserve(256);
    for (uint32_t layerId = 0; layerId < 256; ++layerId)
    {
        const float weight = static_cast<float>(
            std::min(1.0, coverage[layerId] * texelCountInverse));
        if (weight < minLayerWeight) continue;
        sorted.emplace_back(weight, layerId);
    }
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const std::pair<float, uint32_t>& lhs,
           const std::pair<float, uint32_t>& rhs)
        {
            if (lhs.first != rhs.first) return lhs.first > rhs.first;
            return lhs.second < rhs.second;
        });
    result.count = static_cast<uint32_t>(std::min<size_t>(sorted.size(), 4));
    for (uint32_t slot = 0; slot < result.count; ++slot)
    {
        result.weights[slot] = sorted[slot].first;
        result.layerIds[slot] = static_cast<uint8_t>(sorted[slot].second);
    }
    result.valid = result.count != 0;
    return result;
}

} // namespace VL
