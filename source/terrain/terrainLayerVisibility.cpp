#include "terrain/terrainLayerVisibility.h"

#include <algorithm>
#include <cmath>

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

} // namespace

void TerrainLayerVisibilityResolver::Reset()
{
    coverPageCache.clear();
}

uint32_t TerrainLayerVisibilityResolver::MipForPriority(uint8_t priority)
{
    if (priority >= 192) return 0;
    if (priority >= 128) return 1;
    if (priority >= 64) return 2;
    return 3;
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
    if (config.layerCount == 0 || config.sourceMipLevels == 0)
        return stats;

    // 1. 覆盖页 -> 可见层集合（带缓存）。非驻留页的重建是高度图烘焙，
    //    每帧限流，避免一次 feedback 触发上百次全页采样。
    std::map<TerrainLayerPageKey, uint32_t> candidates;
    std::array<bool, 256> layerSeen{};
    // 每个可见层本帧拿到的最高优先级；用于给该层所有已驻留 mip 续命，
    // 避免 priority 在 mip 阈值附近抖动时把上一层 mip 的页反复淘汰/重烘焙。
    std::array<uint8_t, 256> layerPriority{};
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
            layerPriority[layerId] = static_cast<uint8_t>(std::min<uint32_t>(
                255u, std::max<uint32_t>(layerPriority[layerId], priority)));
        }
    }
    stats.candidatePages = static_cast<uint32_t>(candidates.size());

    // 2. 本帧可见的常驻页推进 lastSeen/lastTouched；必须在淘汰之前做，
    //    否则刚被采样到的页会在同一帧进入 idle 候选。
    const uint64_t seenEpoch =
        summary.copiedEpoch != 0 ? summary.copiedEpoch : completedEpoch;
    for (const auto& [key, priority] : candidates)
    {
        layerPages.MarkPageUsedAtEpoch(key, seenEpoch, priority);
    }
    // 可见层的其它 mip 也属于“仍在视野内”的页：shader 的近/远像素会分别
    // 采样不同 mip，只给本帧选中的那一级续命会让相邻 mip 无谓抖动。
    for (const auto& entry : layerPages.Pages())
    {
        if (!entry.second.resident) continue;
        const uint32_t layerId = entry.first.layerId;
        if (layerId >= layerSeen.size() || !layerSeen[layerId]) continue;
        layerPages.MarkPageUsedAtEpoch(
            entry.first, seenEpoch, layerPriority[layerId]);
    }

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
            ++stats.attemptedRequests;
            if (layerPages.RequestPageAtEpoch(
                    request.key,
                    frame,
                    request.priority,
                    completedEpoch))
            {
                ++stats.acceptedRequests;
                ++stats.mipRequestCounts[request.key.mip];
                continue;
            }
            ++stats.deferredRequests;
            ++request.deferredRequestCount;
        }
        survivors.push_back(request);
    }
    pendingRequests.swap(survivors);

    // 6. 淘汰：先清掉离开视野很久且已过 GPU 安全窗的层页，再按预算做 LRU。
    //    idle 淘汰使用 lastSeenEpoch（feedback 真正看到它的 epoch），而不是
    //    只升不降的 priority，避免近景页离开视野后永远压住新页。
    std::vector<TerrainLayerPageKey> idlePages;
    if (config.idleEvictionEnabled)
    {
        for (const auto& entry : layerPages.Pages())
        {
            const TerrainLayerPageManager::PageState& page = entry.second;
            if (!page.resident) continue;
            if (page.lastSeenEpoch + config.idleEvictionEpochs >= seenEpoch)
                continue;
            if (!layerPages.CanEvictPageAt(entry.first, completedEpoch))
                continue;
            idlePages.push_back(entry.first);
        }
    }
    for (const TerrainLayerPageKey& key : idlePages)
    {
        if (layerPages.EvictPage(key)) ++stats.evictedPages;
    }
    while (layerPages.ResidentCount() > layerPages.ResidentCapacity())
    {
        if (!layerPages.EvictLeastRecentlyUsed(completedEpoch)) break;
        ++stats.evictedPages;
    }

    stats.pendingRequests =
        static_cast<uint32_t>(pendingRequests.size());
    return stats;
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
