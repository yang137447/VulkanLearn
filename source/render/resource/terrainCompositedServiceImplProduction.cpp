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
void TerrainCompositedService::Impl::InitializeProduction()
{
    if (!productionTasks.empty())
        return;
    if (!productionPriority)
        productionPriority = std::make_unique<TerrainCompositedProductionPriority>(
            *frozen->runtime.owner, frozen->runtime.surface.slots);
    const auto count = frozen->production.productionSlots;
    const uint32_t capacity = static_cast<uint32_t>(surfaces.size());
    const auto quote = [&](uint64_t size, vk::BufferUsageFlags usage)
    { return backend.QueryBufferMemoryRequirements(size, usage).size; };
    const uint64_t parameters = (count + 1) *
        (quote(Baker::WorldParameterBytes, vk::BufferUsageFlagBits::eStorageBuffer) +
         quote(uint64_t(64) * capacity, vk::BufferUsageFlagBits::eStorageBuffer));
    bakerLease = frozen->runtime.owner->TryReserve({parameters, parameters, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(bakerLease.has_value(), "Baker parameter/chart actual allocations exceed frozen workspace");
    if (!baker)
    {
        const auto limits = backend.GetSelectedDeviceLimits();
        std::string error;
        Require(Baker::ValidateDeviceLimits(limits, error), error);
        baker = std::make_unique<Baker>(backend, pipelineFactory, count, capacity, &limits);
    }
    Require(baker->Available(), baker->UnavailableReason());
    baker->SetShaderReloadCoordinator(shaderReloadCoordinator);
    if (!currentProducer)
        currentProducer = baker->CaptureProducer();
    Require(bool(currentProducer), "Baker has no immutable current producer bundle");
    const auto outputUsage = vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
    const auto outputBytes = quote(Baker::OutputBytes, outputUsage);
    const auto scratchExtent = Baker::RequiredScratchBytes(4);
    const auto scratchUsage = vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eIndirectBuffer;
    const auto scratchBytes = scratchExtent ? quote(scratchExtent, scratchUsage) : 0;
    const auto statusBytes = quote(sizeof(uint32_t), vk::BufferUsageFlagBits::eTransferDst);
    for (uint32_t index = 0; index < count; ++index)
    {
        ProductionTask task;
        task.lease = frozen->runtime.owner->TryReserve(
            {outputBytes + scratchBytes + statusBytes, statusBytes, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(task.lease.has_value(), "production task actual allocations exceed frozen workspace");
        const auto deviceBuffer = [&](uint64_t size, vk::BufferUsageFlags usage, const char* name)
        {
            auto buffer = std::make_shared<Buffer>();
            buffer->backend = &backend;
            buffer->bytes = size;
            const auto created = backend.CreateBuffer(size, usage,
                vk::MemoryPropertyFlagBits::eDeviceLocal, name);
            buffer->buffer = created.first;
            buffer->memory = created.second;
            return buffer;
        };
        task.output = deviceBuffer(Baker::OutputBytes, outputUsage, "RvtProductionOutput");
        if (scratchExtent)
            task.scratch = deviceBuffer(scratchExtent, scratchUsage, "RvtProductionScratch");
        task.status = HostBuffer(sizeof(uint32_t),
            vk::BufferUsageFlagBits::eTransferDst, "RvtProductionStatus");
        Require(backend.SupportsTimestampQueries() && backend.GetGraphicsTimestampValidBits(),
            "RVT production requires GPU timestamp support");
        task.timing = std::make_shared<ProductionTask::Timing>();
        task.timing->backend = &backend;
        task.timing->pool = backend.CreateTimestampQueryPool(4, "RvtProductionAndCopy");
        productionTasks.push_back(std::move(task));
    }
}

void TerrainCompositedService::Impl::ValidateProductionGenerator() const
{
    const auto& expected = store->Production();
    const auto actual = pipelineFactory.GetShaderCompiler().CaptureManifestSnapshot();
    Require(actual.compilePolicy == expected.generator.compilePolicy,
        "runtime compute policy differs from V2 production generator");
    // 生产身份只比较 compute 源码/include 与 runtime 角色 SPIR-V；publication 里
    // 同源编译出的 debug reflection 摘要会随调试信息重写，不能参与比较。原因与
    // 精确对比规则集中在 MatchesTerrainCompositedGeneratorClosure 内注释。
    Require(MatchesTerrainCompositedGeneratorClosure(expected, actual),
        "runtime compute source/SPV closure differs from V2 production identity; rebuild the publication");
}

void TerrainCompositedService::Impl::CompleteProduction(uint64_t completedEpoch, bool loadingFenceComplete)
{
    for (auto& task : productionTasks)
    {
        if (!task.page || (!task.epoch && !(loadingFenceComplete && task.recorded)) ||
            task.epoch > completedEpoch)
            continue;
        bool staleDelivery = false;
        bool staleCancelled = false;
        if (faultStaleHook && task.epoch)
        {
            const auto action = TerrainFaultInjection().ReadAction(faultStaleHook);
            if (!task.staleHold && action && !action->consumed &&
                !action->cancelRequested && !action->timedOut)
            {
                const auto& key = task.page->token.key;
                const TerrainFaultPageIdentity identity{runtime->LogicalSurfaceId(key.surfaceId),
                    key.mip, key.x, key.y, task.page->token.revision};
                if (action->request.page.surfaceId == identity.surfaceId &&
                    action->request.page.mip == identity.mip && action->request.page.x == identity.x &&
                    action->request.page.y == identity.y && action->request.page.revision == identity.revision)
                {
                    auto metadata = frozen->runtime.owner->TryReserve({0, 4096, 0, 0},
                        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
                    Require(metadata.has_value(), "stale completion hold exceeds CPU workspace");
                    auto hold = std::make_unique<ProductionTask::StaleHold>();
                    const auto requested = TerrainFaultInjection().ConsumeAction(
                        faultStaleHook, worldGeneration, identity);
                    if (requested)
                    {
                        hold->metadata = std::move(*metadata);
                        hold->request = *requested;
                        task.staleHold = std::move(hold);
                        ServiceEvidence("RVT_EVIDENCE", "[RVT_EVIDENCE] ", {
                            {"event", "actual-completion-held"}, {"faultId", requested->faultId},
                            {"worldGeneration", worldGeneration}, {"surfaceId", identity.surfaceId},
                            {"revision", identity.revision}, {"slot", task.page->slot},
                            {"reservation", task.page->reservation}, {"writeEpoch", task.epoch},
                            {"completedEpoch", completedEpoch},
                            {"contentIdentity", task.page->token.contentHash.ToHex()}});
                    }
                }
            }
            if (task.staleHold)
            {
                const auto& request = task.staleHold->request;
                const auto current = runtime->Token({request.page.surfaceId,
                    request.page.mip, request.page.x, request.page.y});
                staleCancelled = !action || action->cancelRequested || action->timedOut;
                staleDelivery = current.revision >= request.replacementRevision &&
                    current.revision > task.page->token.revision;
                if (!staleCancelled && !staleDelivery)
                    continue;
            }
            else if (action && (action->cancelRequested || action->timedOut))
                TerrainFaultInjection().FinishAction(faultStaleHook, action->request.faultId);
        }
        uint32_t status = 0;
        std::memcpy(&status, task.status->mapped, sizeof(status));
        const uint32_t rawStatus = status;
        std::optional<TerrainFaultStatusOverride> injected;
        nlohmann::json injectionEvent;
        nlohmann::json failureEvent;
        if (faultStatusHook && task.epoch)
        {
            const auto& key = task.page->token.key;
            injected = TerrainFaultInjection().ConsumeStatus(faultStatusHook,
                {worldGeneration, {runtime->LogicalSurfaceId(key.surfaceId), key.mip, key.x, key.y,
                    task.page->token.revision}, task.epoch, completedEpoch, rawStatus});
            if (injected)
            {
                status = injected->injectedStatus;
                injectionEvent = nlohmann::json{
                    {"protocol", 1}, {"event", "fault-injection"}, {"hook", "statusNonzero"},
                    {"faultId", injected->faultId}, {"worldGeneration", worldGeneration},
                    {"surfaceId", runtime->LogicalSurfaceId(key.surfaceId)}, {"mip", key.mip},
                    {"x", key.x}, {"y", key.y}, {"revision", task.page->token.revision},
                    {"slot", task.page->slot}, {"writeEpoch", task.epoch},
                    {"fenceCompletedEpoch", completedEpoch}, {"monotonicMs", QualityMonotonicMs()},
                    {"originalStatus", rawStatus}, {"injectedStatus", status}};
            }
        }
        std::array<uint64_t, 2> productionTicks{}, copyTicks{};
        const bool productionTimed = backend.TryReadTimestampQueryPair(task.timing->pool, 0, productionTicks);
        const bool copyTimed = backend.TryReadTimestampQueryPair(task.timing->pool, 2, copyTicks);
        if (productionTimed && copyTimed)
        {
            const auto bits = backend.GetGraphicsTimestampValidBits();
            const uint64_t mask = bits >= 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
            const double scale = backend.GetTimestampPeriodNanoseconds() / 1.e6;
            const double productionMs = ((productionTicks[1] - productionTicks[0]) & mask) * scale;
            const double copyMs = ((copyTicks[1] - copyTicks[0]) & mask) * scale;
            std::cout << "[RVT_PRODUCTION_RAW] "
                << nlohmann::json{{"submissionEpoch", task.epoch},
                    {"completedEpoch", completedEpoch}, {"surfaceId", task.page->token.key.surfaceId},
                    {"revision", task.page->token.revision},
                    {"mip", task.page->token.key.mip}, {"x", task.page->token.key.x},
                    {"y", task.page->token.key.y}, {"slot", task.page->slot},
                    {"reservation", task.page->reservation},
                    {"tokenContentHash", task.page->token.contentHash.ToHex()},
                    {"producerIdentity", task.producer ? task.producer->Identity() : ""},
                    {"productionMs", productionMs},
                    {"atlasCopyAndStatusMs", copyMs}, {"combinedMs", productionMs + copyMs},
                    {"status", rawStatus}, {"effectiveStatus", status}, {"timestampValidBits", bits},
                    {"timestampPeriodNanoseconds", backend.GetTimestampPeriodNanoseconds()}}.dump()
                << std::endl;
        }
        const bool accepted = runtime->CompleteProduction(*task.page, task.epoch, status == 0);
        nlohmann::json staleEvent;
        if (task.staleHold)
            staleEvent = {{"event", staleDelivery ? "actual-stale-completion-delivered" : "completion-hold-cancelled"},
                {"faultId", task.staleHold->request.faultId}, {"worldGeneration", worldGeneration},
                {"surfaceId", task.staleHold->request.page.surfaceId}, {"oldRevision", task.page->token.revision},
                {"oldContentIdentity", task.page->token.contentHash.ToHex()},
                {"writeEpoch", task.epoch}, {"completedEpoch", completedEpoch},
                {"slot", task.page->slot}, {"reservation", task.page->reservation},
                {"normalRuntimeAccepted", accepted}, {"cancelledOrTimedOut", staleCancelled}};
        if (injected)
        {
            const auto diagnostics = runtime->ProductionDiagnostics(task.page->token, completedEpoch);
            Require(diagnostics.has_value(), "fault completed task namespace has no native diagnostic");
            failureEvent = nlohmann::json{
                {"protocol", 1}, {"event", "production-failure"}, {"faultId", injected->faultId},
                {"worldGeneration", worldGeneration},
                {"surfaceId", runtime->LogicalSurfaceId(task.page->token.key.surfaceId)},
                {"mip", task.page->token.key.mip}, {"x", task.page->token.key.x},
                {"y", task.page->token.key.y}, {"revision", task.page->token.revision},
                {"slot", task.page->slot}, {"writeEpoch", task.epoch},
                {"fenceCompletedEpoch", completedEpoch}, {"monotonicMs", QualityMonotonicMs()},
                {"originalStatus", rawStatus}, {"status", status}, {"publishedChannels", accepted ? 3 : 0},
                {"attempts", diagnostics->attempts}, {"maxAttempts", diagnostics->maxAttempts},
                {"backoffTicks", diagnostics->backoffTicks}, {"submitResult", 0},
                {"retireEpoch", diagnostics->retireEpoch},
                {"retirementOwnerAlive", diagnostics->retirementOwnerAlive}
            };
        }
        if (task.candidateRevision)
        {
            const auto candidate = std::static_pointer_cast<WorldCandidate>(task.inputOwner);
            if (accepted)
                ++candidate->completedProductionCount;
            const auto logical = runtime->LogicalSurfaceId(task.page->token.key.surfaceId);
            const auto surface = std::find_if(candidate->surfaces.begin(), candidate->surfaces.end(),
                [&](const auto& item) { return item->desc.surfaceId == logical; });
            const bool root = surface != candidate->surfaces.end() &&
                task.page->token.key.mip == (*surface)->desc.Layout().MipCount() - 1;
            candidate->transaction.ObserveProduction(task.candidateRevision, logical,
                root, task.epoch, status, accepted);
        }
        if (!accepted)
            ++staleBakeResults;
        if (status)
            std::cerr << "[TerrainVT] GPU production failed: " << Baker::DescribeStatus(status) << std::endl;
        task.page.reset();
        task.epoch = 0;
        task.recorded = false;
        task.retained.clear();
        task.owner.reset();
        task.snapshot.reset();
        task.producer.reset();
        task.executionLease.reset();
        task.inputOwner.reset();
        task.candidateRevision = 0;
        if (task.staleHold)
        {
            auto faultId = std::move(task.staleHold->request.faultId);
            task.staleHold.reset();
            TerrainFaultInjection().FinishAction(faultStaleHook, faultId);
            ServiceEvidence("RVT_EVIDENCE", "[RVT_EVIDENCE] ", staleEvent);
        }
        if (injected)
        {
            ServiceEvidence("RVT_EVIDENCE", "[RVT_EVIDENCE] ", injectionEvent);
            ServiceEvidence("RVT_EVIDENCE", "[RVT_EVIDENCE] ", failureEvent);
        }
    }
    retired.erase(std::remove_if(retired.begin(), retired.end(), [&](const auto& item)
        { return item.first <= completedEpoch; }), retired.end());
    stagingRetired.erase(std::remove_if(stagingRetired.begin(), stagingRetired.end(), [&](const auto& item)
        { return item.first <= completedEpoch; }), stagingRetired.end());
    retiredCandidates.erase(std::remove_if(retiredCandidates.begin(), retiredCandidates.end(),
        [&](const auto& item) { return item.first <= completedEpoch; }), retiredCandidates.end());
    retiredInputs.erase(std::remove_if(retiredInputs.begin(), retiredInputs.end(),
        [&](const auto& item) { return item.first <= completedEpoch; }), retiredInputs.end());
    retiredSurfaces.erase(std::remove_if(retiredSurfaces.begin(), retiredSurfaces.end(),
        [&](const auto& item) { return item.first <= completedEpoch; }), retiredSurfaces.end());
}

void TerrainCompositedService::Impl::RecordProduction(vk::CommandBuffer command, uint64_t completedEpoch)
{
    if (!runtime || productionTasks.empty())
        return;
    auto requests = runtime->ProductionRequests();
    const auto available = std::find_if(productionTasks.begin(), productionTasks.end(),
        [](const auto& task) { return !task.page; });
    if (available == productionTasks.end())
        return;
    const auto eligible = [&](const auto& token) {
        const bool candidateTask = runtime->CandidateToken(token);
        if (worldCandidate && !candidateTask)
            return false;
        if (candidateTask && (!worldCandidate ||
            worldCandidate->transaction.Phase() != TerrainCompositedSnapshotPhase::Producing))
            return false;
        const auto& inputs = candidateTask ? worldCandidate->surfaces : surfaces;
        const auto logicalId = runtime->LogicalSurfaceId(token.key.surfaceId);
        const auto surface = std::find_if(inputs.begin(), inputs.end(),
            [&](const auto& item) { return item->desc.surfaceId == logicalId; });
        return surface != inputs.end() && !(*surface)->inputPending;
    };
    requests.erase(std::remove_if(requests.begin(), requests.end(),
        [&](const auto& token) { return !eligible(token); }), requests.end());
    productionPriority->Prepare(requests);
    const auto isRoot = [&](const auto& token) {
        const auto& inputs = runtime->CandidateToken(token) ? worldCandidate->surfaces : surfaces;
        const auto logicalId = runtime->LogicalSurfaceId(token.key.surfaceId);
        const auto surface = std::find_if(inputs.begin(), inputs.end(),
            [&](const auto& item) { return item->desc.surfaceId == logicalId; });
        auto width = (*surface)->desc.width;
        auto height = (*surface)->desc.height;
        uint32_t rootMip = 0;
        while (width > 1 || height > 1)
        {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            ++rootMip;
        }
        return token.key.mip == rootMip;
    };
    while (auto decision = productionPriority->Pick(requests, isRoot))
    {
        const auto& token = requests[decision->index];
        const bool candidateTask = runtime->CandidateToken(token);
        const auto& inputs = candidateTask ? worldCandidate->surfaces : surfaces;
        const auto logicalId = runtime->LogicalSurfaceId(token.key.surfaceId);
        const auto surface = std::find_if(inputs.begin(), inputs.end(),
            [&](const auto& item) { return item->desc.surfaceId == logicalId; });
        auto reservation = runtime->ReserveProduction(token, completedEpoch);
        if (!reservation)
        {
            requests.erase(requests.begin() + decision->index);
            continue;
        }
        auto& task = *available;
        task.page = std::move(reservation);
        task.executionLease = frozen->runtime.owner->TryReserve({0, 0, 0, 1},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
        if (!task.executionLease)
        {
            runtime->CompleteProduction(*task.page, completedEpoch, false);
            task.page.reset();
            return;
        }
        auto world = std::make_shared<Baker::AdjacencySnapshot>();
        world->sourceSurfaceId = logicalId;
        world->graph = adjacency;
        for (const auto& neighbor : inputs)
        {
            auto input = neighbor->input;
            input.surfaceId = neighbor->desc.surfaceId;
            input.adjacency.reset();
            world->surfaces.push_back({neighbor->desc.surfaceId, std::move(input)});
            task.retained.insert(task.retained.end(), neighbor->retained.begin(), neighbor->retained.end());
        }
        task.owner = productionOwner;
        task.inputOwner = candidateTask ? std::static_pointer_cast<void>(worldCandidate) :
            std::static_pointer_cast<void>(currentInputs);
        task.candidateRevision = candidateTask ? worldCandidate->transaction.Revision() : 0;
        task.producer = candidateTask ? worldCandidate->producer : currentProducer;
        Require(bool(task.producer), "production task has no selected immutable producer");
        task.snapshot = std::move(world);
        Baker::PageRequest request;
        request.virtualWidth = (*surface)->desc.width;
        request.virtualHeight = (*surface)->desc.height;
        request.virtualMip = token.key.mip;
        request.pageX = token.key.x;
        request.pageY = token.key.y;
        request.areaSamplesPerAxis = token.key.mip == 0 ? 1 : 4;
        const auto index = static_cast<uint32_t>(available - productionTasks.begin());
        std::string error;
        command.resetQueryPool(task.timing->pool, 0, 4);
        command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, task.timing->pool, 0);
        Require(baker->Record(command, index, *task.snapshot, request,
            {task.output->buffer, 0, Baker::OutputBytes},
            task.scratch ? vk::DescriptorBufferInfo(task.scratch->buffer, 0, task.scratch->bytes) :
                vk::DescriptorBufferInfo{}, task.producer, error), "GPU production failed: " + error);
        command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, task.timing->pool, 1);
        command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, task.timing->pool, 2);
        runtime->RecordProductionCopy(command, task.output->buffer, *task.page);
        command.copyBuffer(task.output->buffer, task.status->buffer,
            vk::BufferCopy(Baker::StatusOffset, 0, sizeof(uint32_t)));
        vk::MemoryBarrier host(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
        command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, task.timing->pool, 3);
        task.recorded = true;
        productionPriority->CommitSuccessful(requests, *decision);
        return;
    }
}

void TerrainCompositedService::Impl::SnapshotStats()
{
    std::map<const Texture*, std::shared_ptr<Texture>> sourceTextures;
    uint64_t cacheBytes = 0;
    for (const auto& surface : surfaces)
    {
        for (const auto& source : surface->sources)
            if (source)
                sourceTextures.emplace(source.get(), source);
        if (surface->localWeights)
            sourceTextures.emplace(surface->localWeights.get(), surface->localWeights);
        if (surface->authorWeights)
            sourceTextures.emplace(surface->authorWeights.get(), surface->authorWeights);
        for (const auto& material : surface->materials)
            for (const char* name : {"globalColour", "globalMasks", "layerWeightMaps"})
            {
                const auto source = material->GetTexture(name);
                if (source)
                    sourceTextures.emplace(source.get(), source);
            }
        cacheBytes += surface->inputBytes + surface->height.totalBytes;
    }
    cacheBytes += runtime ? runtime->Statistics().gpuBytes : 0;
    uint64_t sourceBytes = 0;
    uint64_t unknownSourceTextures = 0;
    for (const auto& source : sourceTextures)
    {
        const uint64_t bytes = SourceTextureBytes(backend, *source.second);
        sourceBytes += bytes;
        if (!bytes)
            ++unknownSourceTextures;
    }
    std::ostringstream output;
    output << "TerrainVT generation=" << generation << " surfaces=" << surfaces.size()
        << " worldGpuCap=" << WorldGpuLimit << " worldCpuCap=" << WorldCpuLimit
        << " gpuBudgetScope=cross-world-physical-input-workspace"
        << " cacheBytes=" << cacheBytes << " sourceBytes=" << sourceBytes
        << " sourceTextures=" << sourceTextures.size()
        << " unknownSourceTextures=" << unknownSourceTextures
        << " accountedGpuPayloadBytes=" << cacheBytes + sourceBytes
        << " importedWeightCpuBytes=" << importedWeightBytes
        << " cpuWorkspaceAdmissionPeak=" << cpuWorkspacePeak
        << " gpuTransientAdmissionPeak=" << gpuTransientPeak
        << " fineDiskCache=disabled productionSlots=" << productionTasks.size()
        << " budgetInsufficient=" << (runtime ? runtime->Statistics().budgetInsufficient : false)
        << " queueEffectiveMipBias=" << (runtime ? runtime->Statistics().effectiveMipBias : 0)
        << " shaderRequestMipBias=0"
        << " qualityReadiness=unverified"
        << " staleBakeResults=" << staleBakeResults;
    for (const auto& surface : surfaces)
    {
        const auto stats = runtime->Statistics();
        const auto& resident = stats.surfaces.at(surface->desc.surfaceId);
        const auto currentToken = runtime->Token({surface->desc.surfaceId, 0, 0, 0});
        output << "\n  " << surface->desc.surfaceId << " generation=" << surface->generation
            << " revision=" << surface->revision
            << " sourceRevision=" << surface->sourceRevision
            << " currentContentHash=" << currentToken.contentHash.ToHex()
            << " productionInputIdentity=" << surface->input.contentIdentity
            << " resident=" << resident.resident << "/" << frozen->runtime.surface.slots
            << " retiring=" << resident.retiring
            << " budgetSelectedPages=" << resident.budgetSelectedPages
            << " budgetMipBias=" << resident.budgetMipBias
            << " readsStarted=" << resident.readsStarted
            << " queuedCancel=" << resident.queuedCancel
            << " evictedReads=" << resident.evictedReads
            << " queued=" << resident.pending << " reading=" << resident.reading
            << " cpuBytes=" << resident.cpuBytes << " gpuBytes=" << stats.gpuBytes
            << " stagingBytes=" << stats.stagingBytes << " inputBytes=" << surface->inputBytes
            << " heightBytes=" << surface->height.totalBytes << " dirty=" << resident.dirty
            << " stale=" << resident.stale << " resolveParentFallbacks=" << resident.parentFallbacks
            << " resolveMisses=" << resident.misses << " discarded=" << resident.discardedResults
            << " readFailures=" << resident.readFailures
            << " feedbackEstimate=cpu-current-mapping"
            << " feedbackRequests=" << resident.feedbackRequests
            << " feedbackEstimatedExactHits=" << resident.feedbackEstimatedExactHits
            << " feedbackEstimatedParentFallbacks=" << resident.feedbackEstimatedParentFallbacks
            << " feedbackEstimatedMisses=" << resident.feedbackEstimatedMisses
            << " feedbackEstimatedStaleHits=" << resident.feedbackEstimatedStaleHits;
        const auto histogram = [&](const char* name, const std::vector<uint64_t>& counts)
        {
            output << " " << name << "=";
            for (size_t mip = 0; mip < counts.size(); ++mip)
            {
                if (mip != 0)
                    output << ",";
                output << mip << ":" << counts[mip];
            }
        };
        histogram("feedbackTargetMipCounts", resident.feedbackTargetMipCounts);
        histogram("feedbackEstimatedResolvedMipCounts", resident.feedbackEstimatedResolvedMipCounts);
    }
    std::lock_guard<std::mutex> lock(mutex);
    stats = output.str();
}
} // namespace VL
