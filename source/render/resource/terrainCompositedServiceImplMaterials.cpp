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
TerrainCompositedService::Impl::Surface TerrainCompositedService::Impl::Candidate(const TerrainCompositedService::Impl::Surface& surface)
{
    Surface result;
    result.plan = surface.plan;
    result.model = surface.model;
    result.desc = surface.desc;
    result.authorManifest = surface.authorManifest;
    result.input = surface.input;
    result.retained = surface.retained;
    result.materials = surface.materials;
    result.materialStates = surface.materialStates;
    result.height = surface.height;
    result.revision = surface.revision;
    result.sourceRevision = surface.sourceRevision;
    result.inputBytes = surface.inputBytes;
    result.generation = surface.generation;
    result.inputPending = surface.inputPending;
    result.coverage = surface.coverage;
    result.sources = surface.sources;
    result.sourceMap = surface.sourceMap;
    result.localWeights = surface.localWeights;
    result.authorWeights = surface.authorWeights;
    result.authoredWeightIdentities = surface.authoredWeightIdentities;
    result.importedWeights = surface.importedWeights;
    result.pendingImages = surface.pendingImages;
    result.pendingHeight = surface.pendingHeight;
    result.previousImages = surface.previousImages;
    result.previewFailures = surface.previewFailures;
    return result;
}

void TerrainCompositedService::Impl::BindPlanSources(const TerrainBuildPlan& plan, const TerrainBuildPlan* previous)
{
    const auto& id = TerrainPlacementIdentity(plan);
    const auto& registrations = streaming.pageManager.Registrations();
    if (std::none_of(registrations.begin(), registrations.end(),
        [&](const auto& registration) { return registration.placementId == id; }))
        return;
    Require(bool(plan.heightmap), "PageManager source binding requires height pixels");
    Require(!plan.layerPaint || (!plan.layerPaint->Empty() &&
        plan.layerPaint->layerCount == plan.desc.layers.size()), "PageManager paint source mismatch");
    Require(!previous || !previous->heightmap ||
        (previous->heightmap->width == plan.heightmap->width &&
         previous->heightmap->height == plan.heightmap->height),
        "PageManager source dimensions changed; replace the World");
    streaming.pageManager.UpdateHeightmap(id, plan.heightmap);
    streaming.pageManager.UpdateLayerPaint(id, plan.layerPaint);
}

void TerrainCompositedService::Impl::Requeue(const std::string& identity, TerrainCompositedService::Impl::Update update)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = updates.find(identity);
    if (found == updates.end())
        updates.emplace(identity, std::move(update));
    else
    {
        auto& target = found->second;
        target.dirty.minU = std::min(target.dirty.minU, update.dirty.minU);
        target.dirty.minV = std::min(target.dirty.minV, update.dirty.minV);
        target.dirty.maxU = std::max(target.dirty.maxU, update.dirty.maxU);
        target.dirty.maxV = std::max(target.dirty.maxV, update.dirty.maxV);
        target.force = target.force || update.force;
    }
}

void TerrainCompositedService::Impl::PublishMap()
{
    std::vector<uint8_t> bytes(mapPixels.size() * sizeof(float));
    std::memcpy(bytes.data(), mapPixels.data(), bytes.size());
    const auto image = Image(32, 32, true, std::move(bytes));
    std::optional<TerrainRvtBudgetLease> lease;
    if (frozen)
    {
        TerrainCompositedProductionImageDesc description;
        description.width = description.height = 32;
        description.layers = description.mipLevels = 1;
        description.format = "rgba32-float";
        auto cost = frozen->allocations.image(description, false);
        lease = frozen->runtime.owner->TryReserve(cost,
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(lease.has_value(), "surface feedback map exceeds frozen workspace");
    }
    auto texture = Upload(backend, image, "TerrainCompositedSurfaceMap");
    if (lease)
    {
        struct Retained
        {
            TerrainRvtBudgetLease lease;
            std::shared_ptr<Texture> texture;
        };
        auto owner = std::make_shared<Retained>(Retained{std::move(*lease), std::move(texture)});
        surfaceMap = std::shared_ptr<Texture>(owner, owner->texture.get());
    }
    else
        surfaceMap = std::move(texture);
    mapDirty = false;
}

void TerrainCompositedService::Impl::Bind(TerrainCompositedService::Impl::Surface& surface, const TerrainCompositedMaterialBinding* replacement)
{
    const auto binding = replacement ? *replacement : runtime->Binding(surface.desc.surfaceId);
    for (const auto& material : surface.materials)
    {
        material->SetTextureKeepingIdentity("compositedPageTable", binding.compositedPageTable);
        material->SetTextureKeepingIdentity("compositedAlbedo", binding.compositedAlbedo);
        material->SetTextureKeepingIdentity("compositedNormal", binding.compositedNormal);
        material->SetTextureKeepingIdentity("compositedOrm", binding.compositedOrm);
        if (surface.height.heightAtlas)
        {
            material->SetTextureKeepingIdentity("terrainHeightAtlas", surface.height.heightAtlas);
            material->SetTextureKeepingIdentity("terrainHeightLayerMap", surface.height.layerMap);
        }
        material->SetParameter("u_compositedResolution",
            Eigen::Vector2f(binding.u_compositedResolution[0], binding.u_compositedResolution[1]));
        material->SetParameter("u_compositedMaxMip", binding.u_compositedMaxMip);
        material->SetParameter("u_compositedRepeat", binding.u_compositedRepeat);
        material->SetParameter("u_compositedNormalRG", binding.u_compositedNormalRG);
        material->SetParameter("u_compositedEnabled", binding.u_compositedEnabled);
        material->SetParameter("u_terrainHeightAtlasEnabled", surface.height.heightAtlas ? 1.f : 0.f);
        if (surface.sourceMap)
            material->SetTextureKeepingIdentity("terrainLayerSourceMap", surface.sourceMap);
        material->SetTextureKeepingIdentity("layerTable", streaming.layerResources.layerTable);
        const char* names[] = {"layerAlbedo", "layerNormal", "layerPbr", "layerHeight"};
        for (size_t index = 0; index < surface.sources.size(); ++index)
            material->SetTextureKeepingIdentity(names[index], surface.sources[index]);
        if (surface.localWeights)
            material->SetTextureKeepingIdentity("layerWeightMaps", surface.localWeights);
        material->SetParameter("u_uvScale", surface.input.uvScale);
        material->SetParameter("u_heightBlendStrength", surface.input.retainedFragment.heightBlendStrength);
        material->SetParameter("u_terrainWeightMapMode",
            surface.input.weightSource == Baker::WeightSource::LocalLayerArray ? 1.f : 0.f);
        if (surface.coverage[0])
        {
            material->SetTextureKeepingIdentity("pageTable", surface.coverage[0]);
            material->SetTextureKeepingIdentity("layerIds", surface.coverage[1]);
            material->SetTextureKeepingIdentity("layerWeights", surface.coverage[2]);
            material->SetParameter("u_pageTableSize", 32.f);
            material->SetParameter("u_pageResolution", 128.f);
        }
    }
}

void TerrainCompositedService::Impl::CaptureImportedWeights(TerrainCompositedService::Impl::Surface& surface, const MaterialInstance& material,
    const MaterialInstanceStateSnapshot* replacement)
{
    const auto snapshot = replacement ? *replacement : material.CaptureStateSnapshot();
    const auto binding = snapshot.textures.find("layerWeightMaps");
    Require(binding != snapshot.textures.end() && binding->second.texture &&
        binding->second.textureAssetIdentity, "local weights require an authored texture identity");
    const auto& texture = binding->second.texture;
    if ((texture == surface.localWeights || texture == surface.authorWeights) && surface.importedWeights)
    {
        return;
    }
    const bool restoredOverlay = texture == surface.localWeights;
    auto desc = LoadTextureAssetDesc(*binding->second.textureAssetIdentity);
    Require(desc.textureArray && !desc.sources.empty() &&
        desc.transfer == TextureIO::LoadOptions::Transfer::Linear,
        "local weights require a linear texture array");
    desc.semantic = HostImage::TextureSemantic::Data;
    std::vector<std::filesystem::path> paths;
    for (const auto& source : desc.sources)
        paths.push_back(CommonFunction::Path(source));
    auto image = TextureIO::LoadArray(paths,
        Texture::MaterialLoadOptionsForPath(ToTextureCreateDesc(desc), paths.front()));
    const auto extent = backend.GetImageExtent(texture->getImage());
    Require(restoredOverlay || (extent.width == image.width && extent.height == image.height &&
        texture->GetArrayLayers() == image.arrayLayers),
        "authored CPU/GPU local weight layout differs");
    Require(restoredOverlay || (image.format == HostImage::PixelFormat::RGBA8_UNORM &&
        texture->GetFormat() == vk::Format::eR8G8B8A8Unorm) ||
        (image.format == HostImage::PixelFormat::R16_UNORM &&
            texture->GetFormat() == vk::Format::eR16Unorm) ||
        (image.format == HostImage::PixelFormat::RGBA32_FLOAT &&
            texture->GetFormat() == vk::Format::eR32G32B32A32Sfloat),
        "unsupported or mismatched CPU/GPU local weight format");
    const uint64_t bytes = uint64_t(image.width) * image.height * image.arrayLayers * sizeof(float);
    Require(bytes <= WorldCpuLimit / 4 && image.data.size() <= WorldCpuLimit / 4,
        "local weight snapshot exceeds workspace budget");
    Require(cpuBytes + importedWeightBytes + image.data.size() + bytes * 2 <= WorldCpuLimit,
        "local weight snapshot exceeds World CPU budget");
    auto owner = allocationOverride ? allocationOverride : frozen->runtime.owner;
    auto lease = owner->TryReserve({0, sizeof(HostImage) + image.data.capacity(), 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "imported raw weight snapshot exceeds peak CPU admission");
    auto retained = std::make_shared<CpuAllocation>(CpuAllocation{std::move(*lease), std::move(image)});
    surface.importedWeights = std::shared_ptr<const HostImage>(retained, &retained->image);
    cpuInputOwners[surface.importedWeights.get()] = retained;
    surface.authorWeights = restoredOverlay ? nullptr : texture;
    surface.authoredWeightIdentities.assign(surface.materials.size(),
        binding->second.textureAssetIdentity);
}

void TerrainCompositedService::Impl::RefreshLocalWeights(TerrainCompositedService::Impl::Surface& surface, const TerrainBuildPlan& plan, bool deferred)
{
    Require(bool(surface.importedWeights), "missing imported local weights");
    const auto image = BuildTerrainCompositedWeightOverlay(plan, *surface.importedWeights,
        streaming.pageManager.PlacementLayerIdMap(TerrainPlacementIdentity(plan)));
    Require(gpuBytes + image.data.size() * 2 <= WorldGpuLimit,
        "local weight overlay exceeds World GPU budget");
    auto replacement = deferred ? DeferredImage(surface, image, vk::Filter::eLinear, true)
        : Upload(backend, image, "VTLocalWeightOverlay", vk::Filter::eLinear, true);
    if (surface.localWeights)
    {
        surface.previousImages.push_back(surface.localWeights);
        surface.retained.erase(std::remove(surface.retained.begin(), surface.retained.end(),
            surface.localWeights), surface.retained.end());
    }
    surface.localWeights = std::move(replacement);
    surface.input.localLayerWeights = Sample(backend, surface.localWeights);
    surface.retained.push_back(surface.localWeights);
}

void TerrainCompositedService::Impl::RecordImages(vk::CommandBuffer command, TerrainCompositedService::Impl::Surface& surface)
{
    for (const auto& upload : surface.pendingImages)
    {
        backend.RecordTransitionImageLayout(command, upload.texture->getImage(), 1,
            upload.texture->GetFormat(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal, 0, upload.layers);
        vk::BufferImageCopy region;
        region.setImageSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, upload.layers})
            .setImageExtent({upload.width, upload.height, 1});
        backend.RecordCopyBufferToImageRegions(command, upload.staging->buffer,
            upload.texture->getImage(), {region});
        PublishSampledImage(command, *upload.texture, vk::ImageLayout::eTransferDstOptimal);
    }
    for (const auto& copy : surface.pendingHeight)
    {
        const auto transition = [&](const Texture& texture, vk::ImageLayout before,
            vk::ImageLayout after, vk::AccessFlags source, vk::AccessFlags destination)
        {
            vk::ImageMemoryBarrier barrier;
            barrier.setImage(texture.getImage()).setOldLayout(before).setNewLayout(after)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(source).setDstAccessMask(destination)
                .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0,
                    texture.getMipLevels(), 0, texture.GetArrayLayers()});
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
        };
        transition(*copy.destination, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite);
        backend.RecordClearColorImage(command, copy.destination->getImage(),
            vk::ImageLayout::eTransferDstOptimal,
            vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0}));
        transition(*copy.destination, vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eTransferWrite);
        transition(*copy.source, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits::eShaderRead,
            vk::AccessFlagBits::eTransferRead);
        backend.RecordCopyImageRegions(command, copy.source->getImage(),
            copy.destination->getImage(), copy.regions);
        transition(*copy.source, vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eTransferRead,
            vk::AccessFlagBits::eShaderRead);
        transition(*copy.destination, vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eShaderRead);
    }
}

void TerrainCompositedService::Impl::RefreshHeight(TerrainCompositedService::Impl::Surface& surface)
{
    const auto& source = surface.sources[3];
    std::vector<TerrainHeightAtlasLayer> layers;
    const auto& mapping = streaming.pageManager.PlacementLayerIdMap(surface.desc.surfaceId);
    for (size_t local = 0; local < mapping.size(); ++local)
        if (surface.plan.desc.layers[local].hasHeight)
            layers.push_back({mapping[local], 0, surface.input.globalToSourceSlice[mapping[local]]});
    if (layers.empty())
        return;
    Require(source && source->GetFormat() == vk::Format::eR16Unorm &&
        source->getMipLevels() > TerrainHeightAtlasLayout::SourceMip,
        "height preview requires an R16_UNORM source with mip2");
    const auto extent = backend.GetImageExtent(source->getImage());
    const auto layout = BuildTerrainHeightAtlasLayout({{0,
        std::max(1u, extent.width >> 2), std::max(1u, extent.height >> 2), source->GetArrayLayers()}},
        layers, frozen->runtime.owner->Caps().inputGpuBytes);
    uint64_t retiredBytes = 0;
    for (const auto& item : retired)
        for (const auto& texture : item.second)
            retiredBytes += SourceTextureBytes(backend, *texture);
    Require(gpuBytes + retiredBytes + layout.totalBytes <= WorldGpuLimit,
        "height preview replacement exceeds World GPU budget");
    gpuTransientPeak = std::max(gpuTransientPeak, gpuBytes + retiredBytes + layout.totalBytes);
    TerrainHeightAtlasResources replacement;
    replacement.heightAtlas = AllocateTexture(layout.width, layout.height,
        vk::Format::eR16Unorm, vk::Filter::eLinear, true);
    std::vector<uint8_t> bytes(layout.layerMap.size() * 16);
    std::memcpy(bytes.data(), layout.layerMap.data(), bytes.size());
    replacement.layerMap = DeferredImage(surface,
        Image(static_cast<uint32_t>(layout.layerMap.size()), 1, true, std::move(bytes)),
        vk::Filter::eNearest);
    replacement.width = layout.width;
    replacement.height = layout.height;
    replacement.pageCount = static_cast<uint32_t>(layout.pages.size());
    replacement.layerCount = static_cast<uint32_t>(layout.layerMap.size());
    replacement.atlasBytes = layout.atlasBytes;
    replacement.layerMapBytes = layout.layerMapBytes;
    replacement.totalBytes = layout.totalBytes;
    HeightCopy copy{source, replacement.heightAtlas, {}};
    for (uint32_t pageIndex = 0; pageIndex < layout.pages.size(); ++pageIndex)
        for (const auto& region : BuildTerrainHeightAtlasCopies(layout, pageIndex))
        {
            Require(copy.regions.size() < 32ull * 1024 * 1024 / sizeof(vk::ImageCopy),
                "height preview copy workspace exceeds 32 MiB");
            copy.regions.push_back(vk::ImageCopy(
                {vk::ImageAspectFlagBits::eColor, 2, layout.pages[pageIndex].sourceSlice, 1},
                {int(region.sourceX), int(region.sourceY), 0},
                {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                {int(region.destinationX), int(region.destinationY), 0},
                {region.width, region.height, 1}));
        }
    surface.pendingHeight.push_back(std::move(copy));
    if (surface.height.heightAtlas)
        surface.previousImages.push_back(surface.height.heightAtlas);
    if (surface.height.layerMap)
        surface.previousImages.push_back(surface.height.layerMap);
    surface.height = std::move(replacement);
}

void TerrainCompositedService::Impl::AddHeightCandidate(TerrainCompositedService::Impl::WorldCandidate& candidate, TerrainCompositedService::Impl::Surface& surface)
{
    const auto oldAtlas = surface.height.heightAtlas;
    const auto oldMap = surface.height.layerMap;
    ReserveCandidateCpu(candidate, 8 * 1024 * 1024);
    RefreshHeight(surface);
    for (const auto& upload : surface.pendingImages)
    {
        const uint32_t rowBytes = upload.width * 16;
        Require(rowBytes <= TerrainCompositedProductionGpu::MaxTransferBytes,
            "height layer map row exceeds bounded upload");
        InputPatch patch{upload.texture, 0, 0, 0, upload.width, upload.height, {}};
        const auto* bytes = static_cast<const uint8_t*>(upload.staging->mapped);
        patch.bytes.assign(bytes, bytes + uint64_t(rowBytes) * upload.height);
        candidate.patches.push_back(std::move(patch));
    }
    surface.pendingImages.clear();
    for (auto& copy : surface.pendingHeight)
    {
        bool clear = true;
        for (size_t begin = 0; begin < copy.regions.size(); begin += 8)
        {
            const size_t end = std::min(begin + 8, copy.regions.size());
            HeightCopy chunk{copy.source, copy.destination, {}, clear};
            chunk.regions.assign(copy.regions.begin() + begin, copy.regions.begin() + end);
            candidate.heightCopies.push_back(std::move(chunk));
            clear = false;
        }
    }
    surface.pendingHeight.clear();
    for (const auto& texture : {oldAtlas, oldMap})
        surface.retained.erase(std::remove(surface.retained.begin(), surface.retained.end(), texture),
            surface.retained.end());
    surface.previousImages.clear();
    surface.retained.push_back(surface.height.heightAtlas);
    surface.retained.push_back(surface.height.layerMap);
}

void TerrainCompositedService::Impl::ConsumeMaterials()
{
    std::map<std::string, std::shared_ptr<MaterialInstance>> batch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        batch.swap(materialUpdates);
    }
    for (const auto& event : batch)
        for (const auto& registered : surfaces)
        {
            const auto& surface = registered;
            size_t materialIndex = 0;
            if (event.first == surface->plan.desc.materialInstancePath)
                materialIndex = 0;
            else if (event.first == surface->plan.desc.detailMaterialInstancePath &&
                surface->materials.size() > 1)
                materialIndex = 1;
            else
                continue;
            if (!surface->pendingImages.empty() || !surface->pendingHeight.empty())
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!materialUpdates.count(event.first))
                    materialUpdates.emplace(event.first, event.second);
                continue;
            }
            try
            {
            const auto pending = materialCandidates.find(registered->desc.surfaceId);
            auto surface = std::make_unique<Surface>(Candidate(
                pending == materialCandidates.end() ? *registered : *pending->second));
            const auto& material = event.second;
            const auto intent = materialIntents.at(event.first);
            const auto parameter = [&](const char* name, float fallback) {
                const auto found = intent->parameters.find(name);
                return found == intent->parameters.end() ? fallback : std::get<float>(found->second);
            };
            const auto textureFor = [&](const char* name) {
                const auto found = intent->textures.find(name);
                if (found == intent->textures.end() && std::string_view(name) == "layerWeightMaps")
                    return surface->localWeights;
                return found == intent->textures.end() ? std::shared_ptr<Texture>{} : found->second.texture;
            };
            Require(material->GetBaseMaterial().lock() &&
                material->GetBaseMaterial().lock()->GetShaderName() == "terrain",
                "material preview must use the shared M_terrain evaluator");
            const float uv = parameter("u_uvScale", surface->input.uvScale);
            const float heightBlend = parameter("u_heightBlendStrength",
                surface->input.retainedFragment.heightBlendStrength);
            const auto weightMode = parameter("u_terrainWeightMapMode", 0) > 0.5f
                ? Baker::WeightSource::LocalLayerArray : Baker::WeightSource::CoverageAtlas;
            bool changed = uv != surface->input.uvScale ||
                heightBlend != surface->input.retainedFragment.heightBlendStrength ||
                weightMode != surface->input.weightSource;
            bool heightChanged = false;
            const char* names[] = {"layerAlbedo", "layerNormal", "layerPbr", "layerHeight"};
            Baker::SampledTexture* inputs[] = {&surface->input.layerAlbedo, &surface->input.layerNormal,
                &surface->input.layerOrm, &surface->input.layerHeight};
            for (size_t index = 0; index < 4; ++index)
            {
                const auto texture = textureFor(names[index]);
                Require(bool(texture), std::string("material preview missing source texture: ") + names[index]);
                if (texture == surface->sources[index])
                    continue;
                changed = true;
                heightChanged = heightChanged || index == 3;
                *inputs[index] = Sample(backend, texture);
                surface->previousImages.push_back(surface->sources[index]);
                surface->retained.erase(std::remove(surface->retained.begin(), surface->retained.end(),
                    surface->sources[index]), surface->retained.end());
                surface->sources[index] = texture;
                surface->retained.push_back(texture);
            }
            if (weightMode == Baker::WeightSource::LocalLayerArray)
            {
                const auto texture = textureFor("layerWeightMaps");
                Require(bool(texture), "material preview missing layer weight array");
                if (texture != surface->localWeights && texture != surface->authorWeights)
                {
                    changed = true;
                    CaptureImportedWeights(*surface, *material, intent.get());
                }
            }
            surface->materials[materialIndex] = material;
            surface->materialStates[materialIndex] = intent;
            surface->input.uvScale = uv;
            surface->input.retainedFragment.heightBlendStrength = heightBlend;
            surface->input.weightSource = weightMode;
            surface->input.retainedFragment.globalColourWeight = parameter("u_globalColourWeight", 0);
            surface->input.retainedFragment.globalColourWeightNear = parameter("u_globalColourWeightNear", 0);
            surface->input.retainedFragment.globalAoStrength = parameter("u_globalAoStrength", 0);
            surface->input.retainedFragment.globalWetnessStrength = parameter("u_globalWetnessStrength", 0);
            if (heightChanged)
                surface->inputPending = true;
            if (changed)
            {
                surface->inputPending = true;
                std::lock_guard<std::mutex> lock(mutex);
                auto found = updates.find(surface->desc.surfaceId);
                const auto plan = found == updates.end() ? surface->plan : found->second.plan;
                updates[surface->desc.surfaceId] = {plan, {0, 0, 1, 1}, ++nextEditRevision, 0, true};
            }
            if (changed)
                materialCandidates[surface->desc.surfaceId] = std::move(surface);
            else
            {
                MaterialInstanceStateBatch::Change values{material, intent->parameters, {}};
                MaterialInstanceStateBatch transaction({std::move(values)});
                if (!transaction.TryLockNoThrow())
                    throw std::runtime_error("dynamic material changed during safe-point preparation");
                transaction.CommitNoThrow();
                registered->materialStates[materialIndex] = intent;
            }
            materialIntents.erase(event.first);
            materialFailures.erase(event.first);
            }
            catch (const std::exception& error)
            {
                auto& failures = materialFailures[event.first];
                std::cerr << "[TerrainVT] material update retained previous state: " << event.first
                    << " attempt=" << ++failures << "/3 " << error.what() << std::endl;
                if (failures < 3)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    materialUpdates.emplace(event.first, event.second);
                }
            }
        }
}

void TerrainCompositedService::Impl::CandidateImageBarrier(vk::CommandBuffer command, const Texture& texture,
    vk::ImageLayout before, vk::ImageLayout after,
    vk::AccessFlags source, vk::AccessFlags destination)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(texture.getImage()).setOldLayout(before).setNewLayout(after)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(source).setDstAccessMask(destination)
        .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, texture.GetArrayLayers()});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
}
} // namespace VL
