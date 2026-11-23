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
uint64_t TerrainCompositedService::RefreshRevision(
    uint64_t generation, const std::string& surfaceId) const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    Require(generation == impl->worldGeneration, "stale refresh World generation");
    Require(impl->expectedSurfaces.count(surfaceId) != 0, "unknown refresh placement");
    const auto found = impl->publishedRevisions.find(surfaceId);
    return found == impl->publishedRevisions.end() ? 0 : found->second;
}

TerrainCompositedRefreshTicket TerrainCompositedService::QueueRefresh(
    TerrainCompositedRefreshCommand command)
{
    Require(IsAuthoringMode() && command.inputs && !command.changes.empty() && command.editSeq,
        "refresh requires frozen author inputs, typed changes and edit identity");
    const auto id = TerrainPlacementIdentity(*command.inputs);
    for (const auto& change : command.changes)
    {
        const auto& rect = change.rect;
        Require(change.surfaceId == id && std::isfinite(rect.minU) && std::isfinite(rect.minV) &&
            std::isfinite(rect.maxU) && std::isfinite(rect.maxV) && rect.minU >= 0 && rect.minV >= 0 &&
            rect.maxU <= 1 && rect.maxV <= 1 && rect.minU < rect.maxU && rect.minV < rect.maxV,
            "invalid typed refresh placement/rectangle");
    }
    std::lock_guard<std::mutex> lock(impl->mutex);
    Require(impl->refreshes.size() < 4096, "refresh receipt ledger is full");
    TerrainCompositedRefreshTicket ticket{++impl->nextTicket, command.worldGeneration, command.editSeq, id};
    Impl::Refresh refresh;
    refresh.status.ticket = ticket;
    refresh.command = std::move(command);
    if (refresh.command.worldGeneration != impl->worldGeneration ||
        !impl->expectedSurfaces.count(id) ||
        refresh.command.expectedRevision != impl->publishedRevisions[id])
    {
        refresh.status.state = TerrainCompositedRefreshState::Stale;
        refresh.status.diagnostic = "World or published surface revision changed";
    }
    else
    {
        try
        {
            Require(bool(impl->sourceSnapshots), "refresh pixel owners require a prepared World");
            refresh.command.inputs = std::make_shared<const TerrainBuildPlan>(
                impl->sourceSnapshots->Retain(*refresh.command.inputs));
            for (auto& previous : impl->refreshes)
                if (!previous.second.status.Terminal() && previous.second.status.ticket.surfaceId == id)
                {
                    previous.second.status.state = TerrainCompositedRefreshState::Stale;
                    previous.second.status.diagnostic = "superseded by a newer edit";
                    previous.second.command.inputs.reset();
                }
            refresh.revision = ++impl->nextEditRevision;
        }
        catch (const std::exception& error)
        {
            refresh.status.state = TerrainCompositedRefreshState::Failed;
            refresh.status.diagnostic = error.what();
        }
    }
    if (refresh.status.Terminal())
        refresh.command.inputs.reset();
    impl->refreshes.emplace(ticket.id, std::move(refresh));
    return ticket;
}

TerrainCompositedRefreshStatus TerrainCompositedService::PollRefresh(
    const TerrainCompositedRefreshTicket& ticket) const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    const auto found = impl->refreshes.find(ticket.id);
    if (found == impl->refreshes.end() ||
        found->second.status.ticket.worldGeneration != ticket.worldGeneration ||
        found->second.status.ticket.editSeq != ticket.editSeq ||
        found->second.status.ticket.surfaceId != ticket.surfaceId)
        return {ticket, TerrainCompositedRefreshState::Stale, 0, "unknown refresh receipt"};
    return found->second.status;
}

TerrainCompositedAuthorBaseline TerrainCompositedService::LoadAuthorBaseline(
    uint64_t generation, const std::string& surfaceId) const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    Require(generation == impl->worldGeneration, "stale author baseline World generation");
    Require(bool(impl->store), "author baseline requires a verified V2 publication");
    Require(impl->frozen.has_value() && bool(impl->frozen->runtime.owner),
        "author verification requires a charged World owner");
    const bool sameStore = impl->authorVerificationStore &&
        impl->authorVerificationStore.get() == impl->store.get() &&
        !impl->authorVerificationStore.owner_before(impl->store) &&
        !impl->store.owner_before(impl->authorVerificationStore);
    if (!impl->scopedAuthorBaselineSession || !sameStore ||
        impl->authorVerificationWorld != generation)
    {
        const auto identity = impl->store->Production().ContentHash();
        auto session = TerrainCompositedVerifiedInputs::Begin(
            impl->store, generation, identity, impl->frozen->runtime.owner);
        impl->authorVerificationStore = impl->store;
        impl->authorVerificationWorld = generation;
        impl->authorVerificationIdentity = identity;
        impl->scopedAuthorBaselineSession = std::move(session);
    }
    TerrainCompositedAuthorBaseline baseline;
    baseline.worldGeneration = generation;
    baseline.surfaceId = surfaceId;
    baseline.store = impl->store;
    baseline.productionIdentity = impl->store->Production().SurfaceContentHash(surfaceId);
    const auto& production = impl->store->Production();
    const auto surface = std::find_if(production.surfaces.begin(), production.surfaces.end(),
        [&](const auto& item) { return item.surfaceId == surfaceId; });
    Require(surface != production.surfaces.end() && !surface->authorDependencies.empty(),
        "publication has no captured author dependency snapshot");
    baseline.dependencies = surface->authorDependencies;
    baseline.verificationOwner = impl->scopedAuthorBaselineSession;
    const TerrainCompositedVerifiedInputs::Binding binding{
        impl->store, generation, impl->authorVerificationIdentity, surfaceId};
    const auto before = baseline.verificationOwner->Statistics();
    const double beganMs = QualityMonotonicMs();
    std::set<std::string> verified;
    for (const auto& dependency : baseline.dependencies)
        for (const auto& objectId : dependency.productionObjectIds)
            if (verified.insert(objectId).second)
            {
                if (before.closed)
                    Require(baseline.verificationOwner->HasVerified(binding, objectId),
                        "closed author verification batch has no captured object proof");
                else
                    baseline.verificationOwner->VerifyObject(binding, objectId);
            }
    const auto after = baseline.verificationOwner->Statistics();
    ServiceEvidence("RVT_AUTHOR_BASELINE_SERVICE", "[RVT_AUTHOR_BASELINE_SERVICE] ", nlohmann::json{
        {"worldGeneration", generation}, {"surfaceId", surfaceId},
        {"publicationIdentity", impl->authorVerificationIdentity.ToHex()},
        {"beganMonotonicMs", beganMs}, {"endedMonotonicMs", QualityMonotonicMs()},
        {"readCalls", after.readCalls - before.readCalls},
        {"validatedBytes", after.validatedBytes - before.validatedBytes},
        {"requestedBytes", after.requestedBytes - before.requestedBytes},
        {"hits", after.hits - before.hits}, {"failures", after.failures - before.failures},
        {"batchReadCalls", after.readCalls}, {"batchValidatedBytes", after.validatedBytes},
        {"batchClosed", after.closed}});
    return baseline;
}
} // namespace VL
