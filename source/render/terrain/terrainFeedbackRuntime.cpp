#include "render/terrain/terrainFeedbackRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "material.h"
#include "materialInstance.h"
#include "renderGraph.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainPageBakeGpu.h"
#include "render/resource/terrainFeedbackGpu.h"
#include "render/resource/terrainLayerFeedbackGpu.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMap.h"
#include "terrain/terrainLayerTable.h"
#include "world/worldSnapshotBuilder.h"

namespace VL
{
void TerrainFeedbackRuntime::RequestTerrainPageBakeParity(int tileX, int tileZ)
{
    pageBakeParityTileX.store(tileX, std::memory_order_relaxed);
    pageBakeParityTileZ.store(tileZ, std::memory_order_relaxed);
    pageBakeParityRequested.store(true, std::memory_order_release);
}

void TerrainFeedbackRuntime::RunTerrainPageBakeParityIfRequested()
{
    if (!pageBakeParityRequested.exchange(
            false, std::memory_order_acquire))
        return;
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming || streaming->pageManager.Registrations().empty())
    {
        std::cout << "[TerrainParity] No registered terrain placement."
                  << std::endl;
        return;
    }
    if (!streaming->pageBake || !streaming->pageBake->Available())
    {
        std::cout << "[TerrainParity] GPU page bake unavailable: "
                  << (streaming->pageBake
                          ? streaming->pageBake->UnavailableReason()
                          : std::string("resource not created"))
                  << std::endl;
        return;
    }

    VL::TerrainPageManager& pageManager = streaming->pageManager;
    const VL::TerrainPageManager::Registration& registration =
        pageManager.Registrations().front();
    int tileX = pageBakeParityTileX.load(std::memory_order_relaxed);
    int tileZ = pageBakeParityTileZ.load(std::memory_order_relaxed);
    if (tileX < 0 || tileZ < 0 ||
        static_cast<uint32_t>(tileX) >= registration.tilesPerSide ||
        static_cast<uint32_t>(tileZ) >= registration.tilesPerSide)
    {
        tileX = 0;
        tileZ = 0;
    }

    try
    {
        const VL::TerrainBuildPlan& plan = pageManager.Plans().front();
        const VL::TerrainPageData cpu =
            VL::TerrainWeightMapBuilder::BuildPage(
                plan,
                registration.tilesPerSide,
                VL::TerrainPageManager::PageResolution,
                static_cast<uint32_t>(tileX),
                static_cast<uint32_t>(tileZ),
                pageManager.PlacementLayerIdMap(plan.placementId.empty()
                    ? plan.desc.assetPath : plan.placementId));

        std::vector<uint8_t> gpuIds;
        std::vector<uint8_t> gpuWeights;
        std::string error;
        if (!streaming->pageBake->BakePageSync(
                pageManager,
                0,
                static_cast<uint32_t>(tileX),
                static_cast<uint32_t>(tileZ),
                gpuIds,
                gpuWeights,
                error))
        {
            std::cout << "[TerrainParity] GPU bake failed: " << error
                      << std::endl;
            return;
        }

        size_t idMismatchBytes = 0;
        size_t weightMismatchBytes = 0;
        size_t mismatchedTexels = 0;
        uint32_t worstIdDiff = 0;
        uint32_t worstWeightDiff = 0;
        const size_t texelCount =
            static_cast<size_t>(VL::TerrainPageManager::PageResolution) *
            VL::TerrainPageManager::PageResolution;
        if (cpu.ids.size() != gpuIds.size() ||
            cpu.weights.size() != gpuWeights.size())
        {
            std::cout << "[TerrainParity] Size mismatch cpuIds="
                      << cpu.ids.size() << " gpuIds=" << gpuIds.size()
                      << " cpuWeights=" << cpu.weights.size()
                      << " gpuWeights=" << gpuWeights.size() << std::endl;
            return;
        }
        for (size_t index = 0; index < cpu.ids.size(); ++index)
        {
            const uint32_t idDiff = static_cast<uint32_t>(
                std::abs(static_cast<int>(cpu.ids[index]) -
                         static_cast<int>(gpuIds[index])));
            const uint32_t weightDiff = static_cast<uint32_t>(
                std::abs(static_cast<int>(cpu.weights[index]) -
                         static_cast<int>(gpuWeights[index])));
            if (idDiff != 0) ++idMismatchBytes;
            if (weightDiff != 0) ++weightMismatchBytes;
            worstIdDiff = std::max(worstIdDiff, idDiff);
            worstWeightDiff = std::max(worstWeightDiff, weightDiff);
        }
        for (size_t texel = 0; texel < texelCount; ++texel)
        {
            const size_t base = texel * 4;
            bool mismatch = false;
            for (size_t channel = 0; channel < 4; ++channel)
            {
                if (cpu.ids[base + channel] != gpuIds[base + channel] ||
                    cpu.weights[base + channel] != gpuWeights[base + channel])
                {
                    mismatch = true;
                    break;
                }
            }
            if (mismatch) ++mismatchedTexels;
        }

        size_t paintedTexels = 0;
        if (plan.layerPaint && !plan.layerPaint->Empty())
        {
            for (uint8_t opacity : plan.layerPaint->opacity)
            {
                if (opacity != 0) ++paintedTexels;
            }
        }
        std::cout << "[TerrainParity] placement=" << registration.placementId
                  << " tile=" << tileX << "," << tileZ
                  << " layers=" << plan.desc.layers.size()
                  << " paintNonZeroTexels=" << paintedTexels
                  << " cpuBakeCandidates=" << cpu.candidates.layerIds.size()
                  << std::endl;
        std::cout << "[TerrainParity] idMismatchBytes=" << idMismatchBytes
                  << " weightMismatchBytes=" << weightMismatchBytes
                  << " mismatchedTexels=" << mismatchedTexels
                  << "/" << texelCount
                  << " worstIdDiff=" << worstIdDiff
                  << " worstWeightDiff=" << worstWeightDiff
                  << " result="
                  << ((idMismatchBytes == 0 && weightMismatchBytes == 0)
                          ? "PASS" : "FAIL")
                  << std::endl;
    }
    catch (const std::exception& exception)
    {
        std::cout << "[TerrainParity] Exception: " << exception.what()
                  << std::endl;
    }
}

void TerrainFeedbackRuntime::ConsumeTerrainFeedback(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    uint64_t completedEpoch,
    uint32_t frameIndex)
{
    if (!streaming || !streaming->feedbackRing)
        return;

    // BeginFrame 已经等待过该 frameIndex 的 fence；TryConsume 再用 epoch
    // 复核一次，避免 swapchain/frame ring 重建后误读尚未完成的槽。
    std::optional<VL::TerrainFeedbackFrameSummary> summary =
        streaming->feedbackRing->TryConsume(frameIndex, completedEpoch);
    if (!summary.has_value())
        return;

    streaming->lastConsumedFeedbackEpoch = std::max(
        streaming->lastConsumedFeedbackEpoch,
        summary->copiedEpoch);
    // P3：feedback -> 命中页推进 lastSeenEpoch；miss 页按 priority 进入请求队列。
    // 请求在本帧 BuildUploadBatch 之前完成，所以同帧就能把新页写进 GPU。
    streaming->latestFeedbackApply = streaming->pageManager.ApplyFeedback(
        *summary,
        completedEpoch,
        streaming->maxPageRequestsPerFrame,
        streaming->pendingPageRequests);
    // L4b：层号+层页+mip 的专用 feedback 是主路径；只有它本帧不可用时才
    // 退回 L4a 的覆盖页推导（启动/首帧/backpressure）。
    bool appliedExactLayerFeedback = false;
    if (streaming->layerVTRequested && streaming->layerFeedbackRing)
    {
        const std::optional<VL::TerrainLayerFeedbackFrameSummary>
            layerSummary = streaming->layerFeedbackRing->TryConsume(
                frameIndex,
                completedEpoch);
        if (layerSummary.has_value())
        {
            appliedExactLayerFeedback = ApplyTerrainLayerExactFeedback(
                streaming,
                *layerSummary,
                completedEpoch,
                frameIndex);
        }
    }
    if (!appliedExactLayerFeedback)
    {
        // L4a：覆盖页推导只作为回退/prefetch，不再与精确路径同时写驻留。
        ApplyTerrainLayerFeedback(
            streaming, *summary, completedEpoch, frameIndex);
    }
    StoreTerrainFeedbackSummary(streaming, *summary);
}

void TerrainFeedbackRuntime::ApplyTerrainLayerFeedback(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    const VL::TerrainFeedbackFrameSummary& summary,
    uint64_t completedEpoch,
    uint32_t frameIndex)
{
    if (!streaming || !streaming->layerVTRequested)
        return;
    if (!streaming->layerResources.atlasesCreated ||
        streaming->layerDefinitions.empty())
        return;

    VL::TerrainLayerVisibilityConfig config;
    config.layerCount =
        static_cast<uint32_t>(streaming->layerDefinitions.size());
    // 三张源数组的 mip 链可能不同，只请求三张都能 copy 的级别。
    const uint32_t sourceMipLevels =
        VL::TerrainSourceMinMipLevels(streaming->layerSources);
    config.sourceMipLevels = std::max(1u, sourceMipLevels);
    config.sourceSets = &streaming->layerSources;
    config.maxRequestsPerFrame = streaming->maxLayerPageRequestsPerFrame;
    config.idleEvictionEpochs = streaming->layerIdleEvictionEpochs;
    config.idleEvictionEnabled =
        streaming->layerIdleEvictionEnabled.load(std::memory_order_relaxed);

    streaming->latestLayerVisibility =
        streaming->layerVisibilityResolver.Apply(
            summary,
            streaming->pageManager,
            streaming->layerPageManager,
            streaming->pendingLayerPageRequests,
            frameIndex,
            completedEpoch,
            config);
    streaming->latestLayerFeedbackExact = false;
}

bool TerrainFeedbackRuntime::ApplyTerrainLayerExactFeedback(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    const VL::TerrainLayerFeedbackFrameSummary& summary,
    uint64_t completedEpoch,
    uint32_t frameIndex)
{
    if (!streaming || !streaming->layerVTRequested)
        return false;
    if (!streaming->layerResources.atlasesCreated ||
        streaming->layerDefinitions.empty())
        return false;
    // 精确 feedback 本帧没有任何有效 texel（例如地形完全不在屏幕内）时
    // 仍要走精确路径：空集合表示“没有可见层”，不能退回覆盖页推导去
    // 复活整层页。
    VL::TerrainLayerVisibilityConfig config;
    config.layerCount =
        static_cast<uint32_t>(streaming->layerDefinitions.size());
    const uint32_t sourceMipLevels =
        VL::TerrainSourceMinMipLevels(streaming->layerSources);
    config.sourceMipLevels = std::max(1u, sourceMipLevels);
    config.sourceSets = &streaming->layerSources;
    config.maxRequestsPerFrame = streaming->maxLayerPageRequestsPerFrame;
    config.idleEvictionEpochs = streaming->layerIdleEvictionEpochs;
    config.idleEvictionEnabled =
        streaming->layerIdleEvictionEnabled.load(std::memory_order_relaxed);
    // L4e：相机前向移动时预取细一级 mip 的子页，使用独立每帧名额。
    config.prefetchFinerMipEnabled = streaming->layerPrefetchMoving;
    config.prefetchPagesPerFrame = streaming->maxLayerPrefetchPagesPerFrame;
    // 全局页预算：以层页管理器的驻留容量为上限做 mip 分配。
    //
    // 契约要求“选定 mip 后必须请求该 mip 的全部页”，所以每层页数是 mip 的
    // 离散函数。不给上限时，每个 tile 的每个层都按屏幕像素独立选 mip，
    // 多 tile 场景的总页数可达容量的数倍，淘汰永远追不上请求，每帧
    // 请求->淘汰->再请求 永不收敛。分配器按屏幕覆盖优先级（近处优先）
    // 细化，并保证去重后的完整页集总页数 <= 容量（工作集本身超容量时
    // 保持最粗集并允许稳定超预算）。
    config.layerPageBudget = streaming->layerPageManager.ResidentCapacity();

    streaming->latestLayerExactFeedback =
        streaming->layerVisibilityResolver.ApplyExact(
            summary,
            streaming->layerPageManager,
            streaming->pendingLayerPageRequests,
            frameIndex,
            completedEpoch,
            config);
    streaming->latestLayerFeedbackExact = true;
    // 饥饿帧（没有任何可信的有效 texel）不能当作“全部不可见”：驻留集在
    // resolver 内保持不变，这里返回 false 让调用方继续走覆盖页推导（L4a），
    // 用覆盖页反馈重新估计可见层，等覆盖页恢复后精确路径自动接管。
    return !streaming->latestLayerExactFeedback.starved;
}

void TerrainFeedbackRuntime::PrepareTerrainFeedbackPass(
    uint8_t frameTag,
    vk::Extent2D renderExtent)
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    const bool layerFeedbackEnabled =
        streaming && streaming->layerVTRequested &&
        streaming->layerResources.atlasesCreated;
    const RenderGraph& renderGraph = RenderGraph::GetInstance();
    for (const auto& [passName, renderpass] :
         renderGraph.GetRenderpasses())
    {
        (void)passName;
        if (renderpass.type != VL::RenderGraphPassType::TerrainFeedback)
            continue;
        const std::shared_ptr<MaterialInstance> passMaterial =
            renderpass.materialInstance.lock();
        if (!passMaterial)
            continue;
        // 这里只写 MI 参数；真正的 UBO 上传发生在 feedback pass 录制时。
        if (passMaterial->HasParameter("u_pageTableSize"))
        {
            passMaterial->SetParameter(
                "u_pageTableSize",
                static_cast<float>(VL::TerrainPageManager::PageTableSize));
        }
        if (passMaterial->HasParameter("u_frameTag"))
        {
            passMaterial->SetParameter(
                "u_frameTag",
                static_cast<float>(frameTag));
        }
        // 与 M_terrain 共用同一半 texel 内缩；分辨率必须来自 page manager。
        if (passMaterial->HasParameter("u_pageResolution"))
        {
            passMaterial->SetParameter(
                "u_pageResolution",
                static_cast<float>(VL::TerrainPageManager::PageResolution));
        }
        // L4b：层页 feedback 与 M_terrain 共享同一套层 UV/页公式参数。
        if (passMaterial->HasParameter("u_uvScale") && streaming)
        {
            passMaterial->SetParameter(
                "u_uvScale",
                streaming->layerUvScale);
        }
        if (passMaterial->HasParameter("u_layerPageResolution"))
        {
            passMaterial->SetParameter(
                "u_layerPageResolution",
                static_cast<float>(
                    VL::TerrainLayerPageManager::PageResolution));
        }
        if (passMaterial->HasParameter("u_layerMaxMip"))
        {
            passMaterial->SetParameter(
                "u_layerMaxMip",
                static_cast<float>(
                    VL::TerrainLayerPageManager::MaxMipLevel));
        }
        // L4e：feedback 与 M_terrain 必须报告同一目标 mip；console 的
        // forcemip 两边都从这里取值。
        if (passMaterial->HasParameter("u_layerForceMip") && streaming)
        {
            passMaterial->SetParameter(
                "u_layerForceMip",
                static_cast<float>(
                    streaming->layerForceMip.load(
                        std::memory_order_relaxed)));
        }
        if (passMaterial->HasParameter("u_layerFeedbackEnabled"))
        {
            passMaterial->SetParameter(
                "u_layerFeedbackEnabled",
                layerFeedbackEnabled ? 1.0f : 0.0f);
        }
        // feedback pass 固定 128×128，而 M_terrain 在全屏分辨率上取导数。
        // 不换算的话 feedback 的每像素变化被放大 screen/128 倍，mip 会系统性
        // 选粗（1280 宽时约 3.3 级），层页驻留就会欠配。
        
        if (passMaterial->HasParameter("u_feedbackScaleX"))
        {
            passMaterial->SetParameter(
                "u_feedbackScaleX",
                static_cast<float>(std::max(1u, renderExtent.width)) /
                    static_cast<float>(VL::TerrainFeedbackResolution));
        }
        if (passMaterial->HasParameter("u_feedbackScaleY"))
        {
            passMaterial->SetParameter(
                "u_feedbackScaleY",
                static_cast<float>(std::max(1u, renderExtent.height)) /
                    static_cast<float>(VL::TerrainFeedbackResolution));
        }
    }
}

void TerrainFeedbackRuntime::StoreTerrainFeedbackSummary(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    const VL::TerrainFeedbackFrameSummary& summary)
{
    TerrainFeedbackReport report;
    report.summary = summary;

    if (streaming && streaming->feedbackRing)
    {
        report.hasPendingFrameTagRange =
            streaming->feedbackRing->PendingTagRange(
                report.oldestPendingFrameTag,
                report.newestPendingFrameTag);
    }

    std::lock_guard<std::mutex> lock(statisticsMutex);
    latestReport = std::move(report);
}

void TerrainFeedbackRuntime::ReportTerrainFeedbackStatistics() const
{
    TerrainFeedbackReport report;
    {
        std::lock_guard<std::mutex> lock(statisticsMutex);
        if (!latestReport.has_value())
        {
            std::cout
                << "[TerrainFeedback] No completed feedback frame yet."
                << std::endl;
            return;
        }
        report = *latestReport;
    }

    const VL::TerrainFeedbackFrameSummary& summary = report.summary;
    std::cout
        << "[TerrainFeedback] epoch=" << summary.copiedEpoch
        << " tag=" << static_cast<uint32_t>(summary.frameTag)
        << " validTexels=" << summary.validTexelCount
        << " distinctPages=" << summary.distinctPageCount
        << " missPages=" << summary.missPageCount
        << " maxPriority=" << static_cast<uint32_t>(summary.maxPriority)
        << " staleTexels=" << summary.staleTexelCount;
    if (report.hasPendingFrameTagRange)
    {
        std::cout
            << " pendingTags=["
            << static_cast<uint32_t>(report.oldestPendingFrameTag)
            << ".."
            << static_cast<uint32_t>(report.newestPendingFrameTag)
            << "]";
    }
    std::cout << std::endl;

    // 只打印前几个高优先级页，避免 32×32 全量刷屏；miss 页单独标出。
    const size_t pageCount =
        std::min<size_t>(summary.pages.size(), 8);
    for (size_t index = 0; index < pageCount; ++index)
    {
        const VL::TerrainFeedbackPageRecord& page =
            summary.pages[index];
        std::cout
            << "  page(" << page.virtualPageX << ","
            << page.virtualPageY << ")"
            << " priority=" << static_cast<uint32_t>(page.priority)
            << " miss=" << (page.miss ? 1 : 0)
            << " samples=" << page.sampleCount
            << std::endl;
    }
}

void TerrainFeedbackRuntime::RequestTerrainResidentCapacity(uint32_t capacity)
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[Terrain] No active terrain streaming state."
            << std::endl;
        return;
    }
    streaming->RequestResidentCapacity(capacity);
    std::cout
        << "[Terrain] Resident capacity request queued: "
        << capacity << std::endl;
}

void TerrainFeedbackRuntime::RequestTerrainLayerResidentCapacity(uint32_t capacity)
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[TerrainLayer] No active terrain streaming state."
            << std::endl;
        return;
    }
    streaming->requestedLayerResidentCapacity.store(
        capacity, std::memory_order_relaxed);
    // 设置预算意味着回到 streaming 语义：重新启用 idle 淘汰。
    streaming->layerIdleEvictionEnabled.store(true, std::memory_order_relaxed);
    std::cout
        << "[TerrainLayer] Resident capacity request queued: "
        << capacity << std::endl;
}

void TerrainFeedbackRuntime::RequestTerrainLayerPreload()
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[TerrainLayer] No active terrain streaming state."
            << std::endl;
        return;
    }
    // 对照路径：请求全部层页；像素仍由逐帧上传 ring 分批写进 atlas。
    streaming->requestedLayerPreload.store(true, std::memory_order_relaxed);
    // 关闭 idle 淘汰，否则非可见层会在 3 秒后被清掉，失去对照意义。
    streaming->layerIdleEvictionEnabled.store(false, std::memory_order_relaxed);
    std::cout
        << "[TerrainLayer] Full layer preload request queued "
        << "(idle eviction disabled until 'terrainlayer capacity N')."
        << std::endl;
}

void TerrainFeedbackRuntime::RequestTerrainLayerForceMip(int forceMip)
{
    if (forceMip < -1 || forceMip > 4)
    {
        std::cout
            << "[TerrainLayer] forceMip must be -1 (auto) or 0..4."
            << std::endl;
        return;
    }
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[TerrainLayer] No active terrain streaming state."
            << std::endl;
        return;
    }
    // 只写 atomic；渲染线程帧首把值同步到 M_terrain 与 feedback pass。
    streaming->layerForceMip.store(forceMip, std::memory_order_relaxed);
    std::cout
        << "[TerrainLayer] Forced target mip: "
        << (forceMip < 0 ? std::string("auto") : std::to_string(forceMip))
        << std::endl;
}

void TerrainFeedbackRuntime::ApplyTerrainLayerForceMip(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming)
{
    if (!streaming || !streaming->layerVTRequested)
        return;
    const float forceMip = static_cast<float>(
        streaming->layerForceMip.load(std::memory_order_relaxed));
    // 只在值变化或有新实例登记时写入：SetParameter 会标脏材质 UBO。
    if (forceMip == streaming->lastAppliedLayerForceMip &&
        streaming->layerVTMaterials.size() ==
            streaming->lastAppliedLayerMaterialCount)
    {
        return;
    }
    for (const std::weak_ptr<MaterialInstance>& weakMaterial :
         streaming->layerVTMaterials)
    {
        const std::shared_ptr<MaterialInstance> material = weakMaterial.lock();
        if (material && material->HasParameter("u_layerForceMip"))
            material->SetParameter("u_layerForceMip", forceMip);
    }
    streaming->lastAppliedLayerForceMip = forceMip;
    streaming->lastAppliedLayerMaterialCount =
        streaming->layerVTMaterials.size();
}

void TerrainFeedbackRuntime::UpdateTerrainCameraPrefetch(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    const Eigen::Vector3f& cameraPosition,
    const Eigen::Vector3f& cameraForward)
{
    if (!streaming)
        return;
    if (!streaming->layerVTRequested)
    {
        streaming->layerPrefetchMoving = false;
        return;
    }
    const Eigen::Vector3f position = cameraPosition;
    if (!hasPrefetchCameraPosition)
    {
        lastPrefetchCameraPosition = position;
        hasPrefetchCameraPosition = true;
        streaming->layerPrefetchMoving = false;
        return;
    }
    const Eigen::Vector3f delta = position - lastPrefetchCameraPosition;
    lastPrefetchCameraPosition = position;
    const float distance = delta.norm();
    // 只有“向前走”才提高细一级 mip 的需求：横向平移/转头不会让目标
    // mip 变细，预取反而会和当前可见页抢槽位。
    const float forwardDistance =
        delta.dot(cameraForward);
    streaming->layerPrefetchMoving =
        distance > streaming->layerPrefetchMinForwardCm &&
        forwardDistance > 0.5f * distance;
}

void TerrainFeedbackRuntime::PublishTerrainStreamingStats(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    uint64_t completedEpoch)
{
    if (!streaming)
        return;
    VL::TerrainStreamingStats stats;
    stats.statsPublished = true;
    VL::TerrainPageManager& pageManager = streaming->pageManager;
    // 统计刷新时顺带回收已过隔离期的槽位：free/quarantine 必须反映当前
    // completedEpoch，而不是“等下一次分配才更新”的惰性值。
    pageManager.ReclaimExpiredQuarantines(completedEpoch);
    stats.residentPages = pageManager.ResidentCount();
    stats.residentCapacity = pageManager.ResidentCapacity();
    stats.virtualPages = static_cast<uint32_t>(pageManager.Pages().size());
    stats.registrations =
        static_cast<uint32_t>(pageManager.Registrations().size());
    stats.freePhysicalSlots =
        pageManager.ImmediatelyAllocatablePhysicalSlotCount();
    stats.quarantinedPhysicalSlots =
        pageManager.QuarantinedPhysicalSlotCount();
    stats.requestCount = pageManager.RequestCount();
    stats.hitCount = pageManager.PageHitCount();
    stats.missCount = pageManager.PageMissCount();
    stats.evictionCount = pageManager.EvictionCount();
    stats.preloadDeferredCount = pageManager.PreloadDeferredCount();
    stats.uploadedPagesTotal = streaming->uploadedPagesTotal;
    stats.lastUploadedPages = streaming->lastUploadedPages;
    stats.gpuBakedPagesTotal = streaming->gpuBakedPagesTotal;
    stats.lastGpuBakedPages = streaming->lastGpuBakedPages;
    stats.pendingGpuBakeJobs =
        static_cast<uint32_t>(pageManager.PendingGpuBakeJobCount());
    stats.lastFeedbackEpoch = streaming->lastConsumedFeedbackEpoch;
    stats.lastFeedbackApply = streaming->latestFeedbackApply;
    // L2 层页：与覆盖页完全独立的一组驻留/上传计数。
    VL::TerrainLayerPageManager& layerPages = streaming->layerPageManager;
    // 与覆盖页一致：统计必须反映当前 completedEpoch，而不是等下次分配
    // 才惰性回收隔离槽位。
    layerPages.ReclaimExpiredQuarantines(completedEpoch);
    const VL::TerrainLayerPageManager::Stats layerStats =
        layerPages.SnapshotStats();
    stats.layerResidentPages = layerStats.residentPageCount;
    stats.layerResidentCapacity = layerStats.residentCapacity;
    stats.layerPhysicalPages = layerPages.PhysicalPageCount();
    stats.layerFreePhysicalPages = layerStats.freePhysicalPageCount;
    stats.layerQuarantinedPhysicalPages =
        layerStats.quarantinedPhysicalPageCount;
    stats.layerRequestCount = layerStats.requestCount;
    stats.layerHitCount = layerStats.hitCount;
    stats.layerMissCount = layerStats.missCount;
    stats.layerEvictionCount = layerStats.evictionCount;
    stats.layerUploadedPagesTotal = streaming->layerUploadedPagesTotal;
    stats.layerUploadedBytesTotal = streaming->layerUploadedBytesTotal;
    // S1/S2：全局层数 / sourceSet 数 / 去重掉的局部层数。
    stats.layerGlobalLayers =
        static_cast<uint32_t>(streaming->layerDefinitions.size());
    stats.layerSourceSetCount =
        static_cast<uint32_t>(streaming->layerSources.size());
    stats.layerDedupedLayers = streaming->layerDedupedLayerCount;
    // L4a：可见层推导与逐帧预算的实时值。
    stats.layerVisibleLayers =
        streaming->latestLayerVisibility.visibleLayers;
    if (streaming->latestLayerFeedbackExact)
    {
        // L4b 精确路径：命中/请求/挂起都来自层号+层页+mip 的专用 feedback。
        const VL::TerrainLayerExactFeedbackStats& exact =
            streaming->latestLayerExactFeedback;
        stats.layerVisibleLayers = exact.visibleLayers;
        stats.layerFeedbackHits = exact.markedPages;
        stats.layerRequestedPages = exact.attemptedRequests;
        stats.layerAcceptedPages = exact.acceptedRequests;
        stats.layerPendingRequests = exact.pendingRequests;
        stats.layerMipRequestCounts = exact.mipRequestCounts;
        stats.layerExactFeedbackSlots = exact.validSlots;
        stats.layerExactFeedbackPages = exact.distinctPages;
        // L4c：粗 mip 回退的估算输入（比例由控制台换成像素数）。
        stats.layerFallbackSlots = exact.fallbackSlots;
        stats.layerFallbackTargetPages = exact.fallbackTargetPages;
        stats.layerFallbackLevelPages = exact.fallbackLevelPages;
        stats.layerTargetMipSlots = exact.targetMipSlots;
        // L4e：区分“有粗页可混”和“只能 avgColor”，以及相机前向预取量。
        stats.layerFallbackBlendPages = exact.fallbackBlendPages;
        stats.layerAvgColorPages = exact.avgColorPages;
        stats.layerPrefetchPages = exact.prefetchPages;
        stats.layerPrefetchRequests = exact.prefetchRequests;
        stats.layerPrefetchAccepted = exact.prefetchAccepted;
        stats.layerPageBudget = layerPages.ResidentCapacity();
        stats.layerBudgetPages = exact.budgetTotalPages;
        stats.layerBudgetSets = exact.budgetAllocatedSets;
        stats.layerBudgetClampedRefinements =
            exact.budgetClampedRefinements;
        stats.layerBudgetExceeded = exact.budgetExceeded;
        stats.layerBudgetEntriesByMip = exact.budgetEntriesByMip;
        stats.layerBudgetSetsByMip = exact.budgetSetsByMip;
    }
    else
    {
        const VL::TerrainLayerVisibilityStats& cover =
            streaming->latestLayerVisibility;
        stats.layerFeedbackHits = cover.markedPages;
        stats.layerRequestedPages = cover.attemptedRequests;
        stats.layerAcceptedPages = cover.acceptedRequests;
        stats.layerPendingRequests = cover.pendingRequests;
        stats.layerMipRequestCounts = cover.mipRequestCounts;
        stats.layerExactFeedbackSlots = 0;
        stats.layerExactFeedbackPages = 0;
        stats.layerFallbackSlots = 0;
        stats.layerFallbackTargetPages = cover.fallbackTargetPages;
        stats.layerFallbackLevelPages = cover.fallbackLevelPages;
        stats.layerTargetMipSlots = {};
        // 覆盖页路径没有精确的“目标 mip 页”信息，混合分类保持 0；
        // 预取量同样只由精确路径统计。
        stats.layerFallbackBlendPages = 0;
        stats.layerAvgColorPages = 0;
        stats.layerPrefetchPages = 0;
        stats.layerPrefetchRequests = 0;
        stats.layerPrefetchAccepted = 0;
        // 覆盖页回退路径不做需求级预算分配；控制台必须显示 0，而不是沿用
        // 上一帧精确路径的旧值。
        stats.layerPageBudget = 0;
        stats.layerBudgetPages = 0;
        stats.layerBudgetSets = 0;
        stats.layerBudgetClampedRefinements = 0;
        stats.layerBudgetExceeded = false;
        stats.layerBudgetEntriesByMip = {};
        stats.layerBudgetSetsByMip = {};
    }
    stats.layerLastUploadedPages = streaming->lastLayerUploadedPages;
    stats.layerDirtyPages = layerPages.DirtyPageCount();
    stats.layerDirtyPageTableEntries =
        layerPages.DirtyPageTableEntryCount();
    stats.layerAtlasSlices = streaming->layerResources.atlasSlices;
    stats.layerAtlasBytesPerMap =
        streaming->layerResources.atlasBytesPerMap;
    stats.layerAtlasBytesTotal =
        streaming->layerResources.atlasBytesTotal;
    stats.layerAtlasReady = streaming->layerResources.atlasesCreated;
    // 饥饿状态直接来自 resolver 的跨帧健康记录：精确路径与覆盖页回退
    // 都会写同一份状态，控制台读数与实际驻留策略一致。
    const VL::TerrainLayerFeedbackHealth& feedbackHealth =
        streaming->layerVisibilityResolver.FeedbackHealth();
    stats.layerStarvedFrames = feedbackHealth.starvedFrames;
    stats.layerLastValidFeedbackEpoch =
        feedbackHealth.lastValidFeedbackEpoch;
    stats.layerUsedCoverFallback =
        feedbackHealth.starvedFrames != 0;
    stats.layerIdleGraceEpochs = feedbackHealth.starvationCreditEpochs;
    stats.layerForceMip = static_cast<float>(
        streaming->layerForceMip.load(std::memory_order_relaxed));
    stats.layerPrefetchMoving = streaming->layerPrefetchMoving;
    streaming->PublishStats(stats);
    // 本帧上传数只显示一帧；累计值继续保留。
    streaming->lastUploadedPages = 0;
    streaming->lastLayerUploadedPages = 0;
}

void TerrainFeedbackRuntime::ReportTerrainStatistics()
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[Terrain] No active terrain streaming state."
            << std::endl;
        return;
    }

    const VL::TerrainStreamingStats stats = streaming->SnapshotStats();
    const VL::TerrainFeedbackApplyStats& apply = stats.lastFeedbackApply;
    std::cout
        << "[Terrain] resident=" << stats.residentPages
        << "/" << stats.residentCapacity
        << " virtualPages=" << stats.virtualPages
        << " registrations=" << stats.registrations
        << " freeSlots=" << stats.freePhysicalSlots
        << " quarantined=" << stats.quarantinedPhysicalSlots
        << " requests=" << stats.requestCount
        // requestHits 只统计 RequestPage 命中；feedbackHits 才是 GPU 可见页命中。
        << " requestHits=" << stats.hitCount
        << " misses=" << stats.missCount
        << " evictions=" << stats.evictionCount
        << " preloadDeferred=" << stats.preloadDeferredCount
        << " uploadedPages=" << stats.uploadedPagesTotal
        << " lastUploaded=" << stats.lastUploadedPages
        << " gpuBakedPages=" << stats.gpuBakedPagesTotal
        << " lastGpuBaked=" << stats.lastGpuBakedPages
        << " gpuBakePending=" << stats.pendingGpuBakeJobs
        << " lastFeedbackEpoch=" << stats.lastFeedbackEpoch
        << std::endl;
    std::cout
        << "[Terrain] feedbackHits=" << apply.markedPages
        << " missPages=" << apply.missPagesSeen
        << " queued=" << apply.queuedRequests
        << " attempted=" << apply.attemptedRequests
        << " accepted=" << apply.succeededRequests
        << " deferred=" << apply.deferredRequests
        << " unregistered=" << apply.unregisteredPages
        << " pending=" << apply.pendingRequests
        << std::endl;
}

void TerrainFeedbackRuntime::ReportTerrainLayerStatistics(
    vk::Extent2D renderExtent)
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[TerrainLayer] No active terrain streaming state."
            << std::endl;
        return;
    }
    const VL::TerrainStreamingStats stats = streaming->SnapshotStats();
    // 首帧之前渲染线程还没发布快照，整行 0 会被误读成“没有驻留页”。
    if (!stats.statsPublished)
    {
        std::cout
            << "[TerrainLayer] stats not published yet."
            << std::endl;
        return;
    }
    const VL::TerrainLayerGpuResources& resources =
        streaming->layerResources;
    std::cout
        << "[TerrainLayer] resident=" << stats.layerResidentPages
        << "/" << stats.layerResidentCapacity
        << " physical=" << stats.layerPhysicalPages
        << " free=" << stats.layerFreePhysicalPages
        << " quarantined=" << stats.layerQuarantinedPhysicalPages
        << " requests=" << stats.layerRequestCount
        // 旧 `hits` 是 RequestPage 命中已驻留页的次数；调用方在请求前就会
        // 过滤常驻页，因此它恒为 0。真正的命中看第二行 feedbackHits。
        << " misses=" << stats.layerMissCount
        << " evictions=" << stats.layerEvictionCount
        << " uploadedPages=" << stats.layerUploadedPagesTotal
        << " uploadedBytes=" << stats.layerUploadedBytesTotal
        << " dirtyPages=" << stats.layerDirtyPages
        << " dirtyPageTable=" << stats.layerDirtyPageTableEntries
        << std::endl;
    // 饥饿保护：starvedFrames 是连续没有有效精确 feedback 的帧数；
    // usedCoverFallback=1 表示本帧可见层来自覆盖页推导；idleGrace 是
    // 当前加到 idle 窗口上的补偿 epoch（饥饿期间随帧数增长并封顶）。
    std::cout
        << "[TerrainLayer] starvedFrames=" << stats.layerStarvedFrames
        << " lastValidEpoch=" << stats.layerLastValidFeedbackEpoch
        << " usedCoverFallback=" << (stats.layerUsedCoverFallback ? 1 : 0)
        << " idleGrace=" << stats.layerIdleGraceEpochs
        << std::endl;
    // S1/S2：全局层 / sourceSet / 去重统计。globalLayers 是 catalog 的真实
    // 层数（globalId 0..N-1）；deduped 是跨 TR 复用同一个 key 省下的层数。
    std::cout
        << "[TerrainLayer] globalLayers=" << stats.layerGlobalLayers
        << " sourceSets=" << stats.layerSourceSetCount
        << " dedupedLayers=" << stats.layerDedupedLayers
        << " capacity=" << VL::TerrainLayerTable::MaxLayers
        << std::endl;
    // L4a：可见层与每帧预算。mip0..4 是本帧成功请求的页数分布。
    std::cout
        << "[TerrainLayer] visibleLayers=" << stats.layerVisibleLayers
        // feedbackHits 才是“本帧 GPU 真的看到并续命的层页”；requested 是
        // 本帧尝试请求数（含失败/deferred），accepted 是其中成功的部分。
        << " feedbackHits=" << stats.layerFeedbackHits
        << " exactSlots=" << stats.layerExactFeedbackSlots
        << " exactPages=" << stats.layerExactFeedbackPages
        << " requested=" << stats.layerRequestedPages
        << " accepted=" << stats.layerAcceptedPages
        << " uploadedLast=" << stats.layerLastUploadedPages
        << " pending=" << stats.layerPendingRequests
        << " mipRequests=";
    for (uint32_t mip = 0;
         mip <= VL::TerrainLayerPageManager::MaxMipLevel;
         ++mip)
    {
        if (mip != 0) std::cout << "/";
        std::cout << stats.layerMipRequestCounts[mip];
    }
    std::cout << std::endl;
    // L4c：粗 mip 回退。exactSlots 是有效 feedback texel 数，fallbackSlots 是
    // 其中目标 mip 缺页的部分（shader 会退到更粗 mip；若没有更粗页则显示
    // avgColor）。因此 fallbackSlots/exactSlots × 渲染分辨率是“本帧会走
    // 回退路径的像素数”的上界估计；fallbackLevels=+1/+2/+3 是本帧新增的
    // 粗 mip 预取页数；targetMips 是 feedback texel 的目标 mip 分布。
    const vk::Extent2D fallbackExtent = renderExtent;
    const double fallbackPixels =
        stats.layerExactFeedbackSlots == 0
            ? 0.0
            : static_cast<double>(stats.layerFallbackSlots) /
                static_cast<double>(stats.layerExactFeedbackSlots) *
                static_cast<double>(fallbackExtent.width) *
                static_cast<double>(fallbackExtent.height);
    std::cout
        << "[TerrainLayer] fallbackSlots=" << stats.layerFallbackSlots
        << " fallbackPages=" << stats.layerFallbackTargetPages
        << " fallbackLevels=" << stats.layerFallbackLevelPages[0] << "/"
        << stats.layerFallbackLevelPages[1] << "/"
        << stats.layerFallbackLevelPages[2]
        << " fallbackPixels~=" << static_cast<uint64_t>(fallbackPixels)
        << " targetMips=";
    for (uint32_t mip = 0;
         mip <= VL::TerrainLayerPageManager::MaxMipLevel;
         ++mip)
    {
        if (mip != 0) std::cout << "/";
        std::cout << stats.layerTargetMipSlots[mip];
    }
    std::cout
        << std::endl;
    std::cout
        << "[TerrainLayer] atlasReady=" << (stats.layerAtlasReady ? 1 : 0)
        << " slices=" << stats.layerAtlasSlices
        << " bytesPerMap=" << stats.layerAtlasBytesPerMap
        << " atlasBytes=" << stats.layerAtlasBytesTotal
        << " albedo=" << vk::to_string(resources.albedoFormat)
        << " normal=" << vk::to_string(resources.normalFormat)
        << " pbr=" << vk::to_string(resources.pbrFormat)
        << std::endl;
    // L4e：强制 mip 与相机前向预取。fallbackBlendPages 表示目标页缺页但
    // 有更粗驻留页（shader 按 mip 差渐变）；avgColorPages 表示连粗页都
    // 没有，整页显示 avgColor。prefetchMoving=1 才会产生 prefetchRequests。
    std::cout
        << "[TerrainLayer] forceMip=" << stats.layerForceMip
        << " prefetchMoving=" << (stats.layerPrefetchMoving ? 1 : 0)
        << " prefetchPages=" << stats.layerPrefetchPages
        << " prefetchRequests=" << stats.layerPrefetchRequests
        << " prefetchAccepted=" << stats.layerPrefetchAccepted
        << " fallbackBlendPages=" << stats.layerFallbackBlendPages
        << " avgColorPages=" << stats.layerAvgColorPages
        << std::endl;
    // 全局预算：pages/sets 是去重后的 (layer,mip) 全页集，必须 <= pageBudget
    // 才能保证请求不会与淘汰互相追尾。entries 是可见 (layer,targetMip) 需求
    // 分布，sets 是最终驻留页集分布；近处需求的细 mip 应优先出现。
    std::cout
        << "[TerrainLayer] pageBudget=" << stats.layerPageBudget
        << " budgetPages=" << stats.layerBudgetPages
        << " budgetSets=" << stats.layerBudgetSets
        << " budgetClamped=" << stats.layerBudgetClampedRefinements
        << " budgetExceeded=" << (stats.layerBudgetExceeded ? 1 : 0)
        << " budgetEntries=";
    for (uint32_t mip = 0;
         mip <= VL::TerrainLayerPageManager::MaxMipLevel;
         ++mip)
    {
        if (mip != 0) std::cout << "/";
        std::cout << stats.layerBudgetEntriesByMip[mip];
    }
    std::cout << " budgetMips=";
    for (uint32_t mip = 0;
         mip <= VL::TerrainLayerPageManager::MaxMipLevel;
         ++mip)
    {
        if (mip != 0) std::cout << "/";
        std::cout << stats.layerBudgetSetsByMip[mip];
    }
    std::cout << std::endl;
}

void TerrainFeedbackRuntime::ReportTerrainLodStatistics(
    const VL::TerrainLodStatsSnapshot& stats)
{
    if (!stats.valid)
    {
        std::cout
            << "[TerrainLOD] stats not published yet."
            << std::endl;
        return;
    }

    std::cout
        << "[TerrainLOD] groups=" << stats.groupCount
        << " switchedLastFrame=" << stats.switchedLastFrame
        << " frame=" << stats.frameIndex
        << " generation=" << stats.worldGeneration
        << std::endl;
    for (const VL::TerrainLodGroupStats& group : stats.groups)
    {
        std::cout
            << "[TerrainLOD] group=" << group.groupId
            << " level=" << group.level << "/" << group.levelCount
            << " distCm="
            << static_cast<long long>(std::llround(group.distanceCm))
            << " switches=" << group.switchCount
            << std::endl;
    }
}

void TerrainFeedbackRuntime::ReportTerrainChunkStatistics(
    const VL::TerrainLodStatsSnapshot& lodStats)
{
    const std::shared_ptr<VL::TerrainStreamingState> streaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout
            << "[TerrainChunk] No active terrain streaming state."
            << std::endl;
        return;
    }

    // 加载期快照：只在 World 加载时写一次，运行时不会再变。
    const VL::TerrainChunkResidencyStats chunkStats =
        streaming->SnapshotChunkStats();
    if (!chunkStats.valid)
    {
        std::cout
            << "[TerrainChunk] No terrain chunk residency was recorded."
            << std::endl;
        return;
    }

    uint32_t groupCount = 0;
    for (const VL::TerrainChunkPlacementStats& placement :
         chunkStats.placements)
    {
        groupCount += placement.chunksPerSide * placement.chunksPerSide;
    }
    std::cout
        << "[TerrainChunk] placements=" << chunkStats.placements.size()
        << " groups=" << groupCount
        << " chunks=" << chunkStats.TotalChunks()
        << " skippedChunks=" << chunkStats.TotalSkippedChunks()
        << " vertexBytes=" << chunkStats.TotalVertexBytes()
        << " indexBytes=" << chunkStats.TotalIndexBytes()
        << " skippedVertexBytes=" << chunkStats.TotalSkippedVertexBytes()
        << " skippedIndexBytes=" << chunkStats.TotalSkippedIndexBytes()
        << " totalIfAllBytes="
        << chunkStats.TotalVertexBytes() + chunkStats.TotalIndexBytes() +
            chunkStats.TotalSkippedVertexBytes() +
            chunkStats.TotalSkippedIndexBytes()
        << std::endl;

    for (const VL::TerrainChunkPlacementStats& placement :
         chunkStats.placements)
    {
        std::cout
            << "[TerrainChunk] placement=" << placement.placementId
            << " asset=" << placement.assetPath
            << " groups="
            << placement.chunksPerSide * placement.chunksPerSide
            << " chunksPerSide=" << placement.chunksPerSide
            << " levels=" << placement.lodLevelCount
            << " residency=" << (placement.residencyEnabled ? "on" : "off");
        if (!placement.residencyDistancesCm.empty())
        {
            std::cout << " windowsCm=";
            for (size_t index = 0;
                 index < placement.residencyDistancesCm.size();
                 ++index)
            {
                if (index != 0) std::cout << ",";
                std::cout << static_cast<long long>(
                    std::llround(placement.residencyDistancesCm[index]));
            }
        }
        std::cout << std::endl;
        for (const VL::TerrainChunkLevelStats& level : placement.levels)
        {
            std::cout
                << "[TerrainChunk]   level=" << level.lodLevel
                << " stitched=" << (level.stitched ? 1 : 0)
                << " chunks=" << level.chunkCount
                << "/" << level.chunkCount + level.skippedChunkCount
                << " vertices=" << level.vertexCount
                << " indices=" << level.indexCount
                << " bytes=" << level.vertexBytes + level.indexBytes
                << " skippedBytes="
                << level.skippedVertexBytes + level.skippedIndexBytes
                << std::endl;
        }
    }

    if (!lodStats.valid)
    {
        std::cout
            << "[TerrainChunk] draw stats not published yet."
            << std::endl;
        return;
    }
    std::cout
        << "[TerrainChunk] draw groups=" << lodStats.groupCount
        << " drawnChunks=" << lodStats.drawnChunks
        << " meshDraws=" << lodStats.totalMeshDraws
        << " switchedLastFrame=" << lodStats.switchedLastFrame
        << " frame=" << lodStats.frameIndex
        << std::endl;
    std::cout << "[TerrainChunk] drawnByLevel=";
    for (uint32_t level = 0; level < VL::TerrainMaxLodLevels; ++level)
    {
        if (level != 0) std::cout << ",";
        std::cout << level << ":" << lodStats.drawnByLevel[level];
    }
    std::cout << " objectsByLevel=";
    for (uint32_t level = 0; level < VL::TerrainMaxLodLevels; ++level)
    {
        if (level != 0) std::cout << ",";
        std::cout << level << ":" << lodStats.residentObjectsByLevel[level];
    }
    std::cout << std::endl;
}
} // namespace VL

