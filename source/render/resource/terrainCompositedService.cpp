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
TerrainCompositedService::TerrainCompositedService(
    RendererBackendVulkan& backend, PipelineFactory& pipelineFactory, TerrainStreamingState& streaming)
    : impl(std::make_unique<Impl>(backend, pipelineFactory, streaming))
{
    static std::once_flag providerInstalled;
    std::call_once(providerInstalled, [] {
        SetFrameSubmitFaultProvider([](const std::shared_ptr<TerrainCompositedService>& service,
            uint64_t world, uint64_t epoch) -> std::optional<FrameSubmitFault> {
            auto& state = *service->impl;
            if (!state.recording || !state.faultSubmitHook || state.worldGeneration != world || !epoch)
                return {};
            for (const auto& task : state.productionTasks)
            {
                if (!task.page || !task.recorded || task.epoch)
                    continue;
                const auto& token = task.page->token;
                TerrainFaultSubmitObservation observation{world,
                    {state.runtime->LogicalSurfaceId(token.key.surfaceId), token.key.mip,
                        token.key.x, token.key.y, token.revision},
                    {token.worldGeneration, token.contentHash.ToHex(), epoch}};
                const auto consumed = TerrainFaultInjection().ConsumeBeforeSubmit(
                    state.faultSubmitHook, observation);
                if (!consumed)
                    continue;
                struct Owner
                {
                    std::optional<TerrainRvtBudgetLease> metadata;
                    std::shared_ptr<TerrainCompositedService> service;
                    std::shared_ptr<void> input;
                    Baker::ProducerSnapshot producer;
                    TerrainFaultSubmitAbortObservation result;
                    uint64_t abortedBefore = 0;
                    uint64_t commitsBefore = 0;
                };
                auto metadata = state.frozen->runtime.owner->TryReserve({0, 4096, 0, 0},
                    TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
                Require(metadata.has_value(), "submit fault observation exceeds CPU workspace");
                auto owner = std::make_shared<Owner>();
                owner->metadata = std::move(*metadata);
                owner->service = service;
                owner->input = task.inputOwner;
                owner->producer = task.producer;
                owner->result.abortTicket = consumed->abortTicket;
                owner->result.task = std::move(observation);
                owner->result.activeGenerationBefore = world;
                owner->abortedBefore = state.abortedProductionTasks;
                owner->commitsBefore = state.successfulFrameCommits;
                for (const auto& recorded : state.productionTasks)
                    if (recorded.page && recorded.recorded && !recorded.epoch)
                        ++owner->result.recordedTasks;
                FrameSubmitFault fault;
                fault.registration = state.faultSubmitHook;
                fault.failure = *consumed;
                fault.task = owner->result.task;
                fault.ownerLease = owner;
                fault.observeAfterAbort = [owner] {
                    const auto& current = *owner->service->impl;
                    Require(!current.recording, "submit fault observer ran before Service Abort");
                    Require(current.successfulFrameCommits == owner->commitsBefore,
                        "submit fault scene committed before Abort observation");
                    owner->result.activeGenerationAfter = current.worldGeneration;
                    owner->result.abortedTasks = static_cast<uint32_t>(
                        current.abortedProductionTasks - owner->abortedBefore);
                    owner->result.publishedChannels = 0;
                    owner->result.backendSubmitCalls = 0;
                    return owner->result;
                };
                return fault;
            }
            return {};
        });
    });
}

TerrainCompositedService::~TerrainCompositedService() = default;

void TerrainCompositedService::SetShaderReloadCoordinator(ShaderReloadCoordinator* coordinator)
{
    if (!coordinator)
        impl->ResetFaultHooks();
    else
        impl->RegisterActiveFaultHooks();
    impl->shaderReloadCoordinator = coordinator;
    if (impl->baker)
        impl->baker->SetShaderReloadCoordinator(coordinator);
}

void TerrainCompositedService::ConfigureWorldDescriptorBudget(RendererResourceCache& cache)
{
    Require(impl->frozen.has_value() && bool(impl->descriptorMetadataReserve),
        "World descriptor configuration requires the existing metadata reserve");
    cache.ConfigureWorldDescriptorBudget(impl->worldGeneration,
        {impl->frozen->runtime.owner, impl->descriptorMetadataReserve});
}

void TerrainCompositedService::SetWorldGeneration(uint64_t generation)
{
    Require(!impl->frozen, "World generation must precede terrain preparation");
    impl->ResetFaultHooks();
    impl->worldGeneration = generation;
}

void TerrainCompositedService::SetPhase(TerrainRvtResourcePhase phase)
{
    if (phase == TerrainRvtResourcePhase::Active)
        GlobalWorldHasBeenActivated.store(true, std::memory_order_release);
    if (!impl->frozen)
        return;
    impl->resourcePhase = phase;
    impl->frozen->runtime.owner->SetPhase(phase);
    if (impl->geometryOwner)
        impl->geometryOwner->SetPhase(phase);
}

std::shared_ptr<TerrainRvtBudgetOwner> TerrainCompositedService::BudgetOwner() const
{
    return impl->frozen ? impl->frozen->runtime.owner : nullptr;
}

std::shared_ptr<TerrainRvtBudgetOwner> TerrainCompositedService::DiagnosticReferencePoolOwner() const
{
    Require(impl->frozen && impl->peakOwner, "Diagnostic reference requires actual prepared peak/outer owners");
    return std::make_shared<TerrainRvtBudgetOwner>(impl->frozen->steadyCaps,
        impl->peakOwner, impl->frozen->terrainOwner);
}

TerrainRvtBudgetLease TerrainCompositedService::AdmitCpuWorkspace(uint64_t bytes)
{
    Require(bool(impl->geometryOwner) && bytes, "CPU geometry workspace requires prepared owner");
    auto lease = impl->geometryOwner->TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "terrain CPU construction exceeds steady/outer peak budget");
    return std::move(*lease);
}

void TerrainCompositedService::SetCookMode(bool enabled) { GlobalCookMode.store(enabled); }

bool TerrainCompositedService::CookMode() { return GlobalCookMode.load(); }

void TerrainCompositedService::SetAuthoringMode(bool enabled) { GlobalAuthoringMode.store(enabled); }

bool TerrainCompositedService::IsAuthoringMode() { return GlobalAuthoringMode.load(); }

void TerrainCompositedService::SetFrozenConfig(TerrainCompositedFrozenConfig config)
{
    Require(bool(config.runtime.owner) && config.runtime.frameRingSize &&
        config.runtime.uploadsPerFrame && config.runtime.uploadBytesPerFrame &&
        config.runtime.gpuBytes && config.runtime.cpuBytes &&
        config.runtime.schedulingMicroseconds && config.allocations.image &&
        config.allocations.transfer, "incomplete P0 frozen configuration/allocation queries");
    std::lock_guard<std::mutex> lock(FrozenConfigMutex);
    Require(!FrozenConfig, "P0 frozen configuration cannot be changed during execution");
    FrozenConfig = std::move(config);
}

void TerrainCompositedService::InitializeFrozenConfig(RendererBackendVulkan& backend)
{
    {
        std::lock_guard<std::mutex> lock(FrozenConfigMutex);
        if (FrozenConfig)
            return;
    }
    const auto bytes = ReadBinaryFile(
        std::filesystem::path(CommonFunction::GetProjectPath()) / "config/terrainRvtP0Frozen.json");
    constexpr const char* approvedHash =
        "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd";
    const auto actualHash = TerrainPaintSha256Hex(bytes);
    Require(actualHash == approvedHash, "P0 frozen bytes differ from independently approved SHA256");
    const auto sidecar = ReadBinaryFile(
        std::filesystem::path(CommonFunction::GetProjectPath()) / "config/terrainRvtP0Frozen.json.sha256");
    std::istringstream signature(std::string(sidecar.begin(), sidecar.end()));
    std::string recordedHash;
    signature >> recordedHash;
    Require(recordedHash == approvedHash, "P0 frozen SHA256 sidecar differs from approved receipt");
    std::cout << "[TerrainRvtFrozen] sha256=" << actualHash
        << " independentAudit=approved cache=disabled" << std::endl;
    const auto document = nlohmann::json::parse(bytes.begin(), bytes.end());
    Require(document.at("schemaVersion") == 1 && document.at("frozen") == true,
        "unsupported/unfrozen P0 configuration");
    TerrainCompositedFrozenConfig config;
    config.steadyCaps = FrozenCaps(document.at("steadyRvtCaps"));
    const auto peak = FrozenCaps(document.at("peakRvtOwnerCaps"));
    config.peakCaps = peak;
    const auto& total = document.at("terrainTotalCaps");
    config.terrainSteadyCaps = TerrainCaps(total.at("steadyGpuBytes").get<uint64_t>(),
        total.at("steadyCpuBytes").get<uint64_t>(), config.steadyCaps);
    config.terrainOwner = std::make_shared<TerrainRvtBudgetOwner>(TerrainCaps(
        total.at("candidateActiveRetiringGpuBytes").get<uint64_t>(),
        total.at("candidateActiveRetiringCpuBytes").get<uint64_t>(), peak));
    const char* evidenceEnabled = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
    if (evidenceEnabled && *evidenceEnabled)
    {
        std::lock_guard<std::mutex> lock(EvidenceBudgetMutex);
        if (!EvidenceProcessLease)
        {
            const auto quote = RuntimeEvidence::CostQuote();
            auto lease = config.terrainOwner->TryReserve({quote.gpuBytes, quote.cpuBytes, 0, 0},
                TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
            Require(lease.has_value(), "runtime evidence process workspace exceeds outer terrain CPU budget");
            RuntimeEvidence::Initialize(std::filesystem::path(CommonFunction::GetProjectPath()));
            EvidenceProcessLease.emplace(std::move(*lease));
        }
    }
    if (RuntimeFileTrace::IsEnabled())
    {
        std::lock_guard<std::mutex> lock(EvidenceBudgetMutex);
        if (!FileTraceProcessLease)
        {
            const auto quote = RuntimeFileTrace::CostQuote();
            auto lease = config.terrainOwner->TryReserve({quote.gpuBytes, quote.cpuBytes, 0, 0},
                TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
            if (!lease)
                RuntimeFileTrace::MarkIncomplete();
            Require(lease.has_value(), "runtime file trace process workspace exceeds outer CPU budget");
            FileTraceProcessLease.emplace(std::move(*lease));
        }
    }
    config.geometryGpuBytes = total.at("geometryGpuBytesPerWorld").get<uint64_t>();
    config.geometryCpuBytes = total.at("geometryCpuBytesPerWorld").get<uint64_t>();
    const auto& reserves = document.at("workspaceReserves");
    config.metadataGpuBytes = reserves.at("descriptorDriverMetadataGpuBytesPerWorld").get<uint64_t>();
    const auto& targets = document.at("targets");
    config.initialQualityLimitMs = targets.at("maxProcessToInteractiveSeconds").get<double>() * 1000.0;
    config.poseQualityLimitMs = targets.at("maxPoseOrTeleportConvergenceSeconds").get<double>() * 1000.0;
    auto& runtime = config.runtime;
    runtime.owner = std::make_shared<TerrainRvtBudgetOwner>(peak);
    runtime.gpuBytes = config.steadyCaps.totalGpuBytes;
    runtime.cpuBytes = config.steadyCaps.totalCpuBytes;
    runtime.frameRingSize = reserves.at("uploadRingSlots").get<uint32_t>();
    Require(runtime.frameRingSize == backend.GetFrameFenceCount(),
        "backend frame count differs from frozen upload ring");
    runtime.uploadBytesPerFrame = targets.at("uploadBytesPerFrame").get<uint64_t>();
    runtime.uploadsPerFrame = targets.at("uploadPagesPerFrame").get<uint32_t>();
    runtime.schedulingMicroseconds = 2000;
    runtime.freeSlotReserve = config.steadyCaps.productionTasks;
    runtime.surface.slots = static_cast<uint32_t>(config.steadyCaps.physicalSlots);
    runtime.surface.uploadsPerFrame = runtime.uploadsPerFrame;
    runtime.surface.uploadBytesPerFrame = runtime.uploadBytesPerFrame;
    runtime.surface.queuedRequests = reserves.at("queueEntriesGlobal").get<uint32_t>();
    runtime.surface.cpuBytes = reserves.at("optionalCpuPageCacheBytes").get<uint64_t>() / 16;
    auto& production = config.production;
    production.physicalSlots = runtime.surface.slots;
    production.productionSlots = targets.at("maxGlobalInFlightProductionTasks").get<uint32_t>();
    production.maxTrackedPages = runtime.surface.queuedRequests;
    production.maxAttempts = runtime.surface.maxReadAttempts;
    production.retryBaseTicks = 2;
    production.retryMaxTicks = 64;
    production.agePromotionTicks = 30;
    production.freeSlotReserve = runtime.freeSlotReserve;
    config.packagedMinMip = targets.at("finestPublishedCoarseMip").get<uint32_t>();
    config.allocations = TerrainCompositedProductionGpu::MakeAllocationQuery(
        backend, *runtime.owner, FrozenOverheads());
    SetFrozenConfig(std::move(config));
}

TerrainRvtBudgetLease TerrainCompositedService::AdmitGeometry(
    uint64_t vertexBytes, uint64_t indexBytes, uint64_t patchBytes, bool dynamicVertices,
    std::optional<TerrainRvtBudgetLease>& uploadLease)
{
    Require(bool(impl->geometryOwner), "geometry admission precedes frozen World preparation");
    const auto query = [&](uint64_t bytes, vk::BufferUsageFlags usage)
    {
        return bytes ? impl->backend.QueryBufferMemoryRequirements(bytes, usage).size : 0;
    };
    const auto vertexUsage = vk::BufferUsageFlagBits::eVertexBuffer |
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;
    const auto indexUsage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    const uint64_t vertexAllocation = dynamicVertices ?
        query(vertexBytes, vk::BufferUsageFlagBits::eVertexBuffer) * RenderableObject::DynamicMeshFrameSlotCount :
        query(vertexBytes, vertexUsage);
    const uint64_t staging = std::max({
        dynamicVertices ? uint64_t(0) : query(vertexBytes, vk::BufferUsageFlagBits::eTransferSrc),
        query(indexBytes, vk::BufferUsageFlagBits::eTransferSrc),
        query(patchBytes, vk::BufferUsageFlagBits::eTransferSrc)});
    TerrainRvtCost cost;
    const uint64_t instanceAllocation = query(sizeof(InstanceVertex), vk::BufferUsageFlagBits::eVertexBuffer);
    cost.gpuBytes = vertexAllocation + query(indexBytes, indexUsage) +
        query(patchBytes, indexUsage) + instanceAllocation;
    cost.cpuBytes = vertexBytes + indexBytes +
        (dynamicVertices ? vertexAllocation : 0) + instanceAllocation + sizeof(RenderableObject);
    auto lease = impl->geometryOwner->TryReserve(cost,
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "terrain geometry exceeds steady/outer peak allocation budget");
    if (staging)
    {
        uploadLease = impl->geometryOwner->TryReserve({staging, staging, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(uploadLease.has_value(), "terrain geometry staging exceeds steady/outer peak budget");
    }
    return std::move(*lease);
}

void TerrainCompositedService::PrepareWorld(const std::vector<TerrainBuildPlan>& plans)
{
    if (plans.empty())
        return;
    impl->interactiveReady.store(false, std::memory_order_release);
    Require(!impl->frozen, "World production inputs were already prepared");
    impl->frozen = ReadFrozenConfig();
    auto& frozen = *impl->frozen;
    if (!CookMode())
    {
        const bool initialWorld = !GlobalWorldHasBeenActivated.load(std::memory_order_acquire);
        const double processStartedMs = ProcessStartedMonotonicMs();
        impl->qualityClock.Start(initialWorld, processStartedMs,
            frozen.initialQualityLimitMs, frozen.poseQualityLimitMs);
        ServiceEvidence("RVT_QUALITY_CLOCK", "[RVT_QUALITY_CLOCK] ", nlohmann::json{
            {"worldGeneration", impl->worldGeneration}, {"initialWorld", initialWorld},
            {"processStartedMonotonicMs", processStartedMs},
            {"initialLimitMs", frozen.initialQualityLimitMs}, {"poseLimitMs", frozen.poseQualityLimitMs},
            {"deadlineMonotonicMs", impl->qualityClock.DeadlineMs()}});
    }
    auto terrainWorld = std::make_shared<TerrainRvtBudgetOwner>(
        frozen.terrainSteadyCaps, frozen.terrainOwner);
    impl->peakOwner = frozen.runtime.owner;
    frozen.runtime.owner = std::make_shared<TerrainRvtBudgetOwner>(
        frozen.steadyCaps, frozen.runtime.owner, terrainWorld);
    auto geometryCaps = TerrainCaps(frozen.geometryGpuBytes, frozen.geometryCpuBytes, frozen.steadyCaps);
    impl->geometryOwner = std::make_shared<TerrainRvtBudgetOwner>(geometryCaps, terrainWorld);
    uint64_t planBytes = sizeof(uint64_t) + sizeof(TerrainCompositedQualityClock) +
        sizeof(TerrainCompositedSourceSnapshots) + sizeof(impl->preparedSourcePlans);
    for (const auto& plan : plans)
    {
        const auto& desc = plan.desc;
        uint64_t descriptorBytes = sizeof(TerrainBuildPlan) + plan.placementId.capacity() +
            desc.assetPath.capacity() + desc.name.capacity() + desc.heightmapPath.capacity() +
            desc.paintPath.capacity() + desc.materialInstancePath.capacity() +
            desc.detailMaterialInstancePath.capacity() + desc.compositedVT.packagePath.capacity() +
            desc.compositedVT.productionSourcePath.capacity() +
            desc.layers.capacity() * sizeof(TerrainLayerDesc) +
            (desc.lodDistancesCm.capacity() + desc.chunkResidencyDistancesCm.capacity()) * sizeof(float);
        for (const auto& layer : desc.layers)
            descriptorBytes += layer.name.capacity() + layer.key.capacity();
        planBytes += descriptorBytes * 4;
    }
    if (planBytes)
        impl->planLeases.push_back(AdmitCpuWorkspace(planBytes));
    impl->sourceSnapshots = std::make_unique<TerrainCompositedSourceSnapshots>(impl->geometryOwner);
    for (const auto& plan : plans)
        Require(impl->preparedSourcePlans.emplace(TerrainPlacementIdentity(plan),
            impl->sourceSnapshots->Retain(plan)).second, "duplicate initial pixel source placement");
    frozen.allocations = TerrainCompositedProductionGpu::MakeAllocationQuery(
        impl->backend, *frozen.runtime.owner, FrozenOverheads());
    Require(frozen.metadataGpuBytes >= TerrainQualityAbi::FrameMetadataGpuBytes,
        "frozen metadata reserve cannot cover quality/loading components");
    const uint64_t serviceMetadataGpuBytes =
        frozen.metadataGpuBytes - TerrainQualityAbi::FrameMetadataGpuBytes;
    auto metadata = frozen.runtime.owner->TryReserve(
        {serviceMetadataGpuBytes, 0, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(metadata.has_value(), "terrain metadata exceeds frozen budget");
    impl->descriptorMetadataReserve =
        std::make_shared<const TerrainRvtBudgetLease>(std::move(*metadata));
    Require(impl->frozen->runtime.frameRingSize == impl->backend.GetFrameFenceCount(),
        "frozen frame ring differs from backend");
    impl->WorldGpuLimit = impl->frozen->runtime.owner->Caps().totalGpuBytes;
    impl->WorldCpuLimit = impl->frozen->runtime.owner->Caps().totalCpuBytes;
    impl->worldManifest = ManifestPath(plans.front());
    const auto source = plans.front().desc.compositedVT.productionSourcePath;
    for (const auto& plan : plans)
    {
        Require(ManifestPath(plan) == impl->worldManifest, "all terrains must select one World manifest");
        Require(impl->expectedSurfaces.insert(TerrainPlacementIdentity(plan)).second,
            "duplicate World placement identity");
        if (IsAuthoringMode())
            Require(!source.empty() && plan.desc.compositedVT.productionSourcePath == source,
                "Editor/Cook requires one explicit productionSourcePath");
    }
    if (IsAuthoringMode())
    {
        auto sourcePlan = plans.front();
        sourcePlan.desc.compositedVT.packagePath = source;
        const auto authorPath = ManifestPath(sourcePlan);
        Require(std::filesystem::is_regular_file(authorPath) &&
            !std::filesystem::is_symlink(authorPath), "missing/linked World author recipe");
        const auto bytes = ReadBinaryFile(authorPath);
        impl->authorRecipe = nlohmann::json::parse(bytes.begin(), bytes.end());
        impl->adjacency = ParseTerrainCompositedAuthorRecipe(
            impl->authorRecipe, impl->expectedSurfaces);
    }
    impl->runtime = std::make_unique<TerrainCompositedRuntime>(impl->backend, impl->frozen->runtime);
    if (!CookMode())
    {
        impl->store = std::make_shared<TerrainCompositedVTStore>(
            TerrainCompositedVTStore::Open(impl->worldManifest));
        impl->ValidateProductionGenerator();
        std::set<std::string> ids;
        for (const auto& desc : impl->store->Surfaces())
            ids.insert(desc.surfaceId);
        Require(ids == impl->expectedSurfaces, "World publication surface set differs from scene");
        const auto imageQuote = [&](uint32_t width, uint32_t height, uint32_t layers, vk::Format format)
        {
            vk::ImageCreateInfo info;
            info.setImageType(vk::ImageType::e2D).setExtent({width, height, 1})
                .setMipLevels(1).setArrayLayers(layers).setFormat(format)
                .setSamples(vk::SampleCountFlagBits::e1).setTiling(vk::ImageTiling::eOptimal)
                .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
                .setSharingMode(vk::SharingMode::eExclusive);
            return impl->backend.QueryImageMemoryRequirements(info).size;
        };
        uint64_t tables = 0;
        for (const auto& description : impl->store->Surfaces())
        {
            ValidateTerrainCompositedProductionOutput(description);
            const auto layout = description.Layout();
            tables += imageQuote(layout.Mip(0).pagesX, layout.Mip(0).pagesY,
                layout.MipCount(), vk::Format::eR32G32B32A32Sfloat);
        }
        const auto bufferQuote = [&](uint64_t bytes, vk::BufferUsageFlags usage)
        { return impl->backend.QueryBufferMemoryRequirements(bytes, usage).size; };
        const auto scratchExtent = Baker::RequiredScratchBytes(4);
        const uint64_t scratch = scratchExtent ? bufferQuote(scratchExtent,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer) : 0;
        const uint64_t output = bufferQuote(Baker::OutputBytes, vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst);
        const uint64_t parameters = (frozen.production.productionSlots + 1) *
            (bufferQuote(Baker::WorldParameterBytes, vk::BufferUsageFlagBits::eStorageBuffer) +
             bufferQuote(uint64_t(64) * ids.size(), vk::BufferUsageFlagBits::eStorageBuffer));
        const uint64_t feedbackReserve = impl->backend.GetFrameFenceCount() *
            (bufferQuote(TerrainCompositedFeedbackBufferBytes, vk::BufferUsageFlagBits::eTransferDst) +
             (CookMode() ? bufferQuote(uint64_t(TerrainFeedbackResolution) * TerrainFeedbackResolution *
                TerrainFeedbackTexelBytes, vk::BufferUsageFlagBits::eTransferDst) : 0));
        const uint64_t workspace = frozen.runtime.owner->Statistics().byClass[
            static_cast<size_t>(TerrainRvtCostClass::Workspace)].gpuBytes +
            TerrainQualityAbi::FrameMetadataGpuBytes + tables +
            (scratch + output) * frozen.production.productionSlots + parameters + feedbackReserve;
        Require(workspace <= frozen.steadyCaps.workspaceGpuBytes,
            "World page tables/production scratch/ring/metadata exceed frozen workspace before input restore");
        std::cout << "[TerrainRvtAdmission] workspacePreflightBytes=" << workspace
            << " cap=" << frozen.steadyCaps.workspaceGpuBytes
            << " serviceMetadataGpuBytes=" << serviceMetadataGpuBytes
            << " deferredFrameMetadataGpuBytes=" << TerrainQualityAbi::FrameMetadataGpuBytes
            << " uploadRingBytesPerSlot=1048576" << std::endl;
        impl->productionOwner = TerrainCompositedProductionGpu::Restore(
            impl->backend, *impl->store, *impl->frozen->runtime.owner, impl->frozen->allocations,
            {}, impl->worldScope);
        impl->currentInputs = std::make_shared<Impl::WorldLeafOwner>();
        const uint64_t leafMetadata = TerrainCompositedProductionGpuResources::PhaseUse::MetadataBytes() *
            (impl->store->Production().objects.size() + impl->store->Production().surfaces.size());
        impl->currentInputs->metadata = frozen.runtime.owner->TryReserve(
            {0, leafMetadata, 0, 0}, TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(impl->currentInputs->metadata.has_value(), "World leaf-use metadata exceeds CPU budget");
        for (const auto& object : impl->store->Production().objects)
            if (object.kind == TerrainCompositedProductionObjectKind::Image)
            {
                const auto image = impl->productionOwner->Image(object.objectId);
                impl->inputAllocationCosts[image.get()] =
                    impl->productionOwner->ImageAllocation(object.objectId).actualCost;
                impl->inputAllocations[image.get()] = image;
                impl->currentInputs->textures.push_back(image);
                impl->currentInputs->uses.emplace(image.get(),
                    impl->productionOwner->RetainImageUse(object.objectId, TerrainRvtResourcePhase::Candidate));
            }
        for (const auto& surface : impl->store->Production().surfaces)
        {
            const auto image = impl->productionOwner->SourceMap(surface.surfaceId);
            impl->inputAllocationCosts[image.get()] =
                impl->productionOwner->SourceMapAllocation(surface.surfaceId).actualCost;
            impl->inputAllocations[image.get()] = image;
            impl->currentInputs->textures.push_back(image);
            impl->currentInputs->uses.emplace(image.get(),
                impl->productionOwner->RetainSourceMapUse(surface.surfaceId, TerrainRvtResourcePhase::Candidate));
        }
        const auto& first = impl->store->Production().surfaces.front().inputs;
        auto mappingLease = frozen.runtime.owner->TryReserve({0, 32 * 32 * 4, 0, 0},
            TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
        Require(mappingLease.has_value(), "World coverage mapping exceeds CPU input budget");
        impl->coveragePageMap = impl->store->ReadInput(first.coveragePageTable);
        Require(impl->coveragePageMap.size() == 32 * 32 * 4,
            "World coverage mapping does not match its frozen32x32 schema");
        impl->inputLeases.push_back(std::move(*mappingLease));
        for (const auto& surface : impl->store->Production().surfaces)
            Require(surface.inputs.coveragePageTable == first.coveragePageTable &&
                surface.inputs.coverageIds == first.coverageIds &&
                surface.inputs.coverageWeights == first.coverageWeights &&
                surface.inputs.layerCatalog == first.layerCatalog,
                "World publication must share one coverage/catalog owner");
        auto& resources = impl->streaming.pageResources;
        resources.pageTable = impl->productionOwner->Image(first.coveragePageTable);
        resources.layerIds = impl->productionOwner->Image(first.coverageIds);
        resources.layerWeights = impl->productionOwner->Image(first.coverageWeights);
        impl->streaming.layerResources.layerTable = impl->productionOwner->Image(first.layerCatalog);
        impl->streaming.feedbackRing.reset();
    }
}

void TerrainCompositedService::PrepareMaterialInputs(
    const TerrainBuildPlan& plan, RendererResourceLoadContext& context)
{
    if (CookMode())
        return;
    Require(bool(impl->productionOwner), "World inputs must precede terrain material loading");
    const auto id = TerrainPlacementIdentity(plan);
    const auto& closure = impl->productionOwner->Closure();
    const auto found = std::find_if(closure.surfaces.begin(), closure.surfaces.end(),
        [&](const auto& surface) { return surface.surfaceId == id; });
    Require(found != closure.surfaces.end(), "terrain placement absent from production owner");
    const auto& inputs = found->inputs;
    std::map<std::string, std::shared_ptr<Texture>> bindings;
    const std::pair<const char*, const std::string*> names[] = {
        {"pageTable", &inputs.coveragePageTable}, {"layerIds", &inputs.coverageIds},
        {"layerWeights", &inputs.coverageWeights}, {"layerTable", &inputs.layerCatalog},
        {"layerAlbedo", &inputs.layerAlbedo}, {"layerNormal", &inputs.layerNormal},
        {"layerPbr", &inputs.layerOrm}, {"layerHeight", &inputs.layerHeight},
        {"layerWeightMaps", &inputs.localLayerWeights}, {"globalColour", &inputs.globalColour},
        {"globalMasks", &inputs.globalMasks}, {"terrainHeightAtlas", &inputs.heightAtlas},
        {"terrainHeightLayerMap", &inputs.heightLayerMap}};
    for (const auto& name : names)
        if (!name.second->empty())
            bindings.emplace(name.first, impl->productionOwner->Image(*name.second));
    const auto sourceMap = impl->productionOwner->SourceMap(id);
    impl->sourceMaps.emplace(id, sourceMap);
    bindings.emplace("terrainLayerSourceMap", sourceMap);
    const auto& desc = impl->store->Surface(id);
    impl->runtime->RegisterSurface(impl->store, desc, impl->generation, 0);
    const auto materialPage = impl->runtime->Binding(id);
    bindings.emplace("compositedPageTable", materialPage.compositedPageTable);
    bindings.emplace("compositedAlbedo", materialPage.compositedAlbedo);
    bindings.emplace("compositedNormal", materialPage.compositedNormal);
    bindings.emplace("compositedOrm", materialPage.compositedOrm);
    for (const char* name : {"layerHeight", "layerWeightMaps", "terrainHeightAtlas"})
        if (!bindings.count(name))
            bindings.emplace(name, bindings.at("layerPbr"));
    for (const char* name : {"globalColour", "globalMasks", "terrainHeightLayerMap"})
        if (!bindings.count(name))
            bindings.emplace(name, bindings.at("layerTable"));
    const auto load = [&](const std::string& authorIdentity, const std::string& runtimePath)
    {
        Require(!runtimePath.empty(), "publication missing runtime material");
        auto temporary = plan;
        const auto resourceRoot = std::filesystem::canonical(Roots().resourceRoot);
        const auto full = impl->worldManifest.parent_path() / runtimePath;
        temporary.desc.compositedVT.packagePath = full.lexically_relative(resourceRoot).generic_string();
        const auto checked = ManifestPath(temporary);
        Require(std::filesystem::is_regular_file(checked) &&
            !std::filesystem::is_symlink(checked), "missing/linked runtime material");
        const auto bytes = ReadBinaryFile(checked);
        const auto key = MaterialAssetUtils::NormalizeAssetPath(authorIdentity);
        Require(!context.runtimeMaterialSources.count(key), "runtime MI reused by terrain placements");
        context.runtimeMaterialSources.emplace(key, nlohmann::json::parse(bytes.begin(), bytes.end()));
        context.preboundMaterialTextures.emplace(key, bindings);
    };
    load(plan.desc.materialInstancePath, found->runtimeMaterialInstancePath);
    if (!plan.desc.detailMaterialInstancePath.empty())
        load(plan.desc.detailMaterialInstancePath, found->runtimeDetailMaterialInstancePath);
}

void TerrainCompositedService::RegisterSurface(
    const TerrainBuildPlan& plan, const Eigen::Matrix4f& model,
    const std::shared_ptr<MaterialInstance>& baseMI, const std::shared_ptr<MaterialInstance>& detailMI)
{
    Require(plan.desc.compositedVT.enabled,
        "terrain requires compositedVT; disabled terrain rendering is unsupported");
    Require(!CookMode() || GlobalAuthoringMode.load(), "Cook requires authoring mode");
    Require(!impl->recording && baseMI && impl->surfaces.size() < 256,
        "invalid surface registration state/count");
    const auto identity = TerrainPlacementIdentity(plan);
    for (const auto& previous : impl->surfaces)
    {
        Require(previous->desc.surfaceId != identity, "duplicate surface registration");
        for (const auto& material : previous->materials)
            Require(material != baseMI && (!detailMI || material != detailMI),
                "each composited surface requires independent MI instances");
    }
    auto surface = std::make_unique<Impl::Surface>();
    Require(bool(impl->sourceSnapshots), "surface pixel owners require a prepared World");
    surface->plan = impl->sourceSnapshots->Retain(plan);
    surface->model = model;
    surface->generation = impl->generation;
    surface->materials.push_back(baseMI);
    if (detailMI)
    {
        for (const auto* parameter : {"u_uvScale", "u_heightBlendStrength", "u_terrainWeightMapMode"})
            Require(Parameter(*baseMI, parameter, 0.f) == Parameter(*detailMI, parameter, 0.f),
                std::string("base/detail static bake parameter differs: ") + parameter);
        surface->materials.push_back(detailMI);
    }
    Require(baseMI->GetBaseMaterial().lock() &&
        baseMI->GetBaseMaterial().lock()->GetShaderName() == "terrain",
        "composited baking supports the existing M_terrain evaluator only");
    auto& desc = surface->desc;
    desc.surfaceId = identity;
    desc.width = static_cast<uint32_t>(plan.desc.compositedVT.virtualResolution.x());
    desc.height = static_cast<uint32_t>(plan.desc.compositedVT.virtualResolution.y());
    desc.generatorVersion = TerrainCompositedGeneratorVersion;
    desc.encoding = TerrainCompositedEncoding;
    desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
        TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
    const auto layout = desc.Layout();
    Require(layout.Mip(0).pagesX <= 64 && layout.Mip(0).pagesY <= 64 &&
        layout.MipCount() <= 14, "surface exceeds feedback encoding dimensions");
    auto config = BuildConfig(*baseMI);
    config["worldLayerMapping"] = impl->streaming.pageManager.PlacementLayerIdMap(identity);
    const auto path = ManifestPath(plan);
    Require(CookMode() || std::filesystem::is_regular_file(path),
        "terrain is not render-ready: missing composited package " + path.string() +
        "; save author data, then run main_editor --terrain-vt-cook --initial-scene <scene> "
        "--exit-after-frames 2 before opening the scene");
    Require(impl->frozen.has_value() && bool(impl->runtime), "World inputs must precede surface registration");
    if (CookMode())
    {
        surface->authorManifest = BuildTerrainCompositedDependencies(Roots(), plan,
            Placement(model), plan.desc.materialInstancePath, config);
        ValidateTerrainCompositedAuthors(Roots(), surface->authorManifest);
        impl->Capture(*surface, baseMI);
    }
    else
    {
        desc = impl->store->Surface(identity);
        Require(desc.width == static_cast<uint32_t>(plan.desc.compositedVT.virtualResolution.x()) &&
            desc.height == static_cast<uint32_t>(plan.desc.compositedVT.virtualResolution.y()),
            "TR virtual layout differs from World publication");
        const auto owned = TerrainCompositedProductionGpu::MakeBakeInput(impl->productionOwner, identity);
        surface->input = owned.input;
        const auto& closure = impl->productionOwner->Closure();
        const auto found = std::find_if(closure.surfaces.begin(), closure.surfaces.end(),
            [&](const auto& item) { return item.surfaceId == identity; });
        for (size_t element = 0; element < 16; ++element)
            Require(std::abs(found->localToWorld[element] - double(model.data()[element])) < 1.e-5,
                "World placement differs from publication");
        const auto& inputs = found->inputs;
        const auto image = [&](const std::string& id)
        { return id.empty() ? std::shared_ptr<Texture>() : impl->productionOwner->Image(id); };
        surface->coverage = {image(inputs.coveragePageTable), image(inputs.coverageIds), image(inputs.coverageWeights)};
        surface->sources = {image(inputs.layerAlbedo), image(inputs.layerNormal),
            image(inputs.layerOrm), image(inputs.layerHeight)};
        surface->localWeights = image(inputs.localLayerWeights);
        surface->height.heightAtlas = image(inputs.heightAtlas);
        surface->height.layerMap = image(inputs.heightLayerMap);
        for (const auto& texture : surface->coverage)
            if (texture)
                surface->retained.push_back(texture);
        for (const auto& texture : surface->sources)
            if (texture)
                surface->retained.push_back(texture);
        surface->retained.push_back(image(inputs.surfaceLocalNormal));
        surface->retained.push_back(image(inputs.surfaceLocalTangent));
        surface->sourceMap = impl->sourceMaps.at(identity);
        const auto& mapping = impl->streaming.pageManager.PlacementLayerIdMap(identity);
        Require(mapping.size() == found->localToGlobal.size() &&
            std::equal(mapping.begin(), mapping.end(), found->localToGlobal.begin()),
            "World layer registry differs from production local/global mapping");
        for (size_t local = 0; local < mapping.size(); ++local)
            Require(impl->streaming.layerDefinitions.at(mapping[local]).sourceSet == found->sourceSet,
                "World sourceSet differs from production snapshot");
        for (const auto& material : surface->materials)
        {
            Require(Parameter(*material, "u_uvScale", 64.f) == surface->input.uvScale &&
                Parameter(*material, "u_heightBlendStrength", 0.f) ==
                    surface->input.retainedFragment.heightBlendStrength &&
                (Parameter(*material, "u_terrainWeightMapMode", 0.f) > 0.5f) ==
                    (found->weightSource == TerrainCompositedWeightSource::LocalLayerArray),
                "runtime MI static parameters differ from production snapshot");
            for (const auto* dynamic : {"u_globalColourWeight", "u_globalColourWeightNear",
                "u_globalAoStrength", "u_globalWetnessStrength"})
                Require((!inputs.globalColour.empty() && !inputs.globalMasks.empty()) ||
                    Parameter(*material, dynamic, 0.f) == 0.f,
                    "runtime global material input missing from production closure");
        }
    }
    if (!CookMode() && IsAuthoringMode() && surface->localWeights)
    {
        const auto identity = AuthoredWeightIdentity(plan.desc.materialInstancePath);
        Require(identity.has_value(), "local weights require an authored texture identity");
        for (const auto& previous : impl->surfaces)
            if (!previous->authoredWeightIdentities.empty() &&
                previous->authoredWeightIdentities.front() == identity &&
                previous->authorWeights && previous->importedWeights)
            {
                surface->authorWeights = previous->authorWeights;
                surface->importedWeights = previous->importedWeights;
                break;
            }
        if (!surface->authorWeights)
        {
            const auto description = LoadTextureAssetDesc(*identity);
            Require(description.textureArray && !description.sources.empty() &&
                description.transfer == TextureIO::LoadOptions::Transfer::Linear,
                "local weights require a linear texture array");
            std::vector<std::filesystem::path> paths;
            for (const auto& source : description.sources)
                paths.push_back(CommonFunction::Path(source));
            auto image = TextureIO::LoadArray(paths,
                Texture::MaterialLoadOptionsForPath(ToTextureCreateDesc(description), paths.front()));
            auto author = impl->DeferredImage(*surface, image, vk::Filter::eLinear, true);
            MaterialInstance authorMaterial;
            authorMaterial.SetTexture("layerWeightMaps", author, identity, std::nullopt);
            impl->CaptureImportedWeights(*surface, authorMaterial);
        }
        surface->authoredWeightIdentities.clear();
        for (size_t index = 0; index < surface->materials.size(); ++index)
        {
            const auto authored = index ? AuthoredWeightIdentity(plan.desc.detailMaterialInstancePath) : identity;
            Require(authored == identity, "base/detail authored Local weight identity differs");
            surface->authoredWeightIdentities.push_back(authored);
            surface->materials[index]->SetTexture(
                "layerWeightMaps", surface->localWeights, authored, std::nullopt);
        }
    }
    if (surface->localWeights)
        for (const auto& material : surface->materials)
            material->SetTextureKeepingIdentity("layerWeightMaps", surface->localWeights);
    const auto origin = plan.desc.pageOrigin;
    Require(origin.x() >= 0 && origin.y() >= 0 &&
        origin.x() + int(plan.desc.pageTilesPerSide) <= 32 &&
        origin.y() + int(plan.desc.pageTilesPerSide) <= 32, "coverage surface map region out of range");
    for (uint32_t pageY = 0; pageY < plan.desc.pageTilesPerSide; ++pageY)
        for (uint32_t pageX = 0; pageX < plan.desc.pageTilesPerSide; ++pageX)
        {
            const size_t offset = (size_t(origin.y() + pageY) * 32 + origin.x() + pageX) * 4;
            Require(impl->mapPixels[offset] == 0, "overlapping composited coverage surfaces");
            impl->mapPixels[offset] = float(impl->surfaces.size() + 1);
            impl->mapPixels[offset + 1] = float(desc.width);
            impl->mapPixels[offset + 2] = float(desc.height);
            impl->mapPixels[offset + 3] = float(layout.MipCount() - 1);
        }
    impl->mapDirty = true;
    impl->BindPlanSources(surface->plan);
    impl->surfaces.push_back(std::move(surface));
    impl->preparedSourcePlans.erase(identity);
}

bool TerrainCompositedService::RecordUploads(
    vk::CommandBuffer command, uint32_t frameIndex, uint64_t completedEpoch,
    std::optional<uint64_t> renderFrame)
{
    Require(!impl->recording, "upload transaction already active");
    bool copied = false;
    try
    {
        impl->CompleteProduction(completedEpoch);
        impl->EmitBudgetObservation(completedEpoch);
        impl->CaptureRequestDiagnostics(renderFrame, completedEpoch);
        if (impl->faultBudgetHook)
        {
            const auto action = TerrainFaultInjection().ReadAction(impl->faultBudgetHook);
            if (action && (action->cancelRequested || action->timedOut))
                TerrainFaultInjection().FinishAction(impl->faultBudgetHook, action->request.faultId);
        }
        if (impl->faultStaleHook)
        {
            const auto action = TerrainFaultInjection().ReadAction(impl->faultStaleHook);
            const bool held = std::any_of(impl->productionTasks.begin(), impl->productionTasks.end(),
                [](const auto& task) { return bool(task.staleHold); });
            if (action && !held && (action->cancelRequested || action->timedOut))
                TerrainFaultInjection().FinishAction(impl->faultStaleHook, action->request.faultId);
        }
        if (impl->faultSlotHook)
            TerrainFaultInjection().PumpSlotHook(impl->faultSlotHook, completedEpoch);
        if (GlobalAuthoringMode.load())
        {
            impl->QueuePendingProducer();
            bool superseded = false;
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                superseded = impl->worldCandidate && !impl->updates.empty();
                if (superseded)
                    for (const auto& change : impl->worldCandidate->changes)
                        impl->updates.emplace(change.first, change.second);
            }
            if (superseded)
                impl->DiscardWorldCandidate("World candidate superseded by a newer author snapshot", true);
            impl->AdvanceWorldCandidate(completedEpoch);
            impl->PrepareRefreshes();
            if (!impl->worldCandidate)
                impl->ConsumeMaterials();
            impl->ConsumeUpdates();
        }
        const uint64_t inputBytes = impl->RecordWorldInputs(command, completedEpoch);
        for (const auto& surface : impl->surfaces)
            impl->RecordImages(command, *surface);
        if (impl->finalized)
            impl->RecordProduction(command, completedEpoch);
        if (impl->runtime && impl->finalized)
        {
            TerrainCompositedFrameUploadBudget allowance;
            allowance.remainingBytes = impl->frozen->runtime.uploadBytesPerFrame -
                std::min(impl->frozen->runtime.uploadBytesPerFrame, inputBytes);
            allowance.remainingPages = impl->frozen->runtime.uploadsPerFrame;
            allowance.remainingSchedulingMicroseconds = impl->frozen->runtime.schedulingMicroseconds;
            copied = impl->runtime->RecordUploads(command, frameIndex, completedEpoch, allowance);
        }
        copied = copied || (impl->worldCandidate && impl->worldCandidate->inputsRecorded);
        copied = copied || std::any_of(impl->productionTasks.begin(), impl->productionTasks.end(),
            [](const auto& task) { return task.recorded && task.epoch == 0; });
        impl->recording = true;
    }
    catch (...)
    {
        Abort();
        throw;
    }
    return copied;
}

void TerrainCompositedService::Commit(uint64_t epoch)
{
    if (!impl->recording)
        return;
    if (impl->runtime && impl->finalized)
        impl->runtime->Commit(epoch);
    for (const auto& surface : impl->surfaces)
    {
        if (!surface->previousImages.empty())
            impl->retired.emplace_back(epoch, std::move(surface->previousImages));
        for (const auto& image : surface->pendingImages)
            impl->stagingRetired.emplace_back(epoch, image.staging);
        surface->pendingImages.clear();
        surface->pendingHeight.clear();
    }
    for (auto& task : impl->productionTasks)
        if (task.recorded && task.epoch == 0)
            task.epoch = epoch;
    if (impl->worldCandidate && impl->worldCandidate->inputsRecorded)
    {
        auto& candidate = *impl->worldCandidate;
        candidate.nextPatch = candidate.recordedEnd;
        candidate.cloned = candidate.cloned || candidate.recordedClone;
        candidate.uploadEpoch = epoch;
        if (candidate.transaction.Phase() == TerrainCompositedSnapshotPhase::InputsRecorded)
            candidate.transaction.SubmitInputs(epoch);
        candidate.inputsRecorded = false;
        candidate.recordedClone = false;
        candidate.recordedInitialized.clear();
    }
    impl->lastCommittedEpoch = epoch;
    ++impl->successfulFrameCommits;
    if (IsAuthoringMode())
        impl->UpdateRefreshes(true);
    impl->recording = false;
    impl->SnapshotStats();
}

void TerrainCompositedService::Abort()
{
    if (impl->runtime)
        impl->runtime->Abort();
    for (auto& task : impl->productionTasks)
        if (task.page && task.epoch == 0)
        {
            impl->runtime->CompleteProduction(*task.page, 0, false);
            ++impl->abortedProductionTasks;
            task.page.reset();
            task.recorded = false;
            task.retained.clear();
            task.owner.reset();
            task.snapshot.reset();
            task.producer.reset();
            task.executionLease.reset();
            task.inputOwner.reset();
            task.candidateRevision = 0;
        }
    if (impl->worldCandidate)
    {
        auto& candidate = *impl->worldCandidate;
        candidate.transaction.AbortInputs();
        for (const auto* texture : candidate.recordedInitialized)
            candidate.initialized.erase(texture);
        candidate.recordedInitialized.clear();
        candidate.inputsRecorded = false;
        candidate.recordedClone = false;
    }
    impl->recording = false;
}

bool TerrainCompositedService::HasRequestDiagnostics() const noexcept
{
    return bool(impl->requestDiagnostics);
}

void TerrainCompositedService::ObserveFeedbackReadback(
    const uint8_t* bytes, size_t byteCount, uint8_t frameTag,
    uint64_t copiedEpoch, uint64_t completedEpoch)
{
    if (!impl->requestDiagnostics || !impl->runtime)
        return;
    try
    {
        std::array<std::string_view, 256> surfaces{};
        for (size_t index = 0; index < impl->surfaces.size(); ++index)
            surfaces[index] = impl->surfaces[index]->desc.surfaceId;
        impl->requestDiagnostics->ObserveReadback(*impl->runtime, surfaces, impl->surfaces.size(),
            bytes, byteCount, frameTag, copiedEpoch, completedEpoch);
    }
    catch (const std::exception& error)
    {
        impl->requestDiagnostics.reset();
        RuntimeEvidence::MarkIncomplete();
        std::cerr << "[TerrainVT] explicit readback witness incomplete: " << error.what() << std::endl;
    }
}

void TerrainCompositedService::ConsumeFeedback(
    const std::vector<TerrainCompositedFeedbackRecord>& records)
{
    std::vector<std::vector<TerrainCompositedVTTaskToken>> grouped(impl->surfaces.size());
    for (const auto& record : records)
    {
        if (record.surfaceIndex >= impl->surfaces.size())
            continue;
        const auto& surface = impl->surfaces[record.surfaceIndex];
        TerrainCompositedVTPageKey key{surface->desc.surfaceId, record.mip, record.x, record.y};
        const auto layout = surface->desc.Layout();
        if (!layout.Contains(key))
            continue;
        grouped[record.surfaceIndex].push_back(impl->runtime->Token(key));
    }
    std::vector<TerrainCompositedVTTaskToken> requests;
    for (const auto& group : grouped)
        requests.insert(requests.end(), group.begin(), group.end());
    if (impl->runtime)
    {
        impl->runtime->ConsumeFeedback(requests);
        if (impl->requestDiagnostics)
            try
            {
                impl->requestDiagnostics->ObserveFeedback(*impl->runtime, requests);
            }
            catch (const std::exception& error)
            {
                impl->requestDiagnostics.reset();
                RuntimeEvidence::MarkIncomplete();
                std::cerr << "[TerrainVT] explicit feedback probe incomplete: " << error.what() << std::endl;
            }
    }
}

void TerrainCompositedService::RefreshPlacement(
    const TerrainBuildPlan& plan, const TerrainCompositedVTUVRect& dirty, uint64_t revision)
{
    Require(std::isfinite(dirty.minU) && std::isfinite(dirty.minV) &&
        std::isfinite(dirty.maxU) && std::isfinite(dirty.maxV) &&
        dirty.minU <= dirty.maxU && dirty.minV <= dirty.maxV, "invalid queued dirty rectangle");
    std::lock_guard<std::mutex> lock(impl->mutex);
    const auto identity = TerrainPlacementIdentity(plan);
    auto found = impl->updates.find(identity);
    Require(impl->updates.size() < 256 || found != impl->updates.end(), "editor update queue exceeds surface limit");
    auto merged = dirty;
    if (found != impl->updates.end())
    {
        merged.minU = std::min(merged.minU, found->second.dirty.minU);
        merged.minV = std::min(merged.minV, found->second.dirty.minV);
        merged.maxU = std::max(merged.maxU, found->second.dirty.maxU);
        merged.maxV = std::max(merged.maxV, found->second.dirty.maxV);
    }
    const bool force = found != impl->updates.end() && found->second.force;
    Require(bool(impl->sourceSnapshots), "queued pixel owners require a prepared World");
    auto retained = impl->sourceSnapshots->Retain(plan);
    impl->updates[identity] = {std::move(retained), merged, ++impl->nextEditRevision, revision, force};
}

void TerrainCompositedService::NotifyMaterialChanged(
    const std::string& path, const std::shared_ptr<MaterialInstance>& material)
{
    Require(!path.empty() && bool(material), "material notification requires an identity and instance");
    std::lock_guard<std::mutex> lock(impl->mutex);
    Require(impl->materialUpdates.size() < 512 || impl->materialUpdates.count(path),
        "material notification queue exceeds limit");
    std::shared_ptr<Texture> localWeights;
    std::shared_ptr<Texture> authorWeights;
    std::optional<std::string> authoredWeightIdentity;
    for (const auto& surface : impl->surfaces)
        if (path == surface->plan.desc.materialInstancePath ||
            path == surface->plan.desc.detailMaterialInstancePath)
        {
            localWeights = surface->localWeights;
            authorWeights = surface->authorWeights;
            const size_t index = path == surface->plan.desc.detailMaterialInstancePath ? 1 : 0;
            if (index < surface->authoredWeightIdentities.size())
                authoredWeightIdentity = surface->authoredWeightIdentities[index];
            break;
        }
    const auto intent = AuthorMaterialState(
        material->CaptureStateSnapshot(), localWeights, authorWeights, authoredWeightIdentity);
    std::vector<MaterialInstanceStateBatch::Change> restore;
    for (auto& surface : impl->surfaces)
    {
        const size_t index = path == surface->plan.desc.materialInstancePath ? 0 :
            path == surface->plan.desc.detailMaterialInstancePath ? 1 : SIZE_MAX;
        if (index >= surface->materials.size() || index >= surface->materialStates.size())
            continue;
        auto change = impl->MaterialChange(*surface, material, impl->runtime->Binding(surface->desc.surfaceId));
        change.parameters = surface->materialStates[index]->parameters;
        restore.push_back(std::move(change));
    }
    if (!restore.empty())
    {
        MaterialInstanceStateBatch transaction(std::move(restore));
        Require(transaction.TryLockNoThrow(), "material changed during terrain snapshot handoff");
        transaction.CommitNoThrow();
    }
    for (auto& surface : impl->surfaces)
    {
        const size_t index = path == surface->plan.desc.materialInstancePath ? 0 :
            path == surface->plan.desc.detailMaterialInstancePath ? 1 : SIZE_MAX;
        if (index < surface->materials.size())
            surface->materials[index] = material;
    }
    impl->materialIntents[path] = intent;
    impl->materialUpdates[path] = material;
}

std::shared_ptr<Texture> TerrainCompositedService::SurfaceMap()
{
    FinaliseSurfaceMap();
    return impl->surfaceMap;
}

void TerrainCompositedService::FinaliseSurfaceMap()
{
    if (!impl->finalized && !impl->surfaces.empty())
    {
        std::set<std::string> registered;
        for (const auto& surface : impl->surfaces)
            registered.insert(surface->desc.surfaceId);
        Require(registered == impl->expectedSurfaces, "World registration is incomplete");
        if (CookMode())
        {
            impl->InitializeProduction();
            Require(impl->baker->Available(), impl->baker->UnavailableReason());
            const auto publication = impl->CapturePublication();
            TerrainCompositedVTStore::Publish(impl->worldManifest, publication,
                TerrainCompositedVTStore::PageProducer([&](const auto& key) {
                    const auto found = std::find_if(impl->surfaces.begin(), impl->surfaces.end(),
                        [&](const auto& surface) { return surface->desc.surfaceId == key.surfaceId; });
                    Require(found != impl->surfaces.end(), "Cook requested an unknown World surface");
                    return impl->Bake(**found, key).bytes;
                }));
            impl->store = std::make_shared<TerrainCompositedVTStore>(
                TerrainCompositedVTStore::Open(impl->worldManifest));
            impl->store->ValidateAll();
            for (const auto& surface : impl->surfaces)
                impl->runtime->RegisterSurface(impl->store, surface->desc, impl->generation, 0);
        }
        if (!CookMode())
            impl->adjacency = impl->store->Production().adjacency;
        impl->InitializeProduction();
        impl->runtime->SeedReadOnlyBaseline();
        size_t remaining = impl->surfaces.size() * 4 + 2;
        while (!impl->runtime->Ready())
        {
            Require(remaining-- != 0, "bounded World root warmup made no progress");
            auto command = impl->backend.BeginSingleTimeCommands();
            try
            {
                impl->RecordProduction(command, 0);
                const bool copied = impl->runtime->RecordUploads(command, 0, 0);
                impl->backend.EndSingleTimeCommands(command);
                if (copied)
                    impl->runtime->Commit(0);
                else
                    impl->runtime->Abort();
                impl->CompleteProduction(0, true);
            }
            catch (...)
            {
                impl->runtime->Abort();
                if (command)
                    impl->backend.AbortSingleTimeCommands(command);
                throw;
            }
        }
        for (const auto& surface : impl->surfaces)
        {
            impl->Bind(*surface);
            surface->materialStates.clear();
            for (size_t index = 0; index < surface->materials.size(); ++index)
                surface->materialStates.push_back(AuthorMaterialState(
                    surface->materials[index]->CaptureStateSnapshot(), surface->localWeights,
                    surface->authorWeights,
                    index < surface->authoredWeightIdentities.size()
                        ? surface->authoredWeightIdentities[index] : std::nullopt));
        }
        if (!CookMode())
            impl->productionOwner.reset();
        impl->finalized = true;
        impl->SnapshotStats();
    }
    if (!impl->surfaces.empty())
    {
        for (const auto& plan : impl->streaming.pageManager.Plans())
            Require(plan.desc.compositedVT.enabled || plan.desc.layers.empty(),
                "layer-source terrain feedback cannot mix with composited feedback in one World");
    }
    if (impl->mapDirty || !impl->surfaceMap)
        impl->PublishMap();
}

bool TerrainCompositedService::Enabled() const { return HasSurfaces(); }

bool TerrainCompositedService::HasSurfaces() const { return !impl->surfaces.empty(); }

void TerrainCompositedService::ReportStats() const
{
    impl->budgetSnapshotRequested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::cout << impl->stats << " pendingEditorUpdates=" << impl->updates.size()
        << " pendingMaterialUpdates=" << impl->materialUpdates.size() << std::endl;
}
} // namespace VL
