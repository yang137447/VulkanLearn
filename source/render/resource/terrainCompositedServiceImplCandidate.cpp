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
TerrainCompositedService::Impl::Impl(RendererBackendVulkan& renderer, PipelineFactory& factory, TerrainStreamingState& state)
    : backend(renderer), pipelineFactory(factory), streaming(state)
{
}

TerrainCompositedService::Impl::~Impl()
{
    ResetFaultHooks();
    productionTasks.clear();
    baker.reset();
    bakerLease.reset();
}

void TerrainCompositedService::Impl::ResetFaultHooks()
{
    Deactivate(TerrainFaultInjection());
}

void TerrainCompositedService::Impl::RegisterActiveFaultHooks()
{
    if (CookMode() || !worldGeneration || !runtime || faultStatusHook)
        return;
    auto* runtimeOwner = runtime.get();
    const bool registered = Activate(TerrainFaultInjection(), worldGeneration, {
        [runtimeOwner](uint32_t slot, uint64_t completed) {
            return runtimeOwner->SlotDiagnostics(slot, completed);
        },
        [runtimeOwner](uint32_t slot) { return runtimeOwner->ArmSlotReuseDiagnostics(slot); },
        [runtimeOwner](uint64_t completed) { return runtimeOwner->SlotReuseDiagnostics(completed); },
        [runtimeOwner] { runtimeOwner->CancelSlotReuseDiagnostics(); }});
    if (!registered)
        std::cerr << "[TerrainVT] fault owner registration unavailable at the bounded controller limit\n";
}

void TerrainCompositedService::Impl::CaptureRequestDiagnostics(std::optional<uint64_t> renderFrame, uint64_t completedEpoch)
{
    if (!renderFrame || !runtime || !frozen || !finalized || CookMode())
        return;
    try
    {
        if (!requestDiagnosticsConfigured)
        {
            requestDiagnosticsConfigured = true;
            requestDiagnostics = TerrainCompositedServiceRequestDiagnostics::Create(*frozen->runtime.owner);
        }
        if (requestDiagnostics)
        {
            requestDiagnostics->Capture(*runtime, *frozen->runtime.owner,
                worldGeneration, qualityPose, qualityPoseStartedMs,
                *renderFrame, completedEpoch, QualityMonotonicMs());
            if (requestDiagnostics->Finished())
                requestDiagnostics.reset();
        }
    }
    catch (const std::exception& error)
    {
        requestDiagnostics.reset();
        RuntimeEvidence::MarkIncomplete();
        std::cerr << "[TerrainVT] explicit request probe incomplete: " << error.what() << std::endl;
    }
}

MaterialInstanceStateBatch::Change TerrainCompositedService::Impl::MaterialChange(const TerrainCompositedService::Impl::Surface& surface,
    const std::shared_ptr<MaterialInstance>& material, const TerrainCompositedMaterialBinding& binding)
{
    MaterialInstanceStateBatch::Change change;
    change.material = material;
    change.textures = {
        {"compositedPageTable", binding.compositedPageTable},
        {"compositedAlbedo", binding.compositedAlbedo},
        {"compositedNormal", binding.compositedNormal},
        {"compositedOrm", binding.compositedOrm},
        {"layerTable", streaming.layerResources.layerTable},
        {"pageTable", surface.coverage[0]},
        {"layerIds", surface.coverage[1]},
        {"layerWeights", surface.coverage[2]}};
    if (surface.height.heightAtlas)
    {
        change.textures.emplace("terrainHeightAtlas", surface.height.heightAtlas);
        change.textures.emplace("terrainHeightLayerMap", surface.height.layerMap);
    }
    if (surface.sourceMap) change.textures.emplace("terrainLayerSourceMap", surface.sourceMap);
    if (surface.localWeights) change.textures.emplace("layerWeightMaps", surface.localWeights);
    const char* sources[] = {"layerAlbedo", "layerNormal", "layerPbr", "layerHeight"};
    for (size_t index = 0; index < surface.sources.size(); ++index)
        change.textures.emplace(sources[index], surface.sources[index]);
    change.parameters = {
        {"u_compositedResolution", Eigen::Vector2f(binding.u_compositedResolution[0], binding.u_compositedResolution[1])},
        {"u_compositedMaxMip", binding.u_compositedMaxMip}, {"u_compositedRepeat", binding.u_compositedRepeat},
        {"u_compositedNormalRG", binding.u_compositedNormalRG}, {"u_compositedEnabled", binding.u_compositedEnabled},
        {"u_terrainHeightAtlasEnabled", surface.height.heightAtlas ? 1.f : 0.f},
        {"u_uvScale", surface.input.uvScale},
        {"u_heightBlendStrength", surface.input.retainedFragment.heightBlendStrength},
        {"u_terrainWeightMapMode", surface.input.weightSource == Baker::WeightSource::LocalLayerArray ? 1.f : 0.f},
        {"u_pageTableSize", 32.f}, {"u_pageResolution", 128.f}};
    for (size_t index = 0; index < surface.materials.size(); ++index)
        if (surface.materials[index] == material && index < surface.materialStates.size())
            for (const auto& parameter : surface.materialStates[index]->parameters)
                change.parameters.emplace(parameter.first, parameter.second);
    return change;
}

std::shared_ptr<TerrainCompositedService::Impl::Buffer> TerrainCompositedService::Impl::HostBuffer(uint64_t bytes, vk::BufferUsageFlags usage, const char* name)
{
    auto buffer = std::make_shared<Buffer>();
    buffer->backend = &backend;
    buffer->bytes = bytes;
    const auto created = backend.CreateBuffer(bytes, usage,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, name);
    buffer->buffer = created.first;
    buffer->memory = created.second;
    buffer->mapped = backend.MapMemory(buffer->memory, bytes);
    return buffer;
}

std::shared_ptr<Texture> TerrainCompositedService::Impl::AllocateTexture(
    uint32_t width, uint32_t height, vk::Format format, vk::Filter filter, bool array,
    uint32_t layers)
{
    vk::ImageCreateInfo create;
    create.setImageType(vk::ImageType::e2D).setExtent({width, height, 1})
        .setMipLevels(1).setArrayLayers(layers).setFormat(format)
        .setSamples(vk::SampleCountFlagBits::e1).setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eTransferSrc)
        .setSharingMode(vk::SharingMode::eExclusive).setInitialLayout(vk::ImageLayout::eUndefined);
    Require(bool(frozen), "input allocation requires frozen budget");
    auto owner = allocationOverride ? allocationOverride : frozen->runtime.owner;
    const auto actual = backend.QueryImageMemoryRequirements(create).size;
    const auto overhead = FrozenOverheads();
    const auto cost = TerrainCompositedProductionGpu::AddAllocationCosts(
        {actual, 0, 0, 0}, TerrainCompositedProductionGpu::AddAllocationCosts(
            overhead.imageMetadata, overhead.imageViewAndSampler));
    if (allocationOverride && faultBudgetHook)
    {
        const auto action = TerrainFaultInjection().ReadAction(faultBudgetHook);
        if (action && (action->cancelRequested || action->timedOut))
            TerrainFaultInjection().FinishAction(faultBudgetHook, action->request.faultId);
        else if (action && !action->consumed)
        {
            const auto requested = TerrainFaultInjection().ConsumeAction(faultBudgetHook, worldGeneration);
            if (requested)
            {
                const uint64_t actualLimit = owner->Caps().totalGpuBytes;
                const uint64_t demand = owner->Statistics().current.gpuBytes + cost.gpuBytes;
                const bool validLimit = requested->testAdmissionLimitBytes < actualLimit;
                const bool rejected = validLimit && demand > requested->testAdmissionLimitBytes;
                TerrainFaultInjection().FinishAction(faultBudgetHook, requested->faultId);
                ServiceEvidence("RVT_EVIDENCE", "[RVT_EVIDENCE] ", {
                    {"event", "candidate-budget-admission"}, {"faultId", requested->faultId},
                    {"worldGeneration", worldGeneration}, {"actualFrozenLimit", actualLimit},
                    {"testLimit", requested->testAdmissionLimitBytes}, {"actualDemand", demand},
                    {"validStrictLowerLimit", validLimit}, {"rejected", rejected}});
                Require(validLimit, "test admission limit must be strictly below the actual frozen cap");
                Require(!rejected, "candidate rejected by explicit strict-lower actual input admission test");
            }
        }
    }
    auto lease = owner->TryReserve(cost, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "candidate input exceeds frozen actual peak admission");
    const auto created = backend.CreateImage(create, vk::MemoryPropertyFlagBits::eDeviceLocal, "VTPreviewInput");
    vk::ImageView view;
    vk::Sampler sampler;
    std::shared_ptr<Texture> texture;
    try
    {
        view = backend.CreateImageView(created.first,
            array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
            format, vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers, "VTPreviewInputView");
        sampler = backend.Create2DSampler(filter, vk::SamplerAddressMode::eClampToEdge,
            false, "VTPreviewInputSampler");
        texture = std::make_shared<Texture>(backend, created.first, created.second, view,
            sampler, 1, format, layers);
    }
    catch (...)
    {
        auto image = created.first;
        auto memory = created.second;
        backend.DestroyImageResource(image, memory, view, sampler);
        throw;
    }
    auto allocation = std::make_shared<InputAllocation>(
        InputAllocation{std::move(*lease), std::move(texture)});
    auto retained = std::shared_ptr<Texture>(allocation, allocation->texture.get());
    inputAllocationCosts[retained.get()] = cost;
    inputAllocations[retained.get()] = retained;
    exclusiveInputOwners[retained.get()] = allocation;
    return retained;
}

std::shared_ptr<Texture> TerrainCompositedService::Impl::DeferredImage(
    TerrainCompositedService::Impl::Surface& surface, const HostImage& image, vk::Filter filter, bool array)
{
    const auto format = image.format == HostImage::PixelFormat::RGBA32_FLOAT
        ? vk::Format::eR32G32B32A32Sfloat : image.format == HostImage::PixelFormat::R32_FLOAT
        ? vk::Format::eR32Sfloat : image.format == HostImage::PixelFormat::R16_UNORM
        ? vk::Format::eR16Unorm : vk::Format::eR8G8B8A8Unorm;
    auto texture = AllocateTexture(image.width, image.height, format, filter, array, image.arrayLayers);
    auto staging = HostBuffer(image.data.size(), vk::BufferUsageFlagBits::eTransferSrc, "VTPreviewInputStaging");
    std::memcpy(staging->mapped, image.data.data(), image.data.size());
    surface.pendingImages.push_back({texture, staging, image.width, image.height, image.arrayLayers});
    return texture;
}

void TerrainCompositedService::Impl::ReserveCandidateCpu(TerrainCompositedService::Impl::WorldCandidate& candidate, uint64_t bytes)
{
    auto lease = candidate.budget->TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "candidate CPU patches exceed frozen peak admission");
    candidate.cpuLeases.push_back(std::move(*lease));
}

void TerrainCompositedService::Impl::AddCoveragePatches(TerrainCompositedService::Impl::WorldCandidate& candidate, const TerrainCompositedService::Impl::Surface& surface,
    const TerrainCompositedVTUVRect& dirty)
{
    const uint32_t tiles = surface.plan.desc.pageTilesPerSide;
    const uint32_t minX = std::min(tiles - 1, uint32_t(std::floor(dirty.minU * tiles)));
    const uint32_t minY = std::min(tiles - 1, uint32_t(std::floor(dirty.minV * tiles)));
    const uint32_t maxX = std::min(tiles, uint32_t(std::ceil(dirty.maxU * tiles)));
    const uint32_t maxY = std::min(tiles, uint32_t(std::ceil(dirty.maxV * tiles)));
    ReserveCandidateCpu(candidate, uint64_t(maxX - minX) * (maxY - minY) *
        (128 * 128 * 8 + 2 * sizeof(InputPatch)) * 3);
    for (uint32_t tileY = minY; tileY < maxY; ++tileY)
        for (uint32_t tileX = minX; tileX < maxX; ++tileX)
        {
            auto page = TerrainWeightMapBuilder::BuildPage(surface.plan, tiles, 128,
                tileX, tileY, streaming.pageManager.PlacementLayerIdMap(surface.desc.surfaceId));
            const uint32_t virtualX = surface.plan.desc.pageOrigin.x() + tileX;
            const uint32_t virtualY = surface.plan.desc.pageOrigin.y() + tileY;
            const size_t offset = (size_t(virtualY) * 32 + virtualX) * 4;
            Require(offset + 4 <= coveragePageMap.size() && coveragePageMap[offset + 3],
                "candidate dirty tile has no frozen World coverage mapping");
            const uint32_t x = uint32_t(coveragePageMap[offset]) * 128;
            const uint32_t y = uint32_t(coveragePageMap[offset + 1]) * 128;
            candidate.patches.push_back({candidate.coverage[1], x, y, 0, 128, 128, std::move(page.ids)});
            candidate.patches.push_back({candidate.coverage[2], x, y, 0, 128, 128, std::move(page.weights)});
        }
}

std::shared_ptr<Texture> TerrainCompositedService::Impl::AddCpuImagePatches(TerrainCompositedService::Impl::WorldCandidate& candidate, const HostImage& image,
    vk::Format format, vk::Filter filter, bool array)
{
    auto texture = AllocateTexture(image.width, image.height, format, filter, array, image.arrayLayers);
    const uint32_t rowBytes = image.width * image.GetPixelSize();
    Require(rowBytes && rowBytes <= TerrainCompositedProductionGpu::MaxTransferBytes,
        "candidate image row exceeds bounded transfer bytes");
    const uint32_t chunkRows = uint32_t(TerrainCompositedProductionGpu::MaxTransferBytes / rowBytes);
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
        for (uint32_t row = 0; row < image.height;)
        {
            const uint32_t rows = std::min(chunkRows, image.height - row);
            const auto offset = image.GetMipOffset(0, layer) + uint64_t(row) * image.GetMipRowStride(0);
            InputPatch patch{texture, 0, row, layer, image.width, rows, {}};
            patch.bytes.assign(image.data.begin() + offset,
                image.data.begin() + offset + uint64_t(rows) * rowBytes);
            candidate.patches.push_back(std::move(patch));
            row += rows;
        }
    return texture;
}

void TerrainCompositedService::Impl::AddFramePatches(TerrainCompositedService::Impl::WorldCandidate& candidate, TerrainCompositedService::Impl::Surface& surface)
{
    const uint64_t extent = uint64_t(surface.plan.desc.gridResolution) + 1;
    ReserveCandidateCpu(candidate, extent * extent * 32 * 3 + 1024 * 1024);
    const auto frames = Frames(surface.plan);
    for (auto iterator = surface.retained.begin(); iterator != surface.retained.end();)
        if ((*iterator)->getImageView() == surface.input.surfaceLocalNormal.descriptor.imageView ||
            (*iterator)->getImageView() == surface.input.surfaceLocalTangent.descriptor.imageView)
            iterator = surface.retained.erase(iterator);
        else
            ++iterator;
    const auto normal = AddCpuImagePatches(candidate, frames.first,
        vk::Format::eR32G32B32A32Sfloat, vk::Filter::eLinear);
    const auto tangent = AddCpuImagePatches(candidate, frames.second,
        vk::Format::eR32G32B32A32Sfloat, vk::Filter::eLinear);
    surface.input.surfaceLocalNormal = Sample(backend, normal);
    surface.input.surfaceLocalTangent = Sample(backend, tangent);
    surface.retained.push_back(normal);
    surface.retained.push_back(tangent);
}

std::shared_ptr<Texture> TerrainCompositedService::Impl::RetainExternalInput(const std::shared_ptr<Texture>& texture)
{
    if (!texture)
        return {};
    const auto known = inputAllocations.find(texture.get());
    if (known != inputAllocations.end())
        if (auto alias = known->second.lock())
            return alias;
    const auto extent = backend.GetImageExtent(texture->getImage());
    vk::ImageCreateInfo info;
    info.setImageType(vk::ImageType::e2D).setExtent({extent.width, extent.height, 1})
        .setMipLevels(texture->getMipLevels()).setArrayLayers(texture->GetArrayLayers())
        .setFormat(texture->GetFormat()).setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal).setSharingMode(vk::SharingMode::eExclusive)
        .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst);
    const auto overhead = FrozenOverheads();
    const auto cost = TerrainCompositedProductionGpu::AddAllocationCosts(
        {backend.QueryImageMemoryRequirements(info).size, 0, 0, 0},
        TerrainCompositedProductionGpu::AddAllocationCosts(overhead.imageMetadata, overhead.imageViewAndSampler));
    auto lease = allocationOverride->TryReserve(cost, TerrainRvtCostClass::Input,
        TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "material source replacement exceeds actual candidate peak input budget");
    auto owner = std::make_shared<InputAllocation>(InputAllocation{std::move(*lease), texture});
    auto alias = std::shared_ptr<Texture>(owner, texture.get());
    inputAllocations[texture.get()] = alias;
    inputAllocationCosts[texture.get()] = cost;
    exclusiveInputOwners[texture.get()] = owner;
    return alias;
}

void TerrainCompositedService::Impl::AddLocalWeightPatches(TerrainCompositedService::Impl::WorldCandidate& candidate, TerrainCompositedService::Impl::Surface& surface)
{
    const auto previous = surface.localWeights;
    Require(bool(previous), "local Paint requires a current weight array");
    const auto extent = backend.GetImageExtent(previous->getImage());
    const uint64_t expectedBytes = uint64_t(extent.width) * extent.height *
        previous->GetArrayLayers() * sizeof(float);
    ReserveCandidateCpu(candidate, expectedBytes * 8 + 1024 * 1024);
    if (!surface.importedWeights)
        CaptureImportedWeights(surface, *surface.materials.front());
    const auto image = BuildTerrainCompositedWeightOverlay(surface.plan, *surface.importedWeights,
        streaming.pageManager.PlacementLayerIdMap(surface.desc.surfaceId));
    Require(image.format == HostImage::PixelFormat::R32_FLOAT &&
        image.width == extent.width && image.height == extent.height &&
        image.arrayLayers == previous->GetArrayLayers(),
        "local Paint changed the frozen weight-array layout");
    auto replacement = AllocateTexture(image.width, image.height, vk::Format::eR32Sfloat,
        vk::Filter::eLinear, true, image.arrayLayers);
    const uint32_t rowBytes = image.width * sizeof(float);
    const uint32_t chunkRows = std::max(1u,
        uint32_t(TerrainCompositedProductionGpu::MaxTransferBytes / rowBytes));
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
        for (uint32_t row = 0; row < image.height;)
        {
            const uint32_t rows = std::min(chunkRows, image.height - row);
            const auto offset = image.GetMipOffset(0, layer) + uint64_t(row) * rowBytes;
            InputPatch patch{replacement, 0, row, layer, image.width, rows, {}};
            patch.bytes.assign(image.data.begin() + offset,
                image.data.begin() + offset + uint64_t(rows) * rowBytes);
            candidate.patches.push_back(std::move(patch));
            row += rows;
        }
    surface.retained.erase(std::remove(surface.retained.begin(), surface.retained.end(), previous),
        surface.retained.end());
    surface.localWeights = replacement;
    surface.retained.push_back(replacement);
    surface.input.localLayerWeights = Sample(backend, replacement);
}

void TerrainCompositedService::Impl::CheckCandidateSteadyInputs(TerrainCompositedService::Impl::WorldCandidate& candidate)
{
    for (auto iterator = inputAllocations.begin(); iterator != inputAllocations.end();)
        if (iterator->second.expired())
        {
            inputAllocationCosts.erase(iterator->first);
            exclusiveInputOwners.erase(iterator->first);
            iterator = inputAllocations.erase(iterator);
        }
        else
            ++iterator;
    std::set<std::shared_ptr<Texture>, std::owner_less<std::shared_ptr<Texture>>> textures;
    const auto retain = [&](const std::shared_ptr<Texture>& texture) {
        if (texture) textures.insert(texture);
    };
    retain(streaming.layerResources.layerTable);
    for (const auto& surface : candidate.surfaces)
    {
        for (const auto& texture : surface->retained) retain(texture);
        for (const auto& texture : surface->sources) retain(texture);
        for (const auto& texture : surface->coverage) retain(texture);
        retain(surface->sourceMap);
        retain(surface->localWeights);
        retain(surface->authorWeights);
        retain(surface->height.heightAtlas);
        retain(surface->height.layerMap);
        retain(surface->materials.front()->GetTexture("globalColour"));
        retain(surface->materials.front()->GetTexture("globalMasks"));
    }
    const bool prepareUses = !candidate.inputs;
    if (prepareUses)
    {
        candidate.inputs = std::make_shared<WorldLeafOwner>();
        auto metadata = candidate.budget->TryReserve({0,
            textures.size() * (TerrainCompositedProductionGpuResources::PhaseUse::MetadataBytes() + 256) +
                candidate.surfaces.size() * sizeof(Surface) + 1024 * 1024, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(metadata.has_value(), "World candidate leaf-use metadata exceeds peak CPU budget");
        candidate.inputs->metadata = std::move(*metadata);
    }
    TerrainRvtCost inputCost;
    std::set<const Texture*> unique;
    for (const auto& texture : textures)
        if (unique.insert(texture.get()).second)
        {
            const auto found = inputAllocationCosts.find(texture.get());
            Require(found != inputAllocationCosts.end(), "candidate input lacks actual allocation identity/cost");
            inputCost = TerrainCompositedProductionGpu::AddAllocationCosts(inputCost, found->second);
            if (prepareUses)
                candidate.inputs->textures.push_back(texture);
            if (prepareUses)
            {
                const auto allocation = exclusiveInputOwners.find(texture.get());
                if (allocation != exclusiveInputOwners.end())
                    if (const auto owner = allocation->second.lock())
                        candidate.inputs->exclusiveLeases.emplace(texture.get(), owner->lease);
            }
            if (prepareUses && currentInputs)
            {
                const auto use = currentInputs->uses.find(texture.get());
                if (use != currentInputs->uses.end())
                    candidate.inputs->uses.emplace(texture.get(),
                        use->second->Retain(TerrainRvtResourcePhase::Candidate));
            }
        }
    if (prepareUses)
        for (const auto& surface : candidate.surfaces)
            if (surface->importedWeights)
            {
                const auto allocation = cpuInputOwners.find(surface->importedWeights.get());
                Require(allocation != cpuInputOwners.end(), "raw weight snapshot has no charged CPU owner");
                const auto owner = allocation->second.lock();
                Require(bool(owner), "raw weight CPU allocation owner expired");
                candidate.inputs->exclusiveLeases.emplace(surface->importedWeights.get(), owner->lease);
                candidate.inputs->cpuImages.push_back(surface->importedWeights);
            }
    for (auto iterator = cpuInputOwners.begin(); iterator != cpuInputOwners.end();)
        if (iterator->second.expired()) iterator = cpuInputOwners.erase(iterator);
        else ++iterator;
    const auto common = frozen->runtime.owner->Statistics();
    steadyWorkspaceQuote = std::max(steadyWorkspaceQuote,
        common.byClass[static_cast<size_t>(TerrainRvtCostClass::Workspace)].gpuBytes);
    const uint64_t pool = common.byClass[static_cast<size_t>(TerrainRvtCostClass::PhysicalPool)].gpuBytes;
    Require(inputCost.gpuBytes <= frozen->steadyCaps.inputGpuBytes &&
        steadyWorkspaceQuote <= frozen->steadyCaps.workspaceGpuBytes &&
        pool + inputCost.gpuBytes + steadyWorkspaceQuote <= frozen->steadyCaps.totalGpuBytes,
        "new current World fails frozen steady admission after input allocation deduplication");
    std::cout << "[RVT_CANDIDATE_ADMISSION] " << nlohmann::json{
        {"revision", candidate.transaction.Revision()}, {"uniqueInputs", unique.size()},
        {"newCurrentInputGpuBytes", inputCost.gpuBytes}, {"sharedPoolGpuBytes", pool},
        {"commonWorkspaceUpperBoundGpuBytes", steadyWorkspaceQuote},
        {"candidatePeakGpuBytes", candidate.budget->Statistics().current.gpuBytes}}.dump() << std::endl;
}

void TerrainCompositedService::Impl::ConsumeCandidateBudgetAdmission(
    const std::shared_ptr<TerrainRvtBudgetOwner>& owner, uint64_t additionalGpuBytes)
{
    if (!faultBudgetHook)
        return;
    const auto action = TerrainFaultInjection().ReadAction(faultBudgetHook);
    if (!action || action->consumed)
        return;
    const auto requested = TerrainFaultInjection().ConsumeAction(faultBudgetHook, worldGeneration);
    if (!requested)
        return;
    const uint64_t actualLimit = owner->Caps().totalGpuBytes;
    const uint64_t demand = owner->Statistics().current.gpuBytes + additionalGpuBytes;
    const bool validLimit = requested->testAdmissionLimitBytes < actualLimit;
    const bool rejected = validLimit && demand > requested->testAdmissionLimitBytes;
    TerrainFaultInjection().FinishAction(faultBudgetHook, requested->faultId);
    ServiceEvidence("RVT_EVIDENCE", "[RVT_EVIDENCE] ", {
        {"event", "candidate-budget-admission"}, {"faultId", requested->faultId},
        {"worldGeneration", worldGeneration}, {"actualFrozenLimit", actualLimit},
        {"testLimit", requested->testAdmissionLimitBytes}, {"actualDemand", demand},
        {"validStrictLowerLimit", validLimit}, {"rejected", rejected},
        {"admissionBoundary", "candidate-staging-lease"}});
    Require(validLimit, "test admission limit must be strictly below the actual frozen cap");
    Require(!rejected, "candidate rejected by explicit strict-lower actual staging admission test");
}

void TerrainCompositedService::Impl::QueuePendingProducer()
{
    if (!baker)
        return;
    const auto offered = baker->PendingProducer();
    if (!offered || offered == currentProducer ||
        (worldCandidate && worldCandidate->producer == offered))
        return;
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t revision = ++nextEditRevision;
    for (const auto& surface : surfaces)
        updates[surface->desc.surfaceId] = {surface->plan, {0, 0, 1, 1},
            revision, surface->sourceRevision, true};
}

void TerrainCompositedService::Impl::ConsumeUpdates()
{
    std::map<std::string, Update> batch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (worldCandidate || updates.empty())
            return;
        batch.swap(updates);
    }
    std::shared_ptr<WorldCandidate> candidate;
    try
    {
        std::vector<std::string> ids;
        uint64_t revision = 0;
        for (const auto& surface : surfaces)
            ids.push_back(surface->desc.surfaceId);
        for (const auto& update : batch)
            revision = std::max(revision, update.second.revision);
        candidate = std::make_shared<WorldCandidate>(worldGeneration, revision, ids);
        candidate->producer = baker && baker->PendingProducer() ?
            baker->PendingProducer() : currentProducer;
        Require(bool(candidate->producer), "candidate needs an immutable producer bundle");
        candidate->budget = std::make_shared<TerrainRvtBudgetOwner>(
            frozen->peakCaps, peakOwner, frozen->terrainOwner);
        candidate->changes = batch;
        allocationOverride = candidate->budget;
        candidate->coverage = surfaces.front()->coverage;
        for (const auto& surface : surfaces)
            Require(surface->coverage == candidate->coverage,
                "World coverage input owner is not shared");
        for (uint32_t channel : {1u, 2u})
        {
            const auto& old = candidate->coverage[channel];
            const auto extent = backend.GetImageExtent(old->getImage());
            candidate->coverage[channel] = AllocateTexture(extent.width, extent.height,
                old->GetFormat(), channel == 1 ? vk::Filter::eNearest : vk::Filter::eLinear, false);
        }
        CanonicalFieldHasher identity("terrain-composited-world-candidate-v1");
        identity.AddUInt64("revision", revision);
        identity.AddString("producer", candidate->producer->Identity());
        for (const auto& update : batch)
        {
            identity.AddString("surface", update.first);
            Require(bool(update.second.plan.heightmap), "candidate needs immutable height pixels");
            identity.AddBytes("heightmap", update.second.plan.heightmap->data.data(),
                update.second.plan.heightmap->data.size());
            if (update.second.plan.layerPaint)
            {
                const auto& paint = *update.second.plan.layerPaint;
                identity.AddBytes("weights", paint.weights.data(), paint.weights.size() * sizeof(float));
                identity.AddBytes("opacity", paint.opacity.data(), paint.opacity.size());
            }
        }
        const auto dependency = identity.Finalize();
        for (const auto& previous : surfaces)
        {
            const auto replacement = materialCandidates.find(previous->desc.surfaceId);
            auto surface = std::make_unique<Surface>(Candidate(replacement == materialCandidates.end() ?
                *previous : *replacement->second));
            const bool heightSourceChanged = surface->sources[3] != previous->sources[3];
            for (auto& source : surface->sources)
                source = RetainExternalInput(source);
            Baker::SampledTexture* sourceInputs[] = {&surface->input.layerAlbedo, &surface->input.layerNormal,
                &surface->input.layerOrm, &surface->input.layerHeight};
            for (size_t index = 0; index < surface->sources.size(); ++index)
                if (surface->sources[index])
                {
                    *sourceInputs[index] = Sample(backend, surface->sources[index]);
                    surface->retained.push_back(surface->sources[index]);
                }
            const auto changed = batch.find(surface->desc.surfaceId);
            if (changed != batch.end())
            {
                const auto& next = changed->second.plan;
                Require(next.desc.compositedVT.virtualResolution == surface->plan.desc.compositedVT.virtualResolution &&
                    next.desc.pageTilesPerSide == surface->plan.desc.pageTilesPerSide &&
                    next.desc.gridResolution == surface->plan.desc.gridResolution &&
                    next.desc.size == surface->plan.desc.size &&
                    next.desc.layers.size() == surface->plan.desc.layers.size(),
                    "World candidate cannot change virtual/geometry layout; replace World");
                const bool frameChanged = next.heightmap != surface->plan.heightmap;
                surface->plan = next;
                surface->sourceRevision = changed->second.sourceRevision;
                AddCoveragePatches(*candidate, *surface, changed->second.dirty);
                if (surface->input.weightSource == Baker::WeightSource::LocalLayerArray)
                    AddLocalWeightPatches(*candidate, *surface);
                if (frameChanged)
                    AddFramePatches(*candidate, *surface);
            }
            if (heightSourceChanged)
                AddHeightCandidate(*candidate, *surface);
            for (const auto& old : surface->coverage)
                surface->retained.erase(std::remove(surface->retained.begin(), surface->retained.end(), old),
                    surface->retained.end());
            surface->coverage = candidate->coverage;
            surface->retained.insert(surface->retained.end(), surface->coverage.begin(), surface->coverage.end());
            surface->input.coveragePageTable = Sample(backend, surface->coverage[0]);
            surface->input.coverageIds = Sample(backend, surface->coverage[1]);
            surface->input.coverageWeights = Sample(backend, surface->coverage[2]);
            surface->desc.dependencyHash = dependency;
            surface->revision = revision;
            surface->input.contentIdentity = surface->desc.ContentHash().ToHex();
            surface->inputPending = false;
            surface->previousImages.clear();
            candidate->surfaces.push_back(std::move(surface));
        }
        CheckCandidateSteadyInputs(*candidate);
        const uint64_t stagingBytes = TerrainCompositedProductionGpu::MaxTransferBytes;
        const auto stagingAllocation = backend.QueryBufferMemoryRequirements(
            stagingBytes, vk::BufferUsageFlagBits::eTransferSrc).size;
        ConsumeCandidateBudgetAdmission(
            candidate->budget, stagingAllocation);
        candidate->stagingLease = candidate->budget->TryReserve(
            {stagingAllocation, stagingAllocation, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(candidate->stagingLease.has_value(), "World candidate staging exceeds frozen peak workspace");
        candidate->staging = HostBuffer(stagingBytes, vk::BufferUsageFlagBits::eTransferSrc, "WorldCandidateInputRing");
        allocationOverride.reset();
        worldCandidate = std::move(candidate);
    }
    catch (const std::exception& error)
    {
        allocationOverride.reset();
        std::cerr << "[TerrainVT] World candidate preparation kept current: " << error.what() << std::endl;
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& refresh : refreshes)
            if (!refresh.second.status.Terminal() && batch.count(refresh.second.status.ticket.surfaceId))
            {
                refresh.second.status.state = TerrainCompositedRefreshState::Failed;
                refresh.second.status.diagnostic = error.what();
                refresh.second.command.inputs.reset();
            }
    }
}

uint64_t TerrainCompositedService::Impl::RecordWorldInputs(vk::CommandBuffer command, uint64_t completedEpoch)
{
    if (!worldCandidate)
        return 0;
    auto& candidate = *worldCandidate;
    if (candidate.transaction.Phase() != TerrainCompositedSnapshotPhase::Prepared ||
        candidate.uploadEpoch > completedEpoch)
        return 0;
    candidate.recordedInitialized.clear();
    candidate.recordedClone = !candidate.cloned;
    if (!candidate.cloned)
        for (uint32_t channel : {1u, 2u})
        {
            const auto& source = surfaces.front()->coverage[channel];
            const auto& destination = candidate.coverage[channel];
            const auto extent = backend.GetImageExtent(source->getImage());
            CandidateImageBarrier(command, *source, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits::eShaderRead,
                vk::AccessFlagBits::eTransferRead);
            CandidateImageBarrier(command, *destination, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite);
            backend.RecordCopyImageRegions(command, source->getImage(), destination->getImage(),
                {vk::ImageCopy({vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {0, 0, 0},
                    {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {0, 0, 0},
                    {extent.width, extent.height, 1})});
            CandidateImageBarrier(command, *source, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eTransferRead,
                vk::AccessFlagBits::eShaderRead);
            PublishSampledImage(command, *destination, vk::ImageLayout::eTransferDstOptimal);
            candidate.initialized.insert(destination.get());
            candidate.recordedInitialized.push_back(destination.get());
        }
    uint64_t bytes = 0;
    size_t end = candidate.nextPatch;
    const size_t totalOperations = candidate.patches.size() + candidate.heightCopies.size();
    while (end < totalOperations)
    {
        if (end >= candidate.patches.size())
        {
            const auto& copy = candidate.heightCopies[end - candidate.patches.size()];
            uint64_t copyBytes = 0;
            for (const auto& region : copy.regions)
                copyBytes += uint64_t(region.extent.width) * region.extent.height * 2;
            if (bytes + copyBytes > TerrainCompositedProductionGpu::MaxTransferBytes)
                break;
            const bool initialized = candidate.initialized.count(copy.destination.get()) != 0;
            CandidateImageBarrier(command, *copy.destination,
                initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal,
                initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{},
                vk::AccessFlagBits::eTransferWrite);
            if (copy.clear)
            {
                backend.RecordClearColorImage(command, copy.destination->getImage(),
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0}));
                CandidateImageBarrier(command, *copy.destination,
                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferDstOptimal,
                    vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
            }
            vk::ImageMemoryBarrier sourceBarrier;
            sourceBarrier.setImage(copy.source->getImage())
                .setOldLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSubresourceRange({vk::ImageAspectFlagBits::eColor, TerrainHeightAtlasLayout::SourceMip, 1,
                    0, copy.source->GetArrayLayers()});
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, sourceBarrier);
            backend.RecordCopyImageRegions(command, copy.source->getImage(), copy.destination->getImage(), copy.regions);
            sourceBarrier.setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
                .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferRead).setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {}, sourceBarrier);
            PublishSampledImage(command, *copy.destination, vk::ImageLayout::eTransferDstOptimal);
            if (!initialized)
            {
                candidate.initialized.insert(copy.destination.get());
                candidate.recordedInitialized.push_back(copy.destination.get());
            }
            bytes += copyBytes;
            ++end;
            continue;
        }
        const auto& patch = candidate.patches[end];
        if (bytes + patch.bytes.size() > candidate.staging->bytes)
            break;
        std::memcpy(static_cast<uint8_t*>(candidate.staging->mapped) + bytes,
            patch.bytes.data(), patch.bytes.size());
        const bool initialized = candidate.initialized.count(patch.destination.get()) != 0;
        CandidateImageBarrier(command, *patch.destination,
            initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{},
            vk::AccessFlagBits::eTransferWrite);
        vk::BufferImageCopy region;
        region.setBufferOffset(bytes).setImageSubresource(
            {vk::ImageAspectFlagBits::eColor, 0, patch.layer, 1})
            .setImageOffset({int(patch.x), int(patch.y), 0})
            .setImageExtent({patch.width, patch.height, 1});
        backend.RecordCopyBufferToImageRegions(command, candidate.staging->buffer,
            patch.destination->getImage(), {region});
        PublishSampledImage(command, *patch.destination, vk::ImageLayout::eTransferDstOptimal);
        if (!initialized)
        {
            candidate.initialized.insert(patch.destination.get());
            candidate.recordedInitialized.push_back(patch.destination.get());
        }
        bytes += patch.bytes.size();
        ++end;
    }
    Require(end > candidate.nextPatch || candidate.recordedClone,
        "candidate input patch exceeds the shared bounded transfer contract");
    candidate.recordedEnd = end;
    candidate.inputsRecorded = true;
    if (end == totalOperations)
        candidate.transaction.RecordInputs();
    return bytes;
}

void TerrainCompositedService::Impl::DiscardWorldCandidate(const std::string& diagnostic, bool keepPendingProducer)
{
    if (!worldCandidate)
        return;
    auto candidate = std::move(worldCandidate);
    if (!keepPendingProducer && baker && candidate->producer != currentProducer)
        baker->DiscardPendingProducer(candidate->producer);
    if (candidate->runtimeStarted)
        runtime->CancelCandidate();
    candidate->transaction.Fail(diagnostic);
    const uint64_t epoch = std::max({lastCommittedEpoch, candidate->uploadEpoch,
        candidate->transaction.LastUseEpoch()});
    retiredCandidates.emplace_back(epoch, std::move(candidate));
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& entry : refreshes)
        if (!entry.second.status.Terminal() &&
            entry.second.revision == retiredCandidates.back().second->transaction.Revision())
        {
            entry.second.status.state = TerrainCompositedRefreshState::Failed;
            entry.second.status.diagnostic = diagnostic;
            entry.second.command.inputs.reset();
        }
}

void TerrainCompositedService::Impl::EmitCandidateTelemetry(TerrainCompositedService::Impl::WorldCandidate& candidate, uint64_t completedEpoch)
{
    const double now = QualityMonotonicMs();
    if (now - candidate.lastTelemetryMs < 1000 &&
        candidate.lastTelemetryPhase == candidate.transaction.Phase())
        return;
    candidate.lastTelemetryMs = now;
    candidate.lastTelemetryPhase = candidate.transaction.Phase();
    bool tables = candidate.runtimeStarted && !candidate.surfaces.empty();
    bool phaseTicketReady = false;
    uint64_t requiredPages = 0, currentPages = 0, queued = 0, reading = 0, ready = 0, produced = 0;
    uint64_t tablePublishEpoch = 0, currentRoots = 0;
    nlohmann::json surfaceProgress = nlohmann::json::array();
    for (const auto& surface : candidate.surfaces)
    {
        const auto diagnostics = runtime->CandidateDiagnostics(surface->desc.surfaceId, completedEpoch);
        if (!diagnostics || !diagnostics->candidate.present)
        {
            tables = false;
            continue;
        }
        const auto& progress = diagnostics->candidate;
        phaseTicketReady = diagnostics->jointSwitchPrepared;
        requiredPages += progress.pages.requiredPages;
        currentPages += progress.pages.currentPages;
        queued += progress.pages.queued;
        reading += progress.pages.reading;
        ready += progress.pages.ready;
        produced += progress.pages.produced;
        currentRoots += progress.pages.rootCurrent;
        tablePublishEpoch = std::max(tablePublishEpoch, progress.tablePublishEpoch);
        tables = tables && progress.initialized && !progress.pages.retired &&
            progress.pages.rootCurrent && progress.pages.currentPages == progress.pages.requiredPages &&
            progress.tablePublishEpoch <= completedEpoch;
        surfaceProgress.push_back({{"surfaceId", surface->desc.surfaceId},
            {"revision", progress.pages.revision}, {"requiredPages", progress.pages.requiredPages},
            {"currentPages", progress.pages.currentPages}, {"queued", progress.pages.queued},
            {"reading", progress.pages.reading}, {"ready", progress.pages.ready},
            {"produced", progress.pages.produced}, {"rootCurrent", progress.pages.rootCurrent},
            {"initialized", progress.initialized}, {"tablePublishEpoch", progress.tablePublishEpoch}});
    }
    std::optional<TerrainCompositedPageDiagnostics> poolDiagnostics;
    if (runtime && !surfaces.empty())
    {
        const auto& surface = *surfaces.front();
        poolDiagnostics = runtime->ProductionDiagnostics(runtime->Token({surface.desc.surfaceId,
            surface.desc.Layout().MipCount() - 1, 0, 0}), completedEpoch);
    }
    uint64_t outstanding = 0;
    for (const auto& task : productionTasks)
        if (task.page && task.candidateRevision == candidate.transaction.Revision())
            ++outstanding;
    nlohmann::json tickets = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& entry : refreshes)
            if (!entry.second.status.Terminal() &&
                entry.second.revision == candidate.transaction.Revision())
                tickets.push_back(entry.first);
    }
    ServiceEvidence("RVT_CANDIDATE_STATE", "[RVT_CANDIDATE_STATE] ", nlohmann::json{
        {"worldGeneration", worldGeneration}, {"revision", candidate.transaction.Revision()},
        {"phase", static_cast<int>(candidate.transaction.Phase())}, {"monotonicMs", now},
        {"inputEpoch", candidate.transaction.InputEpoch()}, {"inputFenceDone",
            candidate.transaction.InputEpoch() != 0 && candidate.transaction.InputEpoch() <= completedEpoch},
        {"expectedRootCount", candidate.transaction.ExpectedRootCount()},
        {"completedRootCount", candidate.transaction.CompletedRootCount()},
        {"requiredPageCount", candidate.requiredPageCount},
        {"nativeRequiredPageCount", requiredPages}, {"nativeCurrentPageCount", currentPages},
        {"nativeCurrentRootCount", currentRoots}, {"queued", queued}, {"reading", reading},
        {"readyPayloads", ready}, {"producedPendingTable", produced},
        {"tablePublishEpoch", tablePublishEpoch}, {"phaseTicketReady", phaseTicketReady},
        {"poolFreeSlots", poolDiagnostics ? nlohmann::json(poolDiagnostics->freeSlots) : nlohmann::json()},
        {"poolRetiringSlots", poolDiagnostics ? nlohmann::json(poolDiagnostics->retiringSlots) : nlohmann::json()},
        {"surfaceProgress", std::move(surfaceProgress)},
        {"completedProductionCount", candidate.completedProductionCount},
        {"outstandingProductionTasks", outstanding}, {"requiredPagesCurrent", tables},
        {"tableFenceDone", tables}, {"lastSubmittedEpoch", lastCommittedEpoch},
        {"completedEpoch", completedEpoch}, {"statusLastUseEpoch", candidate.transaction.LastUseEpoch()},
        {"switchBlocker", candidate.switchBlocker}, {"tickets", std::move(tickets)},
        {"activeProducerIdentity", currentProducer ? currentProducer->Identity() : ""},
        {"candidateProducerIdentity", candidate.producer ? candidate.producer->Identity() : ""},
        {"candidateInputIdentity", candidate.surfaces.empty() ? "" : candidate.surfaces.front()->input.contentIdentity},
        {"activeInputIdentity", surfaces.empty() ? "" : surfaces.front()->input.contentIdentity},
        {"candidateOwnerGpuBytes", candidate.budget ? candidate.budget->Statistics().current.gpuBytes : 0}
    });
}

void TerrainCompositedService::Impl::AdvanceWorldCandidate(uint64_t completedEpoch)
{
    if (!worldCandidate)
        return;
    if (worldCandidate->transaction.Phase() == TerrainCompositedSnapshotPhase::Failed)
    {
        DiscardWorldCandidate(worldCandidate->transaction.Diagnostic());
        return;
    }
    auto& candidate = *worldCandidate;
    EmitCandidateTelemetry(candidate, completedEpoch);
    if (candidate.transaction.InputsCompleted(completedEpoch) && !candidate.runtimeStarted)
    {
        std::vector<TerrainCompositedVTBuildDesc> descriptions;
        for (const auto& surface : candidate.surfaces)
            descriptions.push_back(surface->desc);
        try
        {
            candidate.requiredPageCount = runtime->SelectedPages().size();
            runtime->BeginCandidate(descriptions, generation, candidate.transaction.Revision(), candidate.budget);
            candidate.runtimeStarted = true;
            candidate.switchBlocker = "roots-required-pages-table-fence";
            std::vector<InputPatch>().swap(candidate.patches);
            std::vector<HeightCopy>().swap(candidate.heightCopies);
            candidate.cpuLeases.clear();
            candidate.staging.reset();
            candidate.stagingLease.reset();
        }
        catch (const std::exception& error)
        {
            DiscardWorldCandidate(error.what());
            return;
        }
    }
    if (!candidate.runtimeStarted)
        return;
    if (candidate.transaction.Phase() != TerrainCompositedSnapshotPhase::Ready &&
        !candidate.transaction.ConfirmTables(completedEpoch, runtime->CandidateReady(completedEpoch)))
        return;
    JointSwitchWorldCandidate(completedEpoch);
}

void TerrainCompositedService::Impl::JointSwitchWorldCandidate(uint64_t completedEpoch)
{
    auto candidate = worldCandidate;
    std::vector<TerrainCompositedMaterialBinding> bindings;
    std::optional<TerrainRvtBudgetLease> preparationLease;
    std::unique_ptr<MaterialInstanceStateBatch> materialBatch;
    std::unique_ptr<TerrainCompositedPageSourceBatch> pageSources;
    std::optional<TerrainCompositedProductionGpuResources::PhaseTransitionTicket> leafPhases;
    std::optional<PreparedWorldTextureBindings> worldBatch;
    DescriptorBindingSnapshot oldDescriptorIdentity;
    DescriptorBindingSnapshot descriptorSnapshot;
    candidate->switchBlocker = "joint-preparation";
    try
    {
        Require(candidate->producer == currentProducer ||
            (baker && baker->PendingProducer() == candidate->producer),
            "candidate producer was superseded before joint publication");
        CheckCandidateSteadyInputs(*candidate);
        preparationLease = candidate->budget->TryReserve({0, 8 * 1024 * 1024, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(preparationLease.has_value(), "joint material/page-source preparation exceeds peak CPU budget");
        std::vector<MaterialInstanceStateBatch::Change> materialChanges;
        std::vector<TerrainBuildPlan> plans;
        bindings.reserve(candidate->surfaces.size());
        plans.reserve(candidate->surfaces.size());
        for (const auto& surface : candidate->surfaces)
        {
            const auto binding = runtime->CandidateBinding(surface->desc.surfaceId);
            Require(binding.compositedPageTable && binding.u_compositedEnabled > 0,
                "candidate material binding has no current root");
            bindings.push_back(binding);
            plans.push_back(surface->plan);
            publishedRevisions.try_emplace(surface->desc.surfaceId, 0);
            for (const auto& material : surface->materials)
                materialChanges.push_back(MaterialChange(*surface, material, binding));
        }
        auto& cache = RendererResourceCache::GetInstance();
        oldDescriptorIdentity = cache.CaptureDescriptorBindingVersion();
        Require(oldDescriptorIdentity->budget.owner == frozen->runtime.owner &&
            oldDescriptorIdentity->budget.gpuMetadataReserve == descriptorMetadataReserve,
            "World descriptor snapshot belongs to a different budget");
        auto preparedWorld = cache.PrepareWorldTextureBindings(worldGeneration, oldDescriptorIdentity,
            {{"terrainPageTable", candidate->coverage[0]}, {"terrainLayerIds", candidate->coverage[1]},
             {"terrainLayerWeights", candidate->coverage[2]}},
            oldDescriptorIdentity->budget);
        worldBatch.emplace(std::move(preparedWorld));
        descriptorSnapshot = worldBatch->Snapshot();
        materialBatch = std::make_unique<MaterialInstanceStateBatch>(std::move(materialChanges));
        pageSources = std::make_unique<TerrainCompositedPageSourceBatch>(streaming.pageManager, plans);
        Require(materialBatch->MetadataBytes() + pageSources->MetadataBytes() <= preparationLease->Cost().cpuBytes,
            "joint preparation exceeded its admitted metadata footprint");
        retiredInputs.reserve(retiredInputs.size() + 1);
        retiredSurfaces.reserve(retiredSurfaces.size() + 1);
        if (!pageSources->CanCommitNoThrow() || !materialBatch->TryLockNoThrow())
        {
            candidate->switchBlocker = "page-source-or-material-lock";
            return;
        }
        if (!worldBatch->TryLockNoThrow())
        {
            candidate->switchBlocker = "world-binding-cache-trylock";
            materialBatch->CancelNoThrow();
            return;
        }
        std::vector<TerrainRvtBudgetPhaseChange> changes;
        std::vector<TerrainCompositedProductionGpuResources::PhaseTransition> uses;
        if (descriptorSnapshot->metadata)
            changes.push_back({*descriptorSnapshot->metadata, TerrainRvtResourcePhase::Active});
        if (oldDescriptorIdentity->metadata)
            changes.push_back({*oldDescriptorIdentity->metadata, TerrainRvtResourcePhase::Retiring});
        for (const auto& use : candidate->inputs->uses)
            uses.push_back({use.second, TerrainRvtResourcePhase::Active});
        if (currentInputs)
            for (const auto& use : currentInputs->uses)
                uses.push_back({use.second, TerrainRvtResourcePhase::Retiring});
        for (const auto& lease : candidate->inputs->exclusiveLeases)
            changes.push_back({lease.second, TerrainRvtResourcePhase::Active});
        if (currentInputs)
            for (const auto& lease : currentInputs->exclusiveLeases)
                if (!candidate->inputs->exclusiveLeases.count(lease.first))
                    changes.push_back({lease.second, TerrainRvtResourcePhase::Retiring});
        if (candidate->inputs->metadata)
            changes.push_back({*candidate->inputs->metadata, TerrainRvtResourcePhase::Active});
        if (currentInputs && currentInputs->metadata)
            changes.push_back({*currentInputs->metadata, TerrainRvtResourcePhase::Retiring});
        for (const auto& surface : candidate->surfaces)
        {
            const auto pixels = sourceSnapshots->PreparePromote(
                surface->plan, TerrainRvtResourcePhase::Active);
            changes.insert(changes.end(), pixels.begin(), pixels.end());
        }
        auto preparedLeaves = TerrainCompositedProductionGpuResources::PrepareTransitions(uses, *candidate->budget);
        if (!preparedLeaves)
        {
            candidate->switchBlocker = "leaf-ticket-admission";
            worldBatch->CancelNoThrow();
            materialBatch->CancelNoThrow();
            return;
        }
        leafPhases.emplace(std::move(*preparedLeaves));
        if (!leafPhases->TryLockLeavesNoThrow())
        {
            candidate->switchBlocker = "leaf-trylock";
            leafPhases->CancelNoThrow();
            worldBatch->CancelNoThrow();
            materialBatch->CancelNoThrow();
            return;
        }
        changes.insert(changes.end(), leafPhases->BudgetChanges().begin(), leafPhases->BudgetChanges().end());
        if (!runtime->PrepareJointSwitch(completedEpoch, changes))
        {
            candidate->switchBlocker = "runtime-phase-ticket-prepare";
            leafPhases->CancelNoThrow();
            worldBatch->CancelNoThrow();
            materialBatch->CancelNoThrow();
            return;
        }
    }
    catch (const std::exception& error)
    {
        runtime->CancelPreparedJointSwitch();
        if (leafPhases)
            leafPhases->CancelNoThrow();
        if (worldBatch)
            worldBatch->CancelNoThrow();
        if (materialBatch)
            materialBatch->CancelNoThrow();
        DiscardWorldCandidate(error.what());
        return;
    }
    if (!runtime->CommitCandidate())
    {
        candidate->switchBlocker = "runtime-phase-ticket-commit-retry";
        runtime->CancelPreparedJointSwitch();
        leafPhases->CancelNoThrow();
        worldBatch->CancelNoThrow();
        materialBatch->CancelNoThrow();
        return;
    }
    leafPhases->FinalizeNoThrow();
    pageSources->CommitNoThrow();
    worldBatch->CommitNoThrow();
    materialBatch->CommitNoThrow();
    if (candidate->producer != currentProducer &&
        !baker->PublishProducer(candidate->producer))
        std::terminate();
    currentProducer = candidate->producer;
    candidate->transaction.PublishCommittedNoThrow();
    if (currentInputs)
    {
        retiredInputs.emplace_back(lastCommittedEpoch, std::move(currentInputs));
    }
    retiredSurfaces.emplace_back(lastCommittedEpoch, std::move(surfaces));
    surfaces = std::move(candidate->surfaces);
    for (const auto& surface : surfaces)
        publishedRevisions.at(surface->desc.surfaceId) = surface->revision;
    for (const auto& change : candidate->changes)
        materialCandidates.erase(change.first);
    currentInputs = candidate->inputs;
    streaming.pageResources.pageTable = candidate->coverage[0];
    streaming.pageResources.layerIds = candidate->coverage[1];
    streaming.pageResources.layerWeights = candidate->coverage[2];
    worldCandidate.reset();
    RequestTerrainCompositedQualityRevalidation(worldGeneration);
    interactiveReady.store(false, std::memory_order_release);
    std::cout << "[RVT_WORLD_CANDIDATE] revision=" << candidate->transaction.Revision()
        << " jointSwitch=committed lastOldDrawEpoch=" << lastCommittedEpoch
        << " worldDescriptorRevision=" << descriptorSnapshot->revision
        << " descriptorWorldGeneration=" << descriptorSnapshot->worldGeneration << std::endl;
    RuntimeEvidence::Record("RVT_WORLD_CANDIDATE", nlohmann::json{
        {"worldGeneration", worldGeneration}, {"revision", candidate->transaction.Revision()},
        {"jointSwitch", "committed"}, {"lastOldDrawEpoch", lastCommittedEpoch},
        {"worldDescriptorRevision", descriptorSnapshot->revision},
        {"descriptorWorldGeneration", descriptorSnapshot->worldGeneration},
        {"producerIdentity", currentProducer ? currentProducer->Identity() : ""}});
    if (RuntimeEvidence::IsEnabled())
    {
        try
        {
            Require(!recording, "active publication observation requires the serial committed safe point");
            std::vector<TerrainServicePublishedSurfaceView> views;
            views.reserve(surfaces.size());
            for (const auto& surface : surfaces)
                views.push_back({surface->desc.surfaceId, surface->revision,
                    publishedRevisions.at(surface->desc.surfaceId), surface->input.contentIdentity});
            auto observation = BuildTerrainServicePublishedObservation(*runtime, views,
                worldGeneration, candidate->transaction.Revision(), descriptorSnapshot->revision,
                descriptorSnapshot->worldGeneration, completedEpoch,
                candidate->transaction.InputEpoch(), candidate->transaction.LastUseEpoch(),
                QualityMonotonicMs());
            Require(observation.has_value(), "committed active observation identity/fence validation failed");
            observation->summary["producerIdentity"] =
                currentProducer ? currentProducer->Identity() : "";
            RuntimeEvidence::Record("RVT_EVIDENCE", observation->summary);
            for (const auto& page : observation->pages)
                RuntimeEvidence::Record("RVT_EVIDENCE", page);
        }
        catch (const std::exception& error)
        {
            RuntimeEvidence::MarkIncomplete();
            std::cerr << "[TerrainVT] active publication observation incomplete: " << error.what() << std::endl;
        }
    }
}

void TerrainCompositedService::Impl::EmitBudgetObservation(uint64_t completedEpoch)
{
    if (!budgetSnapshotRequested.exchange(false, std::memory_order_acq_rel) ||
        !RuntimeEvidence::IsEnabled())
        return;
    try
    {
        Require(frozen && frozen->terrainOwner, "outer ledger observation has no actual owner");
        auto scratch = frozen->terrainOwner->TryReserve({0, 32768, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
        Require(scratch.has_value(), "outer ledger observation exceeds charged diagnostic workspace");
        auto payload = BuildTerrainServiceOuterLedgerObservation(*frozen->terrainOwner,
            worldGeneration, completedEpoch, QualityMonotonicMs());
        payload["diagnosticWorkspaceIncludedInTotals"] = TerrainServiceCostJson(scratch->Cost());
        payload["configuredDescriptorDriverGpuBytesPerWorld"] = frozen->metadataGpuBytes;
        payload["serviceDescriptorReserveActualCost"] = descriptorMetadataReserve ?
            TerrainServiceCostJson(descriptorMetadataReserve->Cost()) : nlohmann::json();
        payload["serviceDescriptorReserveAlreadyIncludedInTotals"] = true;
        payload["allocationInventoryScope"] = "this-service-current-candidate-retiring-views";
        payload["allocationInventoryCoversAllWorlds"] = false;
        payload["trigger"] = "service-report-stats-request";
        RuntimeEvidence::Record("RVT_EVIDENCE", payload);
        size_t rows = 0;
        const auto emit = [&](const std::shared_ptr<WorldLeafOwner>& input,
            const char* viewPhase, uint64_t retireEpoch) {
            if (!input)
                return;
            for (const auto& entry : input->uses)
            {
                Require(++rows <= TerrainRvtBudgetOwner::MaxRegistryEntries,
                    "explicit leaf inventory exceeds bounded native registry limit");
                const auto allocation = entry.second->GetAllocation();
                RuntimeEvidence::Record("RVT_EVIDENCE", {
                    {"event", "terrain-leaf-allocation-view"}, {"worldGeneration", worldGeneration},
                    {"ownerViewIdentity", reinterpret_cast<std::uintptr_t>(input.get())},
                    {"viewPhase", viewPhase}, {"retireEpoch", retireEpoch},
                    {"allocationIdentity", allocation.allocationIdentity},
                    {"actualCost", TerrainServiceCostJson(allocation.actualCost)},
                    {"nativeUsePhase", static_cast<uint32_t>(entry.second->Phase())},
                    {"costAlreadyIncludedInOuterTotals", true},
                    {"deduplicateByAllocationIdentity", true},
                    {"completedEpoch", completedEpoch}});
            }
            for (const auto& entry : input->exclusiveLeases)
            {
                Require(++rows <= TerrainRvtBudgetOwner::MaxRegistryEntries,
                    "explicit exclusive inventory exceeds bounded native registry limit");
                RuntimeEvidence::Record("RVT_EVIDENCE", {
                    {"event", "terrain-exclusive-allocation-view"}, {"worldGeneration", worldGeneration},
                    {"resourceProcessAddress", reinterpret_cast<std::uintptr_t>(entry.first)},
                    {"viewPhase", viewPhase}, {"retireEpoch", retireEpoch},
                    {"actualCost", TerrainServiceCostJson(entry.second.Cost())},
                    {"nativeLeasePhase", static_cast<uint32_t>(entry.second.Phase())},
                    {"leafAllocationIdentityAvailable", false},
                    {"costAlreadyIncludedInOuterTotals", true},
                    {"completedEpoch", completedEpoch}});
            }
        };
        emit(currentInputs, "active", 0);
        if (worldCandidate)
            emit(worldCandidate->inputs, "candidate", 0);
        for (const auto& item : retiredInputs)
            emit(item.second, "retiring", item.first);
        for (const auto& item : retiredCandidates)
            emit(item.second->inputs, "retiring-candidate", item.first);
        RuntimeEvidence::Record("RVT_EVIDENCE", {
            {"event", "renderer-terrain-ledger-end"}, {"worldGeneration", worldGeneration},
            {"rows", rows}, {"complete", true}, {"completedEpoch", completedEpoch},
            {"allocationInventoryScope", "this-service-current-candidate-retiring-views"}});
    }
    catch (const std::exception& error)
    {
        RuntimeEvidence::MarkIncomplete();
        std::cerr << "[TerrainVT] outer ledger observation incomplete: " << error.what() << std::endl;
    }
}

bool TerrainCompositedService::Impl::PrepareDynamicRefresh(TerrainCompositedService::Impl::Refresh& refresh)
{
    if (worldCandidate || !materialCandidates.empty())
        return false;
    Require(frozen.has_value() && bool(frozen->runtime.owner),
        "dynamic refresh requires a charged World owner");
    auto metadata = frozen->runtime.owner->TryReserve({0, 8 * 1024 * 1024, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    Require(metadata.has_value(), "dynamic material preparation exceeds CPU workspace");
    std::vector<MaterialInstanceStateBatch::Change> changes;
    std::vector<std::pair<Surface*, std::vector<std::shared_ptr<const MaterialInstanceStateSnapshot>>>> states;
    for (const auto& id : refresh.affected)
    {
        const auto found = std::find_if(surfaces.begin(), surfaces.end(),
            [&](const auto& surface) { return surface->desc.surfaceId == id; });
        Require(found != surfaces.end(), "dynamic refresh consumer is no longer registered");
        auto& surface = **found;
        std::vector<std::shared_ptr<const MaterialInstanceStateSnapshot>> values;
        values.reserve(surface.materials.size());
        for (size_t index = 0; index < surface.materials.size(); ++index)
        {
            const auto& material = surface.materials[index];
            const auto& path = index ? surface.plan.desc.detailMaterialInstancePath :
                surface.plan.desc.materialInstancePath;
            const auto pending = materialIntents.find(path);
            auto state = pending == materialIntents.end() ?
                AuthorMaterialState(material->CaptureStateSnapshot(), surface.localWeights,
                    surface.authorWeights,
                    index < surface.authoredWeightIdentities.size()
                        ? surface.authoredWeightIdentities[index] : std::nullopt) :
                pending->second;
            for (const auto* name : {"u_uvScale", "u_heightBlendStrength", "u_terrainWeightMapMode"})
            {
                const auto value = state->parameters.find(name);
                if (value != state->parameters.end())
                    Require(std::get<float>(value->second) == Parameter(*material, name, 0.f),
                        "dynamic-only refresh contains a static bake parameter change");
            }
            for (const auto& texture : state->textures)
                Require(texture.second.texture == material->GetTexture(texture.first) ||
                    (texture.first == "layerWeightMaps" &&
                        texture.second.texture == surface.authorWeights &&
                        material->GetTexture(texture.first) == surface.localWeights),
                    "dynamic texture replacement requires an admitted World input candidate");
            changes.push_back({material, state->parameters, {}});
            values.push_back(std::move(state));
        }
        publishedRevisions.try_emplace(id, 0);
        states.emplace_back(&surface, std::move(values));
    }
    MaterialInstanceStateBatch transaction(std::move(changes));
    Require(transaction.MetadataBytes() <= metadata->Cost().cpuBytes,
        "dynamic material batch exceeded its CPU admission");
    if (!transaction.TryLockNoThrow())
        return false;
    RequestTerrainCompositedQualityRevalidation(worldGeneration);
    transaction.CommitNoThrow();
    for (auto& state : states)
    {
        state.first->materialStates.swap(state.second);
        state.first->revision = refresh.revision;
        state.first->sourceRevision = refresh.command.sourceRevision;
    }
    interactiveReady.store(false, std::memory_order_release);
    refresh.dynamicCommitted = true;
    return true;
}

void TerrainCompositedService::Impl::PrepareRefreshes()
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<TerrainCompositedVTLayout> layouts;
    for (const auto& surface : surfaces)
        layouts.push_back(surface->desc.Layout());
    for (auto& entry : refreshes)
    {
        auto& refresh = entry.second;
        if (refresh.status.state != TerrainCompositedRefreshState::Queued)
            continue;
        try
        {
            for (const auto& change : refresh.command.changes)
                Require(change.kind != TerrainCompositedDependencyChange::Placement &&
                    change.kind != TerrainCompositedDependencyChange::LayerMapping &&
                    change.kind != TerrainCompositedDependencyChange::Filter,
                    "layout/filter change needs a rebuilt World publication");
            const bool generatorChange = std::any_of(refresh.command.changes.begin(),
                refresh.command.changes.end(), [](const auto& change) {
                    return change.kind == TerrainCompositedDependencyChange::Generator;
                });
            if (generatorChange)
            {
                if (!baker || !baker->PendingProducer())
                    continue;
                for (const auto& surface : surfaces)
                {
                    refresh.affected.insert(surface->desc.surfaceId);
                    const auto& plan = surface->desc.surfaceId == refresh.status.ticket.surfaceId ?
                        *refresh.command.inputs : surface->plan;
                    updates[surface->desc.surfaceId] = {plan, {0, 0, 1, 1}, refresh.revision,
                        refresh.command.sourceRevision, true};
                }
                refresh.status.state = TerrainCompositedRefreshState::Prepared;
                continue;
            }
            const auto affected = TerrainCompositedDirtyPropagation(adjacency, layouts)
                .Collect(refresh.command.changes);
            const bool dynamicOnly = std::all_of(refresh.command.changes.begin(),
                refresh.command.changes.end(), [](const auto& change) {
                    using Change = TerrainCompositedDependencyChange;
                    return change.kind == Change::DynamicDetail || change.kind == Change::DynamicMacro ||
                        change.kind == Change::DynamicGlobal || change.kind == Change::DynamicTessellation;
                });
            if (dynamicOnly)
            {
                for (const auto& consumer : affected.surfaces)
                    refresh.affected.insert(consumer.surfaceId);
                if (!PrepareDynamicRefresh(refresh))
                    continue;
                refresh.status.state = TerrainCompositedRefreshState::Prepared;
                continue;
            }
            for (const auto& consumer : affected.surfaces)
            {
                const auto found = std::find_if(surfaces.begin(), surfaces.end(),
                    [&](const auto& surface) { return surface->desc.surfaceId == consumer.surfaceId; });
                Require(found != surfaces.end(), "dirty propagation referenced an unknown surface");
                refresh.affected.insert(consumer.surfaceId);
                if (!consumer.consumers.basePages)
                    continue;
                const auto& plan = consumer.surfaceId == refresh.status.ticket.surfaceId ?
                    *refresh.command.inputs : (*found)->plan;
                TerrainCompositedVTUVRect dirty{1, 1, 0, 0};
                const auto& layout = (*found)->desc.Layout();
                bool foundMipZero = false;
                for (const auto& page : affected.pages)
                    if (page.surfaceId == consumer.surfaceId && page.mip == 0)
                    {
                        foundMipZero = true;
                        const double width = layout.Mip(0).width;
                        const double height = layout.Mip(0).height;
                        dirty.minU = std::min(dirty.minU, double(page.x * 128) / width);
                        dirty.minV = std::min(dirty.minV, double(page.y * 128) / height);
                        dirty.maxU = std::max(dirty.maxU, std::min(1.0, double((page.x + 1) * 128) / width));
                        dirty.maxV = std::max(dirty.maxV, std::min(1.0, double((page.y + 1) * 128) / height));
                    }
                if (foundMipZero)
                    updates[consumer.surfaceId] = {plan, dirty, refresh.revision,
                        refresh.command.sourceRevision, true};
            }
            refresh.status.state = TerrainCompositedRefreshState::Prepared;
        }
        catch (const std::exception& error)
        {
            refresh.status.state = TerrainCompositedRefreshState::Failed;
            refresh.status.diagnostic = error.what();
            refresh.command.inputs.reset();
        }
    }
}

void TerrainCompositedService::Impl::UpdateRefreshes(bool submitted)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto runtimeStats = runtime ? runtime->Statistics() : TerrainCompositedRuntimeStats{};
    for (auto& entry : refreshes)
    {
        auto& refresh = entry.second;
        if (refresh.status.Terminal())
        {
            refresh.command.inputs.reset();
            continue;
        }
        if (refresh.status.state == TerrainCompositedRefreshState::Queued)
            continue;
        if (refresh.dynamicCommitted)
        {
            if (submitted)
            {
                refresh.status.state = TerrainCompositedRefreshState::Published;
                refresh.status.publishedRevision = refresh.revision;
                for (const auto& id : refresh.affected)
                    publishedRevisions.at(id) = refresh.revision;
                refresh.command.inputs.reset();
            }
            continue;
        }
        bool complete = true;
        for (const auto& id : refresh.affected)
        {
            const auto surface = std::find_if(surfaces.begin(), surfaces.end(),
                [&](const auto& item) { return item->desc.surfaceId == id; });
            const auto resident = runtimeStats.surfaces.find(id);
            if (surface == surfaces.end() || resident == runtimeStats.surfaces.end() ||
                (*surface)->revision < refresh.revision ||
                updates.count(id) || materialCandidates.count(id) || (*surface)->inputPending ||
                (worldCandidate && worldCandidate->changes.count(id)) ||
                resident->second.dirty || resident->second.reading)
                complete = false;
        }
        if (submitted && refresh.status.state == TerrainCompositedRefreshState::Prepared)
            refresh.status.state = TerrainCompositedRefreshState::Submitted;
        if (submitted && complete)
        {
            refresh.status.state = TerrainCompositedRefreshState::Published;
            refresh.status.publishedRevision = refresh.revision;
            publishedRevisions[refresh.status.ticket.surfaceId] = refresh.revision;
            refresh.command.inputs.reset();
        }
    }
}
} // namespace VL
