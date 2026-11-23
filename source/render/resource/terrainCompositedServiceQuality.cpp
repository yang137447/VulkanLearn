#include "render/resource/terrainCompositedService.h"
#include "core/runtimeEvidence.h"
#include "core/runtimeFileTrace.h"
#include "core/tracingIfstream.h"

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "texture.h"
#include "textureAssetLoader.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainCompositedBakeGpu.h"
#include "render/resource/terrainCompositedProducerSnapshot.h"
#include "render/resource/terrainCompositedWorldBindings.h"
#include "render/resource/rendererResourceCache.h"
#include "render/frame/frameSubmitFault.h"
#include "render/resource/terrainCompositedProductionGpu.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/materialInstanceStateBatch.h"
#include "shaderCompiler.h"
#include "render/resource/terrainHeightAtlas.h"
#include "render/terrain/terrainCompositedRuntime.h"
#include "render/terrain/terrainCompositedProductionPriority.h"
#include "render/terrain/terrainCompositedServiceRequestDiagnostics.h"
#include "shader/build/atomicFile.h"
#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainCompositedAuthorRecipe.h"
#include "terrain/terrainCompositedFeedback.h"
#include "terrain/terrainCompositedProductionIdentity.h"
#include "terrain/terrainCompositedWeightOverlay.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainWeightMap.h"
#include "terrain/terrainPaintSha256.h"
#include "terrain/terrainRvtScheduler.h"
#include "terrain/terrainCompositedRefresh.h"
#include "terrain/terrainCompositedShaderPath.h"
#include "terrain/terrainCompositedQualityClock.h"
#include "terrain/terrainCompositedSourceSnapshots.h"
#include "terrain/terrainCompositedVerifiedInputs.h"
#include "terrain/terrainFaultInjection.h"
#include "terrain/terrainFaultOwnerRegistration.h"
#include "terrain/terrainCompositedPageSourceBatch.h"
#include "render/resource/terrainCompositedResourceSnapshot.h"
#include "render/frame/terrainCompositedReadiness.h"
#include "renderGraph.h"

#include "render/resource/terrainCompositedQualityGpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#endif


#include "render/resource/terrainCompositedServiceImpl.h"

namespace VL
{
std::shared_ptr<TerrainCompositedQualitySnapshot> TerrainCompositedService::CaptureQualitySnapshot(
    uint64_t frame, uint64_t poseId, uint64_t drawEpoch,
    uint32_t renderWidth, uint32_t renderHeight)
{
    Require(impl->recording && impl->runtime,
        "quality snapshot must bind the currently recorded draw submission");
    auto snapshot = std::make_shared<TerrainCompositedQualitySnapshot>();
    snapshot->frame = frame;
    snapshot->poseId = poseId;
    snapshot->worldGeneration = impl->worldGeneration;
    snapshot->drawSubmissionEpoch = drawEpoch;
    snapshot->renderWidth = renderWidth;
    snapshot->renderHeight = renderHeight;
    const auto identity = [](const ContentDigest& digest)
    {
        std::array<uint32_t, 8> words{};
        std::memcpy(words.data(), digest.bytes.data(), digest.bytes.size());
        return words;
    };
    std::map<std::string, uint32_t> indices;
    struct Retained
    {
        std::shared_ptr<const void> inputs;
        std::vector<std::shared_ptr<Texture>> tables;
    };
    auto retained = std::make_shared<Retained>();
    retained->inputs = impl->currentInputs;
    for (const auto& surface : impl->surfaces)
    {
        const uint32_t index = static_cast<uint32_t>(snapshot->surfaces.size());
        indices.emplace(surface->desc.surfaceId, index);
        const auto binding = impl->runtime->Binding(surface->desc.surfaceId);
        const auto token = impl->runtime->Token({surface->desc.surfaceId, 0, 0, 0});
        TerrainCompositedQualitySurface target;
        target.surfaceId = surface->desc.surfaceId;
        target.pageTable = binding.compositedPageTable;
        retained->tables.push_back(target.pageTable);
        target.data.index = index;
        target.data.width = surface->desc.width;
        target.data.height = surface->desc.height;
        target.data.maxMipAndRepeat = surface->desc.Layout().MipCount() - 1 |
            (surface->desc.addressMode == TerrainCompositedVTAddressMode::Repeat ? 256u : 0u);
        target.data.generationLow = static_cast<uint32_t>(impl->worldGeneration);
        target.data.generationHigh = static_cast<uint32_t>(impl->worldGeneration >> 32);
        target.data.revisionLow = static_cast<uint32_t>(token.revision);
        target.data.revisionHigh = static_cast<uint32_t>(token.revision >> 32);
        target.data.identity = identity(token.contentHash);
        snapshot->surfaces.push_back(std::move(target));
    }
    for (const auto& owner : impl->runtime->RecordedOwners())
    {
        Require(owner.slot < snapshot->owners.size(), "quality slot capacity differs from frozen pool");
        if (!owner.valid)
            continue;
        auto& target = snapshot->owners[owner.slot];
        target.valid = 1;
        target.surface = indices.at(owner.token.key.surfaceId);
        target.mip = owner.token.key.mip;
        target.xy = owner.token.key.x | (owner.token.key.y << 16);
        target.generationLow = static_cast<uint32_t>(impl->worldGeneration);
        target.generationHigh = static_cast<uint32_t>(impl->worldGeneration >> 32);
        target.revisionLow = static_cast<uint32_t>(owner.token.revision);
        target.revisionHigh = static_cast<uint32_t>(owner.token.revision >> 32);
        target.identity = identity(owner.token.contentHash);
    }
    snapshot->drawPageTableOwner = std::move(retained);
    return snapshot;
}

void TerrainCompositedService::SetQualityPose(
    uint64_t worldGeneration, uint64_t poseId, double monotonicMs)
{
    Require(worldGeneration == impl->worldGeneration && poseId && std::isfinite(monotonicMs),
        "invalid quality pose identity/time");
    if (impl->qualityPose == poseId)
        return;
    impl->qualityPose = poseId;
    impl->lastQualityEpoch = 0;
    impl->qualityPoseStartedMs = monotonicMs;
    if (!CookMode())
        impl->qualityClock.SetPose(poseId, monotonicMs);
    impl->interactiveReady.store(false, std::memory_order_release);
}

bool TerrainCompositedService::ObserveQuality(const TerrainCompositedQualityReadback& readback)
{
    if (!readback.snapshot || readback.snapshot->worldGeneration != impl->worldGeneration ||
        readback.snapshot->poseId != impl->qualityPose ||
        readback.submissionEpoch != readback.snapshot->drawSubmissionEpoch ||
        readback.completedEpoch < readback.submissionEpoch ||
        readback.submissionEpoch <= impl->lastQualityEpoch)
        return false;
    impl->lastQualityEpoch = readback.submissionEpoch;
    const auto counts = TerrainQualityAbi::Recompute(readback.words, readback.bytes);
    bool current = readback.snapshot->surfaces.size() == impl->surfaces.size();
    for (const auto& surface : readback.snapshot->surfaces)
    {
        const auto token = impl->runtime->Token({surface.surfaceId, 0, 0, 0});
        std::array<uint32_t, 8> identity{};
        std::memcpy(identity.data(), token.contentHash.bytes.data(), token.contentHash.bytes.size());
        current = current && surface.data.identity == identity &&
            TerrainQualityAbi::Join(surface.data.revisionLow, surface.data.revisionHigh) == token.revision;
    }
    const bool heightReady = std::all_of(impl->surfaces.begin(), impl->surfaces.end(),
        [](const auto& surface) {
            const bool needsHeight = std::any_of(surface->plan.desc.layers.begin(), surface->plan.desc.layers.end(),
                [](const auto& layer) { return layer.hasHeight; });
            return !needsHeight || (surface->height.heightAtlas && surface->height.layerMap);
        });
    const double elapsedMs = readback.completedMonotonicMs - impl->qualityPoseStartedMs;
    if (!std::isfinite(elapsedMs) || elapsedMs < 0)
        return false;
    const bool quality = current && heightReady && impl->runtime->RootsCurrent() &&
        !impl->runtime->Statistics().budgetInsufficient && counts.MeetsQualityThreshold() &&
        !impl->qualityConvergenceFailed.load(std::memory_order_acquire);
    const bool ready = CookMode() ? quality :
        impl->qualityClock.Observe(readback.completedMonotonicMs, quality);
    if (!CookMode() && impl->qualityClock.Failed(QualityMonotonicMs()))
    {
        if (QualityLenient())
        {
            // 互动模式：连续移动可能一直追不上 10 s 收敛预算。只记录一次超时证据并
            // 重开预算窗口，保持 arrival gate 可用；验收默认仍锁存失败。
            if (impl->qualityClock.ResetAfterTimeout(QualityMonotonicMs()))
            {
                ServiceEvidence("RVT_QUALITY_TIMEOUT", "[RVT_QUALITY_TIMEOUT] ", nlohmann::json{
                    {"worldGeneration", impl->worldGeneration},
                    {"poseId", impl->qualityPose},
                    {"elapsedMs", elapsedMs},
                    {"interactive", false}});
            }
        }
        else
        {
            impl->qualityConvergenceFailed.store(true, std::memory_order_release);
        }
    }
    impl->interactiveReady.store(ready, std::memory_order_release);
    ServiceEvidence("RVT_QUALITY_STATE", "[RVT_QUALITY_STATE] ", nlohmann::json{
        {"worldGeneration", impl->worldGeneration}, {"poseId", impl->qualityPose},
        {"submissionEpoch", readback.submissionEpoch}, {"completedEpoch", readback.completedEpoch},
        {"visible", counts.visibleSamples}, {"validAtRawTargetPlusOne", counts.validAtTargetPlusBias},
        {"unresolved", counts.unresolved}, {"incorrectIdentity", counts.incorrectIdentity},
        {"rootOnly", counts.rootOnlySamples}, {"elapsedMs", elapsedMs}, {"interactive", ready},
        {"deadlineMonotonicMs", impl->qualityClock.DeadlineMs()},
        {"convergenceFailed", impl->qualityConvergenceFailed.load(std::memory_order_acquire)}});
    return ready;
}

bool TerrainCompositedService::IsInteractiveReady() const noexcept
{
    return !QualityConvergenceFailed() && impl->interactiveReady.load(std::memory_order_acquire);
}

bool TerrainCompositedService::QualityConvergenceFailed() const noexcept
{
    if (!CookMode() && impl->qualityClock.Failed(QualityMonotonicMs()))
    {
        if (QualityLenient())
        {
            // 互动模式：查询路径同样不能把超时锁死，否则 arrival gate 会永久关闭。
            impl->qualityClock.ResetAfterTimeout(QualityMonotonicMs());
        }
        else
        {
            impl->qualityConvergenceFailed.store(true, std::memory_order_release);
            impl->interactiveReady.store(false, std::memory_order_release);
        }
    }
    return impl->qualityConvergenceFailed.load(std::memory_order_acquire);
}
} // namespace VL
