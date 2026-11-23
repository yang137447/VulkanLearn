#include "render/terrain/terrainFeedbackRuntime.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "materialInstance.h"
#include "renderGraph.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainPageBakeGpu.h"
#include "render/resource/terrainCompositedService.h"
#include "render/resource/terrainFeedbackGpu.h"
#include "render/resource/terrainCompositedFeedbackGpu.h"
#include "terrain/terrainWeightMap.h"
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
    if (!pageBakeParityRequested.exchange(false, std::memory_order_acquire))
        return;
    const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming || streaming->pageManager.Registrations().empty() ||
        !streaming->pageBake || !streaming->pageBake->Available())
    {
        std::cout << "[TerrainParity] Coverage GPU bake unavailable." << std::endl;
        return;
    }
    const auto& registration = streaming->pageManager.Registrations().front();
    int tileX = pageBakeParityTileX.load(std::memory_order_relaxed);
    int tileZ = pageBakeParityTileZ.load(std::memory_order_relaxed);
    if (tileX < 0 || tileZ < 0 || uint32_t(tileX) >= registration.tilesPerSide ||
        uint32_t(tileZ) >= registration.tilesPerSide)
        tileX = tileZ = 0;
    try
    {
        const auto& plan = streaming->pageManager.Plans().front();
        const auto cpu = TerrainWeightMapBuilder::BuildPage(plan, registration.tilesPerSide,
            TerrainPageManager::PageResolution, uint32_t(tileX), uint32_t(tileZ),
            streaming->pageManager.PlacementLayerIdMap(
                plan.placementId.empty() ? plan.desc.assetPath : plan.placementId));
        std::vector<uint8_t> ids;
        std::vector<uint8_t> weights;
        std::string error;
        if (!streaming->pageBake->BakePageSync(streaming->pageManager, 0,
                uint32_t(tileX), uint32_t(tileZ), ids, weights, error))
        {
            std::cout << "[TerrainParity] GPU bake failed: " << error << std::endl;
            return;
        }
        const bool passed = cpu.ids == ids && cpu.weights == weights;
        std::cout << "[TerrainParity] placement=" << registration.placementId
            << " tile=" << tileX << "," << tileZ
            << " result=" << (passed ? "PASS" : "FAIL") << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cout << "[TerrainParity] Exception: " << error.what() << std::endl;
    }
}

void TerrainFeedbackRuntime::ConsumeTerrainFeedback(
    const std::shared_ptr<TerrainStreamingState>& streaming,
    uint64_t completedEpoch, uint32_t frameIndex)
{
    if (!streaming)
        return;
    if (streaming->composited && streaming->compositedFeedbackRing)
    {
        const auto requests = streaming->compositedFeedbackRing->TryConsumeComposited(
            frameIndex, completedEpoch, streaming->composited->HasRequestDiagnostics()
                ? streaming->composited.get() : nullptr);
        if (requests)
            streaming->composited->ConsumeFeedback(*requests);
    }
    if (!streaming->feedbackRing)
        return;
    const auto summary = streaming->feedbackRing->TryConsume(frameIndex, completedEpoch);
    if (!summary)
        return;
    streaming->lastConsumedFeedbackEpoch =
        std::max(streaming->lastConsumedFeedbackEpoch, summary->copiedEpoch);
    streaming->latestFeedbackApply = streaming->pageManager.ApplyFeedback(
        *summary, completedEpoch, streaming->maxPageRequestsPerFrame,
        streaming->pendingPageRequests);
    StoreTerrainFeedbackSummary(streaming, *summary);
}

void TerrainFeedbackRuntime::PrepareTerrainFeedbackPass(
    uint8_t frameTag, vk::Extent2D renderExtent)
{
    for (const auto& [name, pass] : RenderGraph::GetInstance().GetRenderpasses())
    {
        (void)name;
        if (pass.type != RenderGraphPassType::TerrainFeedback)
            continue;
        const auto material = pass.materialInstance.lock();
        if (!material)
            continue;
        const auto set = [&](const char* parameter, float value) {
            if (material->HasParameter(parameter))
                material->SetParameter(parameter, value);
        };
        set("u_pageTableSize", float(TerrainPageManager::PageTableSize));
        set("u_pageResolution", float(TerrainPageManager::PageResolution));
        set("u_frameTag", float(frameTag));
        set("u_feedbackScaleX", float(std::max(1u, renderExtent.width)) / TerrainFeedbackResolution);
        set("u_feedbackScaleY", float(std::max(1u, renderExtent.height)) / TerrainFeedbackResolution);
    }
}

void TerrainFeedbackRuntime::StoreTerrainFeedbackSummary(
    const std::shared_ptr<TerrainStreamingState>& streaming,
    const TerrainFeedbackFrameSummary& summary)
{
    TerrainFeedbackReport report;
    report.summary = summary;
    if (streaming && streaming->feedbackRing)
        report.hasPendingFrameTagRange = streaming->feedbackRing->PendingTagRange(
            report.oldestPendingFrameTag, report.newestPendingFrameTag);
    std::lock_guard<std::mutex> lock(statisticsMutex);
    latestReport = std::move(report);
}

void TerrainFeedbackRuntime::ReportTerrainFeedbackStatistics() const
{
    std::lock_guard<std::mutex> lock(statisticsMutex);
    if (!latestReport)
    {
        std::cout << "[TerrainFeedback] No completed feedback frame yet." << std::endl;
        return;
    }
    const auto& summary = latestReport->summary;
    std::cout << "[TerrainFeedback] epoch=" << summary.copiedEpoch
        << " tag=" << uint32_t(summary.frameTag) << " validTexels=" << summary.validTexelCount
        << " distinctPages=" << summary.distinctPageCount << " missPages=" << summary.missPageCount
        << " staleTexels=" << summary.staleTexelCount << std::endl;
}

void TerrainFeedbackRuntime::RequestTerrainResidentCapacity(uint32_t capacity)
{
    const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (streaming)
        streaming->RequestResidentCapacity(capacity);
}

void TerrainFeedbackRuntime::PublishTerrainStreamingStats(
    const std::shared_ptr<TerrainStreamingState>& streaming, uint64_t completedEpoch)
{
    if (!streaming)
        return;
    auto& manager = streaming->pageManager;
    manager.ReclaimExpiredQuarantines(completedEpoch);
    TerrainStreamingStats stats;
    stats.statsPublished = true;
    stats.residentPages = manager.ResidentCount();
    stats.residentCapacity = manager.ResidentCapacity();
    stats.virtualPages = uint32_t(manager.Pages().size());
    stats.registrations = uint32_t(manager.Registrations().size());
    stats.freePhysicalSlots = manager.ImmediatelyAllocatablePhysicalSlotCount();
    stats.quarantinedPhysicalSlots = manager.QuarantinedPhysicalSlotCount();
    stats.requestCount = manager.RequestCount();
    stats.hitCount = manager.PageHitCount();
    stats.missCount = manager.PageMissCount();
    stats.evictionCount = manager.EvictionCount();
    stats.preloadDeferredCount = manager.PreloadDeferredCount();
    stats.uploadedPagesTotal = streaming->uploadedPagesTotal;
    stats.lastUploadedPages = streaming->lastUploadedPages;
    stats.gpuBakedPagesTotal = streaming->gpuBakedPagesTotal;
    stats.lastGpuBakedPages = streaming->lastGpuBakedPages;
    stats.pendingGpuBakeJobs = uint32_t(manager.PendingGpuBakeJobCount());
    stats.lastFeedbackEpoch = streaming->lastConsumedFeedbackEpoch;
    stats.lastFeedbackApply = streaming->latestFeedbackApply;
    streaming->PublishStats(stats);
    streaming->lastUploadedPages = 0;
}

void TerrainFeedbackRuntime::ReportTerrainStatistics()
{
    const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
    {
        std::cout << "[Terrain] No active terrain streaming state." << std::endl;
        return;
    }
    const auto stats = streaming->SnapshotStats();
    std::cout << "[Terrain] resident=" << stats.residentPages << "/" << stats.residentCapacity
        << " virtualPages=" << stats.virtualPages << " registrations=" << stats.registrations
        << " freeSlots=" << stats.freePhysicalSlots << " quarantined=" << stats.quarantinedPhysicalSlots
        << " requests=" << stats.requestCount << " requestHits=" << stats.hitCount
        << " misses=" << stats.missCount << " evictions=" << stats.evictionCount
        << " uploadedPages=" << stats.uploadedPagesTotal << " lastUploaded=" << stats.lastUploadedPages
        << " gpuBakedPages=" << stats.gpuBakedPagesTotal
        << " gpuBakePending=" << stats.pendingGpuBakeJobs << std::endl;
}

void TerrainFeedbackRuntime::ReportTerrainLodStatistics(const TerrainLodStatsSnapshot& stats)
{
    if (!stats.valid)
    {
        std::cout << "[TerrainLOD] stats not published yet." << std::endl;
        return;
    }
    std::cout << "[TerrainLOD] groups=" << stats.groupCount
        << " switchedLastFrame=" << stats.switchedLastFrame
        << " frame=" << stats.frameIndex << " generation=" << stats.worldGeneration << std::endl;
    for (const auto& group : stats.groups)
        std::cout << "[TerrainLOD] group=" << group.groupId << " level=" << group.level
            << "/" << group.levelCount << " distCm=" << std::llround(group.distanceCm)
            << " switches=" << group.switchCount << std::endl;
}

void TerrainFeedbackRuntime::ReportTerrainChunkStatistics(const TerrainLodStatsSnapshot& lodStats)
{
    const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming)
        return;
    const auto stats = streaming->SnapshotChunkStats();
    if (!stats.valid)
    {
        std::cout << "[TerrainChunk] No terrain chunk residency was recorded." << std::endl;
        return;
    }
    uint32_t groups = 0;
    for (const auto& placement : stats.placements)
        groups += placement.chunksPerSide * placement.chunksPerSide;
    std::cout << "[TerrainChunk] placements=" << stats.placements.size()
        << " groups=" << groups << " chunks=" << stats.TotalChunks()
        << " skippedChunks=" << stats.TotalSkippedChunks()
        << " vertexBytes=" << stats.TotalVertexBytes() << " indexBytes=" << stats.TotalIndexBytes()
        << " skippedVertexBytes=" << stats.TotalSkippedVertexBytes()
        << " skippedIndexBytes=" << stats.TotalSkippedIndexBytes()
        << " totalIfAllBytes=" << stats.TotalVertexBytes() + stats.TotalIndexBytes() +
            stats.TotalSkippedVertexBytes() + stats.TotalSkippedIndexBytes() << std::endl;
    for (const auto& placement : stats.placements)
    {
        std::cout << "[TerrainChunk] placement=" << placement.placementId
            << " asset=" << placement.assetPath << " groups=" << placement.chunksPerSide * placement.chunksPerSide
            << " chunksPerSide=" << placement.chunksPerSide << " levels=" << placement.lodLevelCount
            << " residency=" << (placement.residencyEnabled ? "on" : "off") << std::endl;
        for (const auto& level : placement.levels)
            std::cout << "[TerrainChunk]   level=" << level.lodLevel << " stitched=" << (level.stitched ? 1 : 0)
                << " chunks=" << level.chunkCount << "/" << level.chunkCount + level.skippedChunkCount
                << " vertices=" << level.vertexCount << " indices=" << level.indexCount
                << " bytes=" << level.vertexBytes + level.indexBytes
                << " skippedBytes=" << level.skippedVertexBytes + level.skippedIndexBytes << std::endl;
    }
    if (!lodStats.valid)
        return;
    std::cout << "[TerrainChunk] draw groups=" << lodStats.groupCount
        << " drawnChunks=" << lodStats.drawnChunks << " meshDraws=" << lodStats.totalMeshDraws
        << " switchedLastFrame=" << lodStats.switchedLastFrame << " frame=" << lodStats.frameIndex << std::endl;
    std::cout << "[TerrainChunk] drawnByLevel=";
    for (uint32_t level = 0; level < TerrainMaxLodLevels; ++level)
        std::cout << (level ? "," : "") << level << ":" << lodStats.drawnByLevel[level];
    std::cout << " objectsByLevel=";
    for (uint32_t level = 0; level < TerrainMaxLodLevels; ++level)
        std::cout << (level ? "," : "") << level << ":" << lodStats.residentObjectsByLevel[level];
    std::cout << std::endl;
}
}
