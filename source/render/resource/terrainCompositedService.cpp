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

namespace VL
{
struct TerrainCompositedFrozenConfig
{
    TerrainCompositedRuntimeBudget runtime;
    TerrainRvtSchedulerConfig production;
    TerrainCompositedProductionAllocationQuery allocations;
    uint32_t packagedMinMip = 0;
    TerrainRvtBudgetCaps steadyCaps;
    TerrainRvtBudgetCaps peakCaps;
    TerrainRvtBudgetCaps terrainSteadyCaps;
    std::shared_ptr<TerrainRvtBudgetOwner> terrainOwner;
    uint64_t geometryGpuBytes = 0;
    uint64_t geometryCpuBytes = 0;
    uint64_t metadataGpuBytes = 0;
    double initialQualityLimitMs = 0;
    double poseQualityLimitMs = 0;
};

namespace
{
using Baker = TerrainCompositedBakeGpu;
std::atomic<bool> GlobalCookMode{false};
std::atomic<bool> GlobalAuthoringMode{false};
std::atomic<uint64_t> NextGeneration{0};
std::atomic<bool> GlobalWorldHasBeenActivated{false};
std::mutex FrozenConfigMutex;
std::optional<TerrainCompositedFrozenConfig> FrozenConfig;
std::mutex EvidenceBudgetMutex;
std::optional<TerrainRvtBudgetLease> EvidenceProcessLease;
std::optional<TerrainRvtBudgetLease> FileTraceProcessLease;
constexpr uint64_t ReadOnlyPackageLimit = 4ull * 1024 * 1024 * 1024;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error("Terrain composited service: " + message);
}

void ServiceEvidence(const char* eventClass, const char* prefix, const nlohmann::json& payload)
{
    std::cout << prefix << payload.dump() << std::endl;
    RuntimeEvidence::Record(eventClass, payload);
}

double QualityMonotonicMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct TerrainServicePublishedSurfaceView
{
    std::string surfaceId;
    uint64_t revision = 0;
    uint64_t acknowledgedRevision = 0;
    std::string inputIdentity;
};

struct TerrainServicePublishedObservation
{
    nlohmann::json summary;
    std::vector<nlohmann::json> pages;
};

std::optional<TerrainServicePublishedObservation> BuildTerrainServicePublishedObservation(
    const TerrainCompositedRuntime& runtime,
    const std::vector<TerrainServicePublishedSurfaceView>& views,
    uint64_t world, uint64_t revision, uint64_t descriptorRevision,
    uint64_t descriptorWorld, uint64_t completedEpoch, uint64_t inputEpoch,
    uint64_t statusEpoch, double sampledMs)
{
    if (!revision || !descriptorRevision || descriptorWorld != world || views.empty() ||
        views.size() > 256 || inputEpoch > completedEpoch || statusEpoch > completedEpoch)
        return std::nullopt;
    TerrainServicePublishedObservation result;
    nlohmann::json surfaces = nlohmann::json::array();
    uint64_t currentRoots = 0;
    bool allRequirementsCurrent = true;
    for (const auto& view : views)
    {
        if (view.revision != revision || view.acknowledgedRevision != revision)
            return std::nullopt;
        const auto current = runtime.CandidateDiagnostics(view.surfaceId, completedEpoch);
        if (!current || !current->current.present || current->current.pages.retired ||
            current->current.pages.revision != revision || !current->current.initialized ||
            current->current.tablePublishEpoch > completedEpoch)
            return std::nullopt;
        const auto token = runtime.Token({view.surfaceId, 0, 0, 0});
        if (token.revision != revision ||
            token.worldGeneration != current->current.pages.worldGeneration)
            return std::nullopt;
        const auto& progress = current->current;
        currentRoots += progress.pages.rootCurrent;
        const bool requiredCurrent = progress.pages.rootCurrent &&
            progress.pages.currentPages == progress.pages.requiredPages;
        allRequirementsCurrent = allRequirementsCurrent && requiredCurrent;
        surfaces.push_back({
            {"surfaceId", view.surfaceId}, {"namespaceKind", "active"},
            {"activeNamespace", token.key.surfaceId}, {"revision", token.revision},
            {"tokenWorldGeneration", token.worldGeneration},
            {"currentContentHash", token.contentHash.ToHex()}, {"inputIdentity", view.inputIdentity},
            {"requiredPages", progress.pages.requiredPages}, {"currentPages", progress.pages.currentPages},
            {"queued", progress.pages.queued}, {"reading", progress.pages.reading},
            {"rootCurrent", progress.pages.rootCurrent},
            {"requiredPagesCurrent", requiredCurrent},
            {"tablePublicationEpoch", progress.tablePublishEpoch},
            {"asOfCompletedEpoch", completedEpoch}});
    }
    const auto owners = runtime.RecordedOwners();
    if (owners.size() > 512)
        return std::nullopt;
    for (const auto& owner : owners)
    {
        if (!owner.valid)
            continue;
        const auto view = std::find_if(views.begin(), views.end(),
            [&](const auto& value) { return value.surfaceId == owner.token.key.surfaceId; });
        if (view == views.end())
            return std::nullopt;
        const auto slot = runtime.SlotDiagnostics(owner.slot, completedEpoch);
        if (!slot)
            return std::nullopt;
        result.pages.push_back({
            {"event", "world-active-page-owner"}, {"phase", "active"},
            {"namespaceKind", "active"}, {"worldGeneration", world}, {"jointRevision", revision},
            {"surfaceId", view->surfaceId}, {"slot", owner.slot},
            {"mip", owner.token.key.mip}, {"x", owner.token.key.x}, {"y", owner.token.key.y},
            {"tokenRevision", owner.token.revision}, {"tokenWorldGeneration", owner.token.worldGeneration},
            {"tokenContentHash", owner.token.contentHash.ToHex()},
            {"publishedNamespace", slot->publishedToken.key.surfaceId},
            {"publishedRevision", slot->publishedToken.revision},
            {"publishEpoch", slot->publishEpoch}, {"completedEpoch", completedEpoch},
            {"reservation", slot->reservation}, {"slotState", static_cast<uint32_t>(slot->state)}});
    }
    result.summary = {
        {"event", "world-active-publication"}, {"phase", "active"}, {"namespaceKind", "active"},
        {"jointSwitch", "committed"}, {"published", true}, {"worldGeneration", world},
        {"revision", revision}, {"descriptorRevision", descriptorRevision},
        {"descriptorWorldGeneration", descriptorWorld}, {"asOfCompletedEpoch", completedEpoch},
        {"inputEpoch", inputEpoch}, {"statusLastUseEpoch", statusEpoch},
        {"sampleMonotonicMs", sampledMs}, {"renderFrame", nullptr},
        {"observationStage", "post-joint-commit-before-record"},
        {"currentRootCount", currentRoots}, {"surfaceCount", views.size()},
        {"allCurrentRequirementsSatisfied", allRequirementsCurrent},
        {"pageOwnerCount", result.pages.size()}, {"surfaces", std::move(surfaces)}};
    return result;
}

nlohmann::json TerrainServiceCostJson(const TerrainRvtCost& cost)
{
    return {{"gpuBytes", cost.gpuBytes}, {"cpuBytes", cost.cpuBytes},
        {"physicalSlots", cost.physicalSlots}, {"productionTasks", cost.productionTasks}};
}

nlohmann::json BuildTerrainServiceOuterLedgerObservation(
    const TerrainRvtBudgetOwner& outer, uint64_t world, uint64_t completedEpoch, double sampledMs)
{
    const auto stats = outer.Statistics();
    const auto caps = outer.Caps();
    const auto identity = reinterpret_cast<std::uintptr_t>(outer.IdentityToken().get());
    nlohmann::json byClass = nlohmann::json::object();
    nlohmann::json byPhase = nlohmann::json::object();
    const char* classes[] = {"physicalPool", "input", "workspace"};
    const char* phases[] = {"candidate", "active", "retiring"};
    for (size_t index = 0; index < 3; ++index)
    {
        byClass[classes[index]] = TerrainServiceCostJson(stats.byClass[index]);
        byPhase[phases[index]] = TerrainServiceCostJson(stats.byPhase[index]);
    }
    return {
        {"event", "renderer-terrain-outer-ledger"}, {"worldGeneration", world},
        {"completedEpoch", completedEpoch}, {"sampleMonotonicMs", sampledMs},
        {"scope", "simultaneous-all-worlds-terrain-owner"}, {"ownerIdentity", identity},
        {"ownerIdentityScope", "process-local-live-budget-state"},
        {"current", TerrainServiceCostJson(stats.current)}, {"peak", TerrainServiceCostJson(stats.peak)},
        {"currentByClass", std::move(byClass)}, {"currentByPhase", std::move(byPhase)},
        {"peakByClass", nullptr}, {"peakByPhase", nullptr},
        {"historicalClassPhasePeaksAvailable", false},
        {"peakComponentsMayHaveDifferentSampleTimes", true},
        {"metadataBytes", stats.metadataBytes}, {"peakMetadataBytes", stats.peakMetadataBytes},
        {"metadataAlreadyIncludedInCpuTotals", true},
        {"allocations", stats.allocations}, {"rejected", stats.rejected},
        {"actualCaps", {{"totalGpuBytes", caps.totalGpuBytes}, {"totalCpuBytes", caps.totalCpuBytes},
            {"physicalSlots", caps.physicalSlots}, {"productionTasks", caps.productionTasks}}}};
}


const double ModuleStartedMonotonicMs = QualityMonotonicMs();

double ProcessStartedMonotonicMs()
{
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{}, current{};
    Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0,
        "cannot query process creation time for initial quality deadline");
    GetSystemTimePreciseAsFileTime(&current);
    const uint64_t creation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const uint64_t now = (uint64_t(current.dwHighDateTime) << 32) | current.dwLowDateTime;
    Require(creation && now >= creation, "invalid process creation timestamp");
    return QualityMonotonicMs() - double(now - creation) / 10000.0;
#else
    return ModuleStartedMonotonicMs;
#endif
}

float Parameter(const MaterialInstance& material, const char* name, float fallback)
{
    return material.HasParameter(name) ? material.GetParameter<float>(name) : fallback;
}

std::shared_ptr<const MaterialInstanceStateSnapshot> AuthorMaterialState(
    const MaterialInstanceStateSnapshot& snapshot, const std::shared_ptr<Texture>& runtimeLocalWeights,
    const std::shared_ptr<Texture>& authorWeights = {},
    const std::optional<std::string>& authoredWeightIdentity = std::nullopt)
{
    auto textures = snapshot.textures;
    for (const char* binding : {"compositedPageTable", "compositedAlbedo", "compositedNormal",
        "compositedOrm", "terrainHeightAtlas", "terrainHeightLayerMap", "terrainLayerSourceMap",
        "layerTable", "pageTable", "layerIds", "layerWeights"})
        textures.erase(binding);
    const auto localWeights = textures.find("layerWeightMaps");
    if (localWeights != textures.end() && runtimeLocalWeights &&
        localWeights->second.texture == runtimeLocalWeights)
    {
        const auto identity = authoredWeightIdentity ? authoredWeightIdentity :
            localWeights->second.textureAssetIdentity;
        textures.erase(localWeights);
        if (authorWeights && identity)
            textures.emplace("layerWeightMaps", MaterialInstanceTextureBindingSnapshot(
                authorWeights, identity, std::nullopt));
    }
    return std::make_shared<const MaterialInstanceStateSnapshot>(
        snapshot.normalizedMaterialInstanceIdentity, snapshot.parameters, std::move(textures));
}

std::optional<std::string> AuthoredWeightIdentity(const std::string& materialPath)
{
    if (materialPath.empty())
        return std::nullopt;
    RuntimeFileTrace::TracingIfstream file(CommonFunction::Path(materialPath), std::ios::binary);
    if (!file)
        return std::nullopt;
    nlohmann::json source;
    file >> source;
    const auto textures = source.find("textures");
    if (textures == source.end() || !textures->is_object())
        return std::nullopt;
    const auto binding = textures->find("layerWeightMaps");
    if (binding == textures->end() || !binding->is_string() || binding->get<std::string>().empty())
        return std::nullopt;
    return MaterialAssetUtils::NormalizeAssetPath(binding->get<std::string>());
}

TerrainCompositedDependencyRoots Roots()
{
    return {CommonFunction::GetResourcePath(), CommonFunction::GetProjectPath()};
}

std::filesystem::path ManifestPath(const TerrainBuildPlan& plan)
{
    const auto roots = Roots();
    const std::string name = plan.desc.compositedVT.packagePath;
    Require(!name.empty() && name.find(':') == std::string::npos &&
        name.find('\0') == std::string::npos, "missing/invalid packagePath; Cook requires an explicit output manifest");
    std::filesystem::path relative(name);
    Require(!relative.is_absolute() && !relative.has_root_name(), "packagePath must be resource relative");
    for (const auto& part : relative)
        Require(part != "..", "packagePath traversal");
    const auto root = std::filesystem::canonical(roots.resourceRoot);
    const auto target = std::filesystem::weakly_canonical(root / relative);
    const auto local = target.lexically_relative(root);
    Require(!local.empty() && !local.is_absolute(), "packagePath escapes resource root");
    for (const auto& part : local)
        Require(part != "..", "canonical packagePath escapes resource root");
    return target;
}

HostImage Image(uint32_t width, uint32_t height, bool floating, std::vector<uint8_t> bytes)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.format = floating ? HostImage::PixelFormat::RGBA32_FLOAT : HostImage::PixelFormat::RGBA8_UNORM;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = width * (floating ? 16 : 4);
    image.data = std::move(bytes);
    return image;
}

void PublishSampledImage(
    vk::CommandBuffer command, const Texture& texture,
    vk::ImageLayout previousLayout = vk::ImageLayout::eShaderReadOnlyOptimal)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(texture.getImage()).setOldLayout(previousLayout)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
        .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0,
            texture.getMipLevels(), 0, texture.GetArrayLayers()});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader,
        {}, {}, {}, barrier);
}

std::shared_ptr<Texture> Upload(RendererBackendVulkan& backend, const HostImage& image,
    const std::string& name, vk::Filter filter = vk::Filter::eNearest, bool array = false)
{
    Texture::CreateDesc desc;
    desc.semantic = HostImage::TextureSemantic::Data;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.flipY = false;
    desc.filter = filter;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = name;
    desc.textureArray = array;
    auto texture = std::make_shared<Texture>(backend, image, desc);
    auto command = backend.BeginSingleTimeCommands();
    try
    {
        PublishSampledImage(command, *texture);
        backend.EndSingleTimeCommands(command);
    }
    catch (...)
    {
        if (command)
            backend.AbortSingleTimeCommands(command);
        throw;
    }
    return texture;
}

Baker::SampledTexture Sample(RendererBackendVulkan& backend, const std::shared_ptr<Texture>& texture)
{
    Require(bool(texture), "missing source texture");
    const auto extent = backend.GetImageExtent(texture->getImage());
    Baker::SampledTexture result;
    result.descriptor = texture->GetDescriptorInfo();
    result.width = extent.width;
    result.height = extent.height;
    result.layers = texture->GetArrayLayers();
    result.mipLevels = texture->getMipLevels();
    const auto format = texture->GetFormat();
    result.encoding = format == vk::Format::eR8G8B8A8Srgb ||
        format == vk::Format::eBc7SrgbBlock ? Baker::Encoding::SrgbView : Baker::Encoding::Linear;
    Require(result.width && result.height, "unknown source image dimensions");
    return result;
}

std::array<float, 16> Placement(const Eigen::Matrix4f& model)
{
    std::array<float, 16> matrix;
    std::copy_n(model.data(), matrix.size(), matrix.begin());
    return matrix;
}

nlohmann::json BuildConfig(const MaterialInstance& material)
{
    nlohmann::json numeric = nlohmann::json::object();
    for (const auto& entry : material.CaptureNumericParameterValues())
    {
        if (entry.first != "u_uvScale" && entry.first != "u_heightBlendStrength" &&
            entry.first != "u_terrainWeightMapMode")
            continue;
        std::visit([&](const auto& value)
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, float>)
                numeric[entry.first] = value;
            else
            {
                auto array = nlohmann::json::array();
                for (Eigen::Index index = 0; index < value.size(); ++index)
                    array.push_back(value[index]);
                numeric[entry.first] = std::move(array);
            }
        }, entry.second);
    }
    return {{"generator", "terrainCompositedBake-v1"}, {"localWeightOverlay", "imported-paint-r32-v1"},
        {"mip0SamplesPerAxis", 1}, {"coarseSamplesPerAxis", 4},
        {"frozenHeightBlendFade", 1.0}, {"retainedFragmentPostprocess", true},
        {"frame", "mesh-lod0-gradient-tangent-1-0-0-positive-w"},
        {"channels", {"rgba8-srgb", "rgba8-unorm", "rgba8-unorm"}}, {"parameters", numeric}};
}

std::pair<HostImage, HostImage> Frames(const TerrainBuildPlan& plan)
{
    Require(bool(plan.heightmap) && plan.desc.gridResolution > 0 &&
        plan.desc.gridResolution <= 4096 && plan.desc.size.x() > 0 && plan.desc.size.y() > 0,
        "invalid macro height/frame dimensions");
    const uint32_t extent = plan.desc.gridResolution + 1;
    std::vector<uint8_t> normals(size_t(extent) * extent * 16);
    std::vector<uint8_t> tangents(normals.size());
    const auto transform = TerrainHeightmap::MeshUvTransformFor(plan.desc, *plan.heightmap);
    const float step = 1.f / plan.desc.gridResolution;
    for (uint32_t pixelY = 0; pixelY < extent; ++pixelY)
        for (uint32_t pixelX = 0; pixelX < extent; ++pixelX)
        {
            const auto uv = transform.Apply(pixelX * step, pixelY * step);
            const auto height = [&](float deltaU, float deltaV)
            {
                return TerrainHeightmap::SampleHeightCm(plan.desc, *plan.heightmap,
                    uv.x() + deltaU * transform.scaleU, uv.y() + deltaV * transform.scaleV);
            };
            const Eigen::Vector3f normal = Eigen::Vector3f(
                -(height(step, 0) - height(-step, 0)) / (2 * step * plan.desc.size.x()), 1,
                -(height(0, step) - height(0, -step)) / (2 * step * plan.desc.size.y())).normalized();
            const std::array<float, 4> packedNormal{normal.x(), normal.y(), normal.z(), 0};
            const std::array<float, 4> packedTangent{1, 0, 0, 1};
            const size_t offset = (size_t(pixelY) * extent + pixelX) * 16;
            std::memcpy(normals.data() + offset, packedNormal.data(), 16);
            std::memcpy(tangents.data() + offset, packedTangent.data(), 16);
        }
    return {Image(extent, extent, true, std::move(normals)),
        Image(extent, extent, true, std::move(tangents))};
}

TerrainCompositedVTUVRect SnapshotDirty(const TerrainBuildPlan& previous, const TerrainBuildPlan& next)
{
    TerrainCompositedVTUVRect dirty{1, 1, 0, 0};
    const auto include = [&](uint32_t pixelX, uint32_t pixelY, uint32_t width, uint32_t height)
    {
        const double padU = 2.0 / std::max(1u, width - 1);
        const double padV = 2.0 / std::max(1u, height - 1);
        const double sampleU = double(pixelX) / std::max(1u, width - 1);
        const double sampleV = double(pixelY) / std::max(1u, height - 1);
        dirty.minU = std::min(dirty.minU, sampleU - padU);
        dirty.minV = std::min(dirty.minV, sampleV - padV);
        dirty.maxU = std::max(dirty.maxU, sampleU + padU);
        dirty.maxV = std::max(dirty.maxV, sampleV + padV);
    };
    if (previous.layerPaint != next.layerPaint)
    {
        const auto& before = previous.layerPaint;
        const auto& after = next.layerPaint;
        if (!before || !after || before->width != after->width || before->height != after->height ||
            before->layerCount != after->layerCount || before->Empty() || after->Empty())
            return {0, 0, 1, 1};
        const size_t pixels = size_t(after->width) * after->height;
        for (size_t index = 0; index < after->weights.size(); ++index)
            if (before->weights[index] != after->weights[index] ||
                before->opacity[index] != after->opacity[index])
            {
                const size_t pixel = index % pixels;
                include(static_cast<uint32_t>(pixel % after->width),
                    static_cast<uint32_t>(pixel / after->width), after->width, after->height);
            }
    }
    if (previous.heightmap != next.heightmap)
    {
        if (!previous.heightmap || !next.heightmap ||
            previous.heightmap->width != next.heightmap->width ||
            previous.heightmap->height != next.heightmap->height)
            return {0, 0, 1, 1};
        const auto& after = *next.heightmap;
        for (uint32_t pixelY = 0; pixelY < after.height; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < after.width; ++pixelX)
                if (TerrainHeightmap::ReadNormalizedPixel(*previous.heightmap, pixelX, pixelY) !=
                    TerrainHeightmap::ReadNormalizedPixel(after, pixelX, pixelY))
                {
                    const auto transform = TerrainHeightmap::MeshUvTransformFor(next.desc, after);
                    const double sampleU = (double(pixelX) / std::max(1u, after.width - 1) -
                        transform.offsetU) / transform.scaleU;
                    const double sampleV = (double(pixelY) / std::max(1u, after.height - 1) -
                        transform.offsetV) / transform.scaleV;
                    const double padding = 3.0 / next.desc.gridResolution;
                    dirty.minU = std::min(dirty.minU, sampleU - padding);
                    dirty.minV = std::min(dirty.minV, sampleV - padding);
                    dirty.maxU = std::max(dirty.maxU, sampleU + padding);
                    dirty.maxV = std::max(dirty.maxV, sampleV + padding);
                }
    }
    if (dirty.minU >= dirty.maxU || dirty.minV >= dirty.maxV)
        return {0, 0, 0, 0};
    return {std::clamp(dirty.minU, 0.0, 1.0), std::clamp(dirty.minV, 0.0, 1.0),
        std::clamp(dirty.maxU, 0.0, 1.0), std::clamp(dirty.maxV, 0.0, 1.0)};
}

TerrainCompositedFrozenConfig ReadFrozenConfig()
{
    std::lock_guard<std::mutex> lock(FrozenConfigMutex);
    Require(FrozenConfig.has_value(), "P0 frozen configuration has not been supplied");
    return *FrozenConfig;
}

TerrainRvtBudgetCaps FrozenCaps(const nlohmann::json& value)
{
    TerrainRvtBudgetCaps caps;
    caps.physicalPoolGpuBytes = value.at("physicalPoolGpuBytes").get<uint64_t>();
    caps.inputGpuBytes = value.at("inputGpuBytes").get<uint64_t>();
    caps.workspaceGpuBytes = value.at("workspaceGpuBytes").get<uint64_t>();
    caps.totalGpuBytes = value.at("totalGpuBytes").get<uint64_t>();
    caps.totalCpuBytes = value.at("totalCpuBytes").get<uint64_t>();
    caps.physicalSlots = value.at("physicalSlots").get<uint64_t>();
    caps.productionTasks = value.at("productionTasks").get<uint64_t>();
    caps.physicalPoolCpuBytes = caps.inputCpuBytes = caps.workspaceCpuBytes = caps.totalCpuBytes;
    Require(caps.totalGpuBytes && caps.totalCpuBytes && caps.physicalSlots &&
        caps.productionTasks, "empty frozen budget");
    return caps;
}

TerrainRvtBudgetCaps TerrainCaps(uint64_t gpu, uint64_t cpu, const TerrainRvtBudgetCaps& rvt)
{
    auto caps = rvt;
    caps.totalGpuBytes = gpu;
    caps.totalCpuBytes = cpu;
    caps.physicalPoolGpuBytes = caps.inputGpuBytes = caps.workspaceGpuBytes = gpu;
    caps.physicalPoolCpuBytes = caps.inputCpuBytes = caps.workspaceCpuBytes = cpu;
    return caps;
}

TerrainCompositedProductionAllocationOverheads FrozenOverheads()
{
    TerrainCompositedProductionAllocationOverheads overheads;
    overheads.imageViewAndSampler = {0, sizeof(vk::DescriptorImageInfo), 0, 0};
    overheads.imageMetadata = {0, sizeof(Texture) + sizeof(TerrainCompositedProductionObject), 0, 0};
    overheads.transferMetadata = {0, sizeof(vk::Buffer) + sizeof(vk::DeviceMemory), 0, 0};
    overheads.queryMetadata = {0, sizeof(vk::ImageCreateInfo) + sizeof(vk::MemoryRequirements), 0, 0};
    return overheads;
}

uint64_t SourceTextureBytes(RendererBackendVulkan& backend, const Texture& texture)
{
    const auto extent = backend.GetImageExtent(texture.getImage());
    uint64_t bytes = 0;
    for (uint32_t mip = 0; mip < texture.getMipLevels(); ++mip)
    {
        const uint32_t width = std::max(1u, extent.width >> mip);
        const uint32_t height = std::max(1u, extent.height >> mip);
        uint64_t texelBytes = 0;
        switch (texture.GetFormat())
        {
        case vk::Format::eR8G8Unorm:
        case vk::Format::eR8G8Srgb:
        case vk::Format::eR16Unorm:
        case vk::Format::eR16Sfloat:
            texelBytes = 2;
            break;
        case vk::Format::eR16G16Sfloat:
        case vk::Format::eR32Sfloat:
            texelBytes = 4;
            break;
        default:
            break;
        }
        const uint64_t mipBytes = texelBytes
            ? uint64_t(width) * height * texelBytes * texture.GetArrayLayers()
            : EstimateTextureBytes(texture.GetFormat(), width, height, texture.GetArrayLayers());
        if (!extent.width || !extent.height || !mipBytes)
            return 0;
        bytes += mipBytes;
    }
    return bytes;
}
}

struct TerrainCompositedService::Impl : TerrainFaultOwnerRegistration
{
    struct Buffer
    {
        RendererBackendVulkan* backend = nullptr;
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
        uint64_t bytes = 0;
        ~Buffer()
        {
            if (mapped)
                backend->UnmapMemory(memory);
            if (buffer)
                backend->DestroyBuffer(buffer, memory);
        }
    };
    struct InputAllocation
    {
        TerrainRvtBudgetLease lease;
        std::shared_ptr<Texture> texture;
    };
    struct CpuAllocation
    {
        TerrainRvtBudgetLease lease;
        HostImage image;
    };
    struct PendingImage
    {
        std::shared_ptr<Texture> texture;
        std::shared_ptr<Buffer> staging;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 1;
    };
    struct ProductionTask
    {
        struct StaleHold
        {
            std::optional<TerrainRvtBudgetLease> metadata;
            TerrainFaultRequest request;
        };
        std::unique_ptr<StaleHold> staleHold;
        std::optional<TerrainRvtBudgetLease> lease;
        struct Timing
        {
            RendererBackendVulkan* backend = nullptr;
            vk::QueryPool pool;
            ~Timing()
            {
                if (pool)
                    backend->DestroyQueryPool(pool);
            }
        };
        std::shared_ptr<Timing> timing;
        std::optional<TerrainRvtBudgetLease> executionLease;
        std::shared_ptr<Buffer> output;
        std::shared_ptr<Buffer> scratch;
        std::shared_ptr<Buffer> status;
        std::optional<TerrainCompositedPageUpload> page;
        uint64_t epoch = 0;
        bool recorded = false;
        std::vector<std::shared_ptr<Texture>> retained;
        std::shared_ptr<const TerrainCompositedProductionGpuResources> owner;
        std::shared_ptr<const Baker::AdjacencySnapshot> snapshot;
        std::shared_ptr<void> inputOwner;
        uint64_t candidateRevision = 0;
        Baker::ProducerSnapshot producer;
    };
    struct HeightCopy
    {
        std::shared_ptr<Texture> source;
        std::shared_ptr<Texture> destination;
        std::vector<vk::ImageCopy> regions;
        bool clear = true;
    };
    struct Surface
    {
        TerrainBuildPlan plan;
        Eigen::Matrix4f model;
        TerrainCompositedVTBuildDesc desc;
        nlohmann::json authorManifest;
        Baker::InputSnapshot input;
        std::vector<std::shared_ptr<Texture>> retained;
        std::vector<std::shared_ptr<MaterialInstance>> materials;
        std::vector<std::shared_ptr<const MaterialInstanceStateSnapshot>> materialStates;
        TerrainHeightAtlasResources height;
        uint64_t revision = 0;
        uint64_t sourceRevision = 0;
        uint64_t inputBytes = 0;
        uint64_t generation = 0;
        bool inputPending = false;
        std::array<std::shared_ptr<Texture>, 3> coverage;
        std::array<std::shared_ptr<Texture>, 4> sources;
        std::shared_ptr<Texture> sourceMap;
        std::shared_ptr<Texture> localWeights;
        std::shared_ptr<Texture> authorWeights;
        std::vector<std::optional<std::string>> authoredWeightIdentities;
        std::shared_ptr<const HostImage> importedWeights;
        std::vector<PendingImage> pendingImages;
        std::vector<HeightCopy> pendingHeight;
        std::vector<std::shared_ptr<Texture>> previousImages;
        uint32_t previewFailures = 0;
    };
    struct Update
    {
        TerrainBuildPlan plan;
        TerrainCompositedVTUVRect dirty;
        uint64_t revision = 0;
        uint64_t sourceRevision = 0;
        bool force = false;
        uint32_t attempts = 0;
    };
    struct InputPatch
    {
        std::shared_ptr<Texture> destination;
        uint32_t x = 0, y = 0, layer = 0, width = 0, height = 0;
        std::vector<uint8_t> bytes;
    };
    struct WorldLeafOwner
    {
        std::optional<TerrainRvtBudgetLease> metadata;
        std::map<const void*, TerrainRvtBudgetLease> exclusiveLeases;
        std::vector<std::shared_ptr<Texture>> textures;
        std::vector<std::shared_ptr<const HostImage>> cpuImages;
        std::map<const Texture*, std::shared_ptr<TerrainCompositedProductionGpuResources::PhaseUse>> uses;
    };
    struct WorldCandidate
    {
        WorldCandidate(uint64_t world, uint64_t revision, std::vector<std::string> ids)
            : transaction(world, revision, std::move(ids)) {}
        TerrainCompositedResourceSnapshot transaction;
        std::shared_ptr<TerrainRvtBudgetOwner> budget;
        std::vector<std::unique_ptr<Surface>> surfaces;
        std::array<std::shared_ptr<Texture>, 3> coverage;
        std::vector<TerrainRvtBudgetLease> cpuLeases;
        std::vector<InputPatch> patches;
        std::vector<HeightCopy> heightCopies;
        std::optional<TerrainRvtBudgetLease> stagingLease;
        std::shared_ptr<Buffer> staging;
        std::set<const Texture*> initialized;
        size_t nextPatch = 0;
        size_t recordedEnd = 0;
        uint64_t uploadEpoch = 0;
        bool cloned = false;
        bool recordedClone = false;
        bool inputsRecorded = false;
        bool runtimeStarted = false;
        std::vector<const Texture*> recordedInitialized;
        std::map<std::string, Update> changes;
        std::shared_ptr<WorldLeafOwner> inputs;
        Baker::ProducerSnapshot producer;
        double lastTelemetryMs = 0;
        TerrainCompositedSnapshotPhase lastTelemetryPhase = TerrainCompositedSnapshotPhase::Cancelled;
        size_t requiredPageCount = 0;
        uint64_t completedProductionCount = 0;
        const char* switchBlocker = "inputs";
    };
    RendererBackendVulkan& backend;
    PipelineFactory& pipelineFactory;
    TerrainStreamingState& streaming;
    uint64_t generation = ++NextGeneration;
    uint64_t worldGeneration = 0;
    ShaderReloadCoordinator* shaderReloadCoordinator = nullptr;
    Baker::ProducerSnapshot currentProducer;
    std::atomic<bool> interactiveReady{true};
    std::atomic<bool> qualityConvergenceFailed{false};
    uint64_t qualityPose = 0;
    uint64_t lastQualityEpoch = 0;
    double qualityPoseStartedMs = 0;
    TerrainCompositedQualityClock qualityClock;
    std::unique_ptr<Baker> baker;
    std::unique_ptr<TerrainCompositedRuntime> runtime;
    std::unique_ptr<TerrainCompositedServiceRequestDiagnostics> requestDiagnostics;
    bool requestDiagnosticsConfigured = false;
    std::optional<TerrainCompositedFrozenConfig> frozen;
    std::shared_ptr<TerrainCompositedVTStore> store;
    std::shared_ptr<TerrainCompositedProductionGpuResources> productionOwner;
    TerrainCompositedProductionGpuResources::WorldScope worldScope = std::make_shared<const uint64_t>(generation);
    std::shared_ptr<TerrainRvtBudgetOwner> peakOwner;
    std::shared_ptr<TerrainRvtBudgetOwner> allocationOverride;
    std::shared_ptr<WorldCandidate> worldCandidate;
    std::shared_ptr<WorldLeafOwner> currentInputs;
    std::vector<std::pair<uint64_t, std::shared_ptr<WorldLeafOwner>>> retiredInputs;
    uint64_t steadyWorkspaceQuote = 0;
    std::vector<std::pair<uint64_t, std::shared_ptr<WorldCandidate>>> retiredCandidates;
    std::vector<std::pair<uint64_t, std::vector<std::unique_ptr<Surface>>>> retiredSurfaces;
    std::map<const Texture*, TerrainRvtCost> inputAllocationCosts;
    std::map<const Texture*, std::weak_ptr<Texture>> inputAllocations;
    std::map<const Texture*, std::weak_ptr<InputAllocation>> exclusiveInputOwners;
    std::map<const HostImage*, std::weak_ptr<CpuAllocation>> cpuInputOwners;
    std::filesystem::path worldManifest;
    nlohmann::json authorRecipe;
    TerrainCompositedAdjacencyGraph adjacency;
    std::set<std::string> expectedSurfaces;
    bool finalized = false;
    uint64_t WorldGpuLimit = 0;
    uint64_t WorldCpuLimit = 0;
    std::vector<TerrainRvtBudgetLease> inputLeases;
    std::shared_ptr<const TerrainRvtBudgetLease> descriptorMetadataReserve;
    std::shared_ptr<TerrainRvtBudgetOwner> geometryOwner;
    std::unique_ptr<TerrainCompositedSourceSnapshots> sourceSnapshots;
    std::map<std::string, TerrainBuildPlan> preparedSourcePlans;
    TerrainRvtResourcePhase resourcePhase = TerrainRvtResourcePhase::Candidate;
    std::vector<TerrainRvtBudgetLease> planLeases;
    std::map<std::string, std::shared_ptr<Texture>> sourceMaps;
    std::map<std::string, TerrainHeightAtlasResources> sharedHeightInputs;
    std::vector<std::unique_ptr<Surface>> surfaces;
    std::vector<float> mapPixels = std::vector<float>(32 * 32 * 4, 0);
    std::vector<uint8_t> coveragePageMap;
    std::shared_ptr<Texture> surfaceMap;
    bool mapDirty = false;
    bool recording = false;
    uint64_t gpuBytes = 0;
    uint64_t cpuBytes = 0;
    uint64_t importedWeightBytes = 0;
    std::vector<ProductionTask> productionTasks;
    std::optional<TerrainRvtBudgetLease> bakerLease;
    std::unique_ptr<TerrainCompositedProductionPriority> productionPriority;
    std::vector<std::pair<uint64_t, std::vector<std::shared_ptr<Texture>>>> retired;
    std::vector<std::pair<uint64_t, std::shared_ptr<Buffer>>> stagingRetired;
    uint64_t lastCommittedEpoch = 0;
    std::atomic<bool> budgetSnapshotRequested{false};
    uint64_t nextEditRevision = 0;
    uint64_t staleBakeResults = 0;
    uint64_t abortedProductionTasks = 0;
    uint64_t successfulFrameCommits = 0;
    uint64_t cpuWorkspacePeak = 0;
    uint64_t gpuTransientPeak = 0;
    size_t nextUploadSurface = 0;
    mutable std::mutex mutex;
    std::map<std::string, Update> updates;
    struct Refresh
    {
        TerrainCompositedRefreshStatus status;
        TerrainCompositedRefreshCommand command;
        uint64_t revision = 0;
        std::set<std::string> affected;
        bool dynamicCommitted = false;
    };
    uint64_t nextTicket = 0;
    std::map<uint64_t, Refresh> refreshes;
    std::map<std::string, uint64_t> publishedRevisions;
    std::map<std::string, TerrainCompositedAuthorBaseline> authorBaselines;
    std::shared_ptr<const TerrainCompositedVTStore> authorVerificationStore;
    uint64_t authorVerificationWorld = 0;
    ContentDigest authorVerificationIdentity;
    std::shared_ptr<TerrainCompositedVerifiedInputs> scopedAuthorBaselineSession;
    std::map<std::string, std::shared_ptr<MaterialInstance>> materialUpdates;
    std::map<std::string, std::shared_ptr<const MaterialInstanceStateSnapshot>> materialIntents;
    std::map<std::string, std::unique_ptr<Surface>> materialCandidates;
    std::map<std::string, uint32_t> materialFailures;
    std::string stats = "TerrainVT: no registered surfaces";

    Surface Candidate(const Surface& surface)
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

    void BindPlanSources(const TerrainBuildPlan& plan, const TerrainBuildPlan* previous = nullptr)
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

    void Requeue(const std::string& identity, Update update)
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

    Impl(RendererBackendVulkan& renderer, PipelineFactory& factory, TerrainStreamingState& state)
        : backend(renderer), pipelineFactory(factory), streaming(state)
    {
    }

    ~Impl()
    {
        ResetFaultHooks();
        productionTasks.clear();
        baker.reset();
        bakerLease.reset();
    }

    void ResetFaultHooks()
    {
        Deactivate(TerrainFaultInjection());
    }

    void RegisterActiveFaultHooks()
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

    void CaptureRequestDiagnostics(std::optional<uint64_t> renderFrame, uint64_t completedEpoch)
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

    void InitializeProduction()
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

    void ValidateProductionGenerator() const
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

    void PublishMap()
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

    void Bind(Surface& surface, const TerrainCompositedMaterialBinding* replacement = nullptr)
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

    MaterialInstanceStateBatch::Change MaterialChange(const Surface& surface,
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

    std::shared_ptr<Buffer> HostBuffer(uint64_t bytes, vk::BufferUsageFlags usage, const char* name)
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

    std::shared_ptr<Texture> AllocateTexture(
        uint32_t width, uint32_t height, vk::Format format, vk::Filter filter, bool array,
        uint32_t layers = 1)
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

    std::shared_ptr<Texture> DeferredImage(
        Surface& surface, const HostImage& image, vk::Filter filter, bool array = false)
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

    void CaptureImportedWeights(Surface& surface, const MaterialInstance& material,
        const MaterialInstanceStateSnapshot* replacement = nullptr)
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

    void RefreshLocalWeights(Surface& surface, const TerrainBuildPlan& plan, bool deferred)
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

    void RecordImages(vk::CommandBuffer command, Surface& surface)
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

    void RefreshHeight(Surface& surface)
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

    void AddHeightCandidate(WorldCandidate& candidate, Surface& surface)
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

    void ConsumeMaterials()
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

    void ReserveCandidateCpu(WorldCandidate& candidate, uint64_t bytes)
    {
        auto lease = candidate.budget->TryReserve({0, bytes, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(lease.has_value(), "candidate CPU patches exceed frozen peak admission");
        candidate.cpuLeases.push_back(std::move(*lease));
    }

    void AddCoveragePatches(WorldCandidate& candidate, const Surface& surface,
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

    std::shared_ptr<Texture> AddCpuImagePatches(WorldCandidate& candidate, const HostImage& image,
        vk::Format format, vk::Filter filter, bool array = false)
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

    void AddFramePatches(WorldCandidate& candidate, Surface& surface)
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

    std::shared_ptr<Texture> RetainExternalInput(const std::shared_ptr<Texture>& texture)
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

    void AddLocalWeightPatches(WorldCandidate& candidate, Surface& surface)
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

    void CheckCandidateSteadyInputs(WorldCandidate& candidate)
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

    void ConsumeCandidateBudgetAdmission(
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

    void QueuePendingProducer()
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

    void ConsumeUpdates()
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

    void CandidateImageBarrier(vk::CommandBuffer command, const Texture& texture,
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

    uint64_t RecordWorldInputs(vk::CommandBuffer command, uint64_t completedEpoch)
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

    void DiscardWorldCandidate(const std::string& diagnostic, bool keepPendingProducer = false)
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

    void EmitCandidateTelemetry(WorldCandidate& candidate, uint64_t completedEpoch)
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

    void AdvanceWorldCandidate(uint64_t completedEpoch)
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

    void JointSwitchWorldCandidate(uint64_t completedEpoch)
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

    void EmitBudgetObservation(uint64_t completedEpoch)
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

    bool PrepareDynamicRefresh(Refresh& refresh)
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

    void PrepareRefreshes()
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

    void UpdateRefreshes(bool submitted)
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

    void CompleteProduction(uint64_t completedEpoch, bool loadingFenceComplete = false)
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

    void RecordProduction(vk::CommandBuffer command, uint64_t completedEpoch)
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

    void Capture(Surface& surface, const std::shared_ptr<MaterialInstance>& material)
    {
        auto& input = surface.input;
        const auto& plan = surface.plan;
        const auto& layerMap = streaming.pageManager.PlacementLayerIdMap(TerrainPlacementIdentity(plan));
        Require(layerMap.size() == plan.desc.layers.size() && !layerMap.empty(),
            "missing placement global layer mapping");
        std::set<uint32_t> sourceSets;
        for (size_t local = 0; local < layerMap.size(); ++local)
        {
            const auto global = layerMap[local];
            Require(global < streaming.layerDefinitions.size(), "global layer mapping out of range");
            sourceSets.insert(streaming.layerDefinitions[global].sourceSet);
            input.localToGlobalLayer[local] = global;
            const auto source = streaming.layerSources.find(streaming.layerDefinitions[global].sourceSet);
            uint32_t slice = 0;
            Require(source != streaming.layerSources.end() &&
                source->second.TryGetLayerSlice(global, slice), "missing source slice mapping");
            input.globalToSourceSlice[global] = slice;
        }
        Require(sourceSets.size() == 1, "S1 supports exactly one sourceSet per surface");
        const auto keep = [&](const std::shared_ptr<Texture>& texture)
        {
            surface.retained.push_back(texture);
            return Sample(backend, texture);
        };
        input.layerAlbedo = keep(material->GetTexture("layerAlbedo"));
        input.layerNormal = keep(material->GetTexture("layerNormal"));
        input.layerOrm = keep(material->GetTexture("layerPbr"));
        input.layerHeight = keep(material->GetTexture("layerHeight"));
        surface.sources = {material->GetTexture("layerAlbedo"), material->GetTexture("layerNormal"),
            material->GetTexture("layerPbr"), material->GetTexture("layerHeight")};
        input.layerCatalog = keep(streaming.layerResources.layerTable);
        input.uvScale = Parameter(*material, "u_uvScale", 64);
        input.localToWorld = Placement(surface.model);
        input.localLayerCount = static_cast<uint32_t>(layerMap.size());
        input.retainFragmentPostprocess = true;
        input.frozenHeightBlendFade = 1;
        input.retainedFragment.heightBlendStrength = Parameter(*material, "u_heightBlendStrength", 0);
        if (Parameter(*material, "u_terrainWeightMapMode", 0) > 0.5f)
        {
            input.weightSource = Baker::WeightSource::LocalLayerArray;
            CaptureImportedWeights(surface, *material);
            RefreshLocalWeights(surface, plan, false);
        }
        const float extent = float(plan.desc.gridResolution + 1);
        input.frameUvOffset = {0.5f / extent, 0.5f / extent};
        input.frameUvScale = {(extent - 1) / extent, (extent - 1) / extent};
        auto frames = Frames(plan);
        input.surfaceLocalNormal = keep(Upload(backend, frames.first, "VTBakeLocalNormal", vk::Filter::eLinear));
        input.surfaceLocalTangent = keep(Upload(backend, frames.second, "VTBakeLocalTangent", vk::Filter::eLinear));
        surface.inputBytes = frames.first.data.size() + frames.second.data.size();
        surface.coverage = {streaming.pageResources.pageTable,
            streaming.pageResources.layerIds, streaming.pageResources.layerWeights};
        input.coverageUvOffset = {float(plan.desc.pageOrigin.x()) / 32.f,
            float(plan.desc.pageOrigin.y()) / 32.f};
        input.coverageUvScale = {float(plan.desc.pageTilesPerSide) / 32.f,
            float(plan.desc.pageTilesPerSide) / 32.f};
        input.coveragePageTable = keep(surface.coverage[0]);
        input.coverageIds = keep(surface.coverage[1]);
        input.coverageWeights = keep(surface.coverage[2]);
        if (surface.importedWeights)
            surface.inputBytes += uint64_t(surface.importedWeights->width) *
                surface.importedWeights->height * surface.importedWeights->arrayLayers * sizeof(float);
        std::vector<uint8_t> mapping(256 * 2 * 16, 0);
        for (uint32_t global = 0; global < 256; ++global)
        {
            const auto slice = input.globalToSourceSlice[global];
            const std::array<float, 4> row{slice == Baker::InvalidSlice ? 0.f : float(slice),
                slice == Baker::InvalidSlice ? 0.f : 1.f, 0, 0};
            std::memcpy(mapping.data() + size_t(global) * 16, row.data(), 16);
            const auto globalId = input.localToGlobalLayer[global];
            const std::array<float, 4> localRow{globalId == Baker::InvalidSlice ? 0.f : float(globalId),
                globalId == Baker::InvalidSlice ? 0.f : 1.f, 0, 0};
            std::memcpy(mapping.data() + (256 + size_t(global)) * 16, localRow.data(), 16);
        }
        const auto sourceMap = Upload(backend, Image(256, 2, true, std::move(mapping)), "TerrainLayerSourceMap");
        surface.sourceMap = sourceMap;
        surface.retained.push_back(sourceMap);
        for (const auto& target : surface.materials)
            target->SetTextureKeepingIdentity("terrainLayerSourceMap", sourceMap);
        std::vector<TerrainHeightAtlasLayer> heightLayers;
        for (size_t local = 0; local < layerMap.size(); ++local)
            if (plan.desc.layers[local].hasHeight)
                heightLayers.push_back({layerMap[local], *sourceSets.begin(), input.globalToSourceSlice[layerMap[local]]});
        if (!heightLayers.empty())
        {
            CanonicalFieldHasher key("terrain-world-height-input-owner");
            key.AddUInt64("sourceTexture", reinterpret_cast<uintptr_t>(material->GetTexture("layerHeight").get()));
            for (const auto& layer : heightLayers)
            {
                key.AddUInt32("globalLayer", layer.globalLayer);
                key.AddUInt32("sourceSet", layer.sourceSet);
                key.AddUInt32("sourceSlice", layer.sourceSlice);
            }
            const auto identity = key.Finalize().ToHex();
            const auto existing = sharedHeightInputs.find(identity);
            if (existing == sharedHeightInputs.end())
                surface.height = sharedHeightInputs.emplace(identity, CreateTerrainHeightAtlas(backend,
                    {{*sourceSets.begin(), material->GetTexture("layerHeight")}}, heightLayers,
                    frozen->runtime.owner->Caps().inputGpuBytes)).first->second;
            else
                surface.height = existing->second;
        }
    }

    TerrainCompositedVTPublication CapturePublication()
    {
        TerrainCompositedVTPublication result;
        auto& closure = result.production;
        closure.adjacency = adjacency;
        std::map<const Texture*, std::string> captured;
        std::map<std::string, std::string> capturedImageObjects;
        const auto archive = [&](TerrainCompositedProductionObject object,
            const std::vector<uint8_t>& bytes)
        {
            try
            {
                TerrainCompositedProductionGpu::WriteInputObject(
                    worldManifest.parent_path(), object, bytes);
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("CapturePublication archive [" + object.objectId +
                    " -> " + object.relativePath + "]: " + error.what());
            }
        };
        const auto capture = [&](const std::shared_ptr<Texture>& texture,
            bool repeat, vk::Filter filter) -> std::string
        {
            if (!texture)
                return {};
            const auto existing = captured.find(texture.get());
            if (existing != captured.end())
                return existing->second;
            TerrainCompositedProductionObject object;
            object.objectId = "image-" + std::to_string(captured.size());
            object.relativePath = "inputs/pending.bin";
            const auto extent = backend.GetImageExtent(texture->getImage());
            auto& image = object.image;
            image.width = extent.width;
            image.height = extent.height;
            image.layers = texture->GetArrayLayers();
            image.mipLevels = texture->getMipLevels();
            const std::map<vk::Format, std::string> formats{
                {vk::Format::eR8Unorm, "r8-unorm"}, {vk::Format::eR8G8Unorm, "rg8-unorm"},
                {vk::Format::eR8G8B8A8Unorm, "rgba8-unorm"},
                {vk::Format::eR8G8B8A8Srgb, "rgba8-srgb"}, {vk::Format::eR16Unorm, "r16-unorm"},
                {vk::Format::eR32Sfloat, "r32-float"}, {vk::Format::eR32G32B32A32Sfloat, "rgba32-float"},
                {vk::Format::eBc5UnormBlock, "bc5-unorm"},
                {vk::Format::eBc7UnormBlock, "bc7-unorm"}, {vk::Format::eBc7SrgbBlock, "bc7-srgb"}};
            image.format = formats.at(texture->GetFormat());
            image.transfer = image.format == "rgba8-srgb" || image.format == "bc7-srgb"
                ? TerrainCompositedProductionTransfer::Srgb : TerrainCompositedProductionTransfer::Linear;
            image.address = repeat ? TerrainCompositedProductionAddress::Repeat :
                TerrainCompositedProductionAddress::Clamp;
            image.filter = filter == vk::Filter::eLinear ?
                TerrainCompositedProductionFilter::Linear : TerrainCompositedProductionFilter::Nearest;
            CanonicalFieldHasher metadataHash("terrain-production-image-view");
            metadataHash.AddString("format", image.format);
            metadataHash.AddUInt32("width", image.width);
            metadataHash.AddUInt32("height", image.height);
            metadataHash.AddUInt32("layers", image.layers);
            metadataHash.AddUInt32("mips", image.mipLevels);
            metadataHash.AddUInt32("transfer", static_cast<uint32_t>(image.transfer));
            metadataHash.AddUInt32("address", static_cast<uint32_t>(image.address));
            metadataHash.AddUInt32("filter", static_cast<uint32_t>(image.filter));
            const auto viewIdentity = metadataHash.Finalize().ToHex();
            std::vector<uint8_t> catalog;
            object = TerrainCompositedProductionGpu::CaptureGpuImage(backend, object, texture,
                *frozen->runtime.owner, [&](const auto& metadata, const auto& bytes) {
                    auto addressed = metadata;
                    addressed.relativePath = "inputs/" + addressed.digest.ToHex() + "-" + viewIdentity + ".bin";
                    archive(addressed, bytes);
                    if (texture == streaming.layerResources.layerTable)
                        catalog = bytes;
                }, frozen->allocations);
            object.relativePath = "inputs/" + object.digest.ToHex() + "-" + viewIdentity + ".bin";
            const auto objectIdentity = object.digest.ToHex() + viewIdentity;
            const auto duplicate = capturedImageObjects.find(objectIdentity);
            if (duplicate != capturedImageObjects.end())
            {
                captured.emplace(texture.get(), duplicate->second);
                return duplicate->second;
            }
            if (!catalog.empty())
                PopulateTerrainCompositedCatalogDigests(object, catalog);
            captured.emplace(texture.get(), object.objectId);
            capturedImageObjects.emplace(objectIdentity, object.objectId);
            closure.objects.push_back(object);
            return object.objectId;
        };
        const auto archiveMaterial = [&](const Surface& surface, size_t index)
        {
            const auto& path = index == 0 ? surface.plan.desc.materialInstancePath :
                surface.plan.desc.detailMaterialInstancePath;
            const auto raw = ReadBinaryFile(CommonFunction::Path(path));
            auto document = nlohmann::json::parse(raw.begin(), raw.end());
            const auto resolved = MaterialInstanceResolver::Resolve(path, document);
            document.erase("textures");
            document["parameters"] = nlohmann::json::object();
            for (const auto& parameter : surface.materials[index]->CaptureNumericParameterValues())
                std::visit([&](const auto& value) {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, float>)
                        document["parameters"][parameter.first] = value;
                    else
                    {
                        auto array = nlohmann::json::array();
                        for (Eigen::Index component = 0; component < value.size(); ++component)
                            array.push_back(value[component]);
                        document["parameters"][parameter.first] = std::move(array);
                    }
                }, parameter.second);
            for (auto parameter = document["parameters"].begin(); parameter != document["parameters"].end();)
                if (resolved.materialJson.at("parameters").contains(parameter.key()) &&
                    parameter.value() == resolved.materialJson.at("parameters").at(parameter.key()).at("default"))
                    parameter = document["parameters"].erase(parameter);
                else
                    ++parameter;
            if (document["parameters"].empty())
                document.erase("parameters");
            MaterialInstanceResolver::ResolveWithDefinition(path, document, resolved.materialJson);
            const auto text = document.dump(2);
            const auto hash = ContentHasher::HashBytes(text.data(), text.size()).ToHex();
            const auto relative = "runtime/" + hash + ".json";
            std::filesystem::create_directories(worldManifest.parent_path() / "runtime");
            WriteTextFileAtomically(worldManifest.parent_path() / relative, text);
            return relative;
        };
        for (const auto& registered : surfaces)
        {
            auto& surface = *registered;
            const auto& input = surface.input;
            TerrainCompositedSurfaceProduction production;
            production.surfaceId = surface.desc.surfaceId;
            production.runtimeMaterialInstancePath = archiveMaterial(surface, 0);
            if (surface.materials.size() > 1)
                production.runtimeDetailMaterialInstancePath = archiveMaterial(surface, 1);
            production.weightSource = input.weightSource == Baker::WeightSource::LocalLayerArray
                ? TerrainCompositedWeightSource::LocalLayerArray : TerrainCompositedWeightSource::CoverageAtlas;
            const auto& local = streaming.pageManager.PlacementLayerIdMap(surface.desc.surfaceId);
            production.localToGlobal.assign(local.begin(), local.end());
            production.sourceSet = streaming.layerDefinitions.at(local.front()).sourceSet;
            production.globalToSourceSlice = input.globalToSourceSlice;
            std::copy(input.localToWorld.begin(), input.localToWorld.end(), production.localToWorld.begin());
            const auto pair = [](const std::array<float, 2>& value)
            { return std::array<double, 2>{value[0], value[1]}; };
            auto& sampling = production.sampling;
            sampling.surfaceUvOffset = pair(input.surfaceUvOffset);
            sampling.surfaceUvScale = pair(input.surfaceUvScale);
            sampling.coverageUvOffset = pair(input.coverageUvOffset);
            sampling.coverageUvScale = pair(input.coverageUvScale);
            sampling.frameUvOffset = pair(input.frameUvOffset);
            sampling.frameUvScale = pair(input.frameUvScale);
            production.parameters.uvScale = input.uvScale;
            production.parameters.heightBlendStrength = input.retainedFragment.heightBlendStrength;
            auto& images = production.inputs;
            images.coveragePageTable = capture(surface.coverage[0], false, vk::Filter::eNearest);
            images.coverageIds = capture(surface.coverage[1], false, vk::Filter::eNearest);
            images.coverageWeights = capture(surface.coverage[2], false, vk::Filter::eLinear);
            images.layerCatalog = capture(streaming.layerResources.layerTable, false, vk::Filter::eNearest);
            images.layerAlbedo = capture(surface.sources[0], true, vk::Filter::eLinear);
            images.layerNormal = capture(surface.sources[1], true, vk::Filter::eLinear);
            images.layerOrm = capture(surface.sources[2], true, vk::Filter::eLinear);
            images.layerHeight = capture(surface.sources[3], true, vk::Filter::eLinear);
            images.localLayerWeights = capture(surface.localWeights, false, vk::Filter::eLinear);
            for (const auto& texture : surface.retained)
            {
                if (texture->getImageView() == input.surfaceLocalNormal.descriptor.imageView)
                    images.surfaceLocalNormal = capture(texture, false, vk::Filter::eLinear);
                if (texture->getImageView() == input.surfaceLocalTangent.descriptor.imageView)
                    images.surfaceLocalTangent = capture(texture, false, vk::Filter::eLinear);
            }
            images.heightAtlas = capture(surface.height.heightAtlas, false, vk::Filter::eLinear);
            images.heightLayerMap = capture(surface.height.layerMap, false, vk::Filter::eNearest);
            images.globalColour = capture(surface.materials.front()->GetTexture("globalColour"), false, vk::Filter::eLinear);
            images.globalMasks = capture(surface.materials.front()->GetTexture("globalMasks"), false, vk::Filter::eLinear);
            closure.surfaces.push_back(TerrainCompositedProductionGpu::FreezeSurface(
                surface.desc.surfaceId, production.sourceSet, input, std::move(production.inputs),
                production.runtimeMaterialInstancePath, production.runtimeDetailMaterialInstancePath));
        }
        auto& compiler = pipelineFactory.GetShaderCompiler();
        const auto manifest = compiler.CaptureManifestSnapshot();
        auto& generator = closure.generator;
        generator.version = TerrainCompositedGeneratorVersion;
        generator.evaluator = "M_terrain";
        generator.filterRule = TerrainCompositedFilterRule;
        generator.encoding = TerrainCompositedEncoding;
        generator.compilePolicy = manifest.compilePolicy;
        std::map<std::string, std::string> capturedShaders;
        std::map<std::string, std::string> capturedShaderPaths;
        const auto shader = [&](const std::string& identity, const std::string& digest,
            TerrainCompositedProductionObjectKind kind)
        {
            const auto relative = TerrainCompositedShaderRelativePath(identity,
                kind == TerrainCompositedProductionObjectKind::ShaderSource);
            const auto authorPath = (std::filesystem::path("shader") / relative).generic_string();
            const auto previous = capturedShaders.find(digest);
            if (previous != capturedShaders.end())
            {
                capturedShaderPaths[identity] = previous->second;
                capturedShaderPaths[authorPath] = previous->second;
                return previous->second;
            }
            const auto bytes = ReadBinaryFile(compiler.GetShaderRoot() / relative);
            TerrainCompositedProductionObject object;
            object.objectId = "shader-" + std::to_string(closure.objects.size());
            object.kind = kind;
            object.bytes = bytes.size();
            object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
            Require(object.digest.ToHex() == digest, "generator artifact/source changed during freeze");
            object.relativePath = "inputs/" + object.digest.ToHex() + ".bin";
            archive(object, bytes);
            closure.objects.push_back(object);
            capturedShaders.emplace(digest, object.objectId);
            capturedShaderPaths[identity] = object.objectId;
            capturedShaderPaths[authorPath] = object.objectId;
            return object.objectId;
        };
        const auto include = [&](const std::string& id)
        {
            if (id != generator.computeSource &&
                std::find(generator.includeSources.begin(), generator.includeSources.end(), id) ==
                    generator.includeSources.end())
                generator.includeSources.push_back(id);
        };
        for (const auto& artifact : manifest.artifacts)
        {
            if (artifact.second.normalizedKey.find("terrain/terrainCompositedBakeWorld.comp") == std::string::npos &&
                artifact.second.normalizedKey.find("terrain/terrainCompositedBakeWorldPadding.comp") == std::string::npos)
                continue;
            for (const auto& source : artifact.second.primarySources)
            {
                const auto id = shader(source.identity, source.digest,
                    TerrainCompositedProductionObjectKind::ShaderSource);
                if (generator.computeSource.empty() &&
                    artifact.second.normalizedKey.find("terrain/terrainCompositedBakeWorld.comp") != std::string::npos)
                    generator.computeSource = id;
                else
                    include(id);
            }
            for (const auto& dependency : artifact.second.dependencies)
                include(shader(dependency.path, dependency.digest,
                    TerrainCompositedProductionObjectKind::ShaderSource));
            for (const auto& output : artifact.second.outputs)
            {
                const auto id = shader(output.second.path, output.second.digest,
                    TerrainCompositedProductionObjectKind::ShaderArtifact);
                if (std::find(generator.compiledArtifacts.begin(), generator.compiledArtifacts.end(), id) ==
                    generator.compiledArtifacts.end())
                    generator.compiledArtifacts.push_back(id);
            }
        }
        generator.includeSources.erase(std::remove(generator.includeSources.begin(),
            generator.includeSources.end(), generator.computeSource), generator.includeSources.end());
        for (auto& production : closure.surfaces)
        {
            const auto found = std::find_if(surfaces.begin(), surfaces.end(),
                [&](const auto& surface) { return surface->desc.surfaceId == production.surfaceId; });
            Require(found != surfaces.end() && !(*found)->authorManifest.is_null(),
                "Cook is missing the pre-capture author snapshot");
            ValidateTerrainCompositedAuthors(Roots(), (*found)->authorManifest);
            const auto& author = **found;
            std::map<std::string, std::set<std::string>> provenance;
            const auto add = [&](const std::string& path, const std::string& object)
            {
                if (!path.empty() && !object.empty())
                    provenance[path].insert(object);
            };
            const auto& bindings = production.inputs;
            const std::vector<std::string> derived{
                bindings.coveragePageTable, bindings.coverageIds, bindings.coverageWeights,
                bindings.layerCatalog, bindings.localLayerWeights,
                bindings.surfaceLocalNormal, bindings.surfaceLocalTangent,
                bindings.heightAtlas, bindings.heightLayerMap};
            for (const auto& object : derived)
                add(author.plan.desc.assetPath, object);
            for (const auto& object : {bindings.surfaceLocalNormal, bindings.surfaceLocalTangent,
                bindings.coverageIds, bindings.coverageWeights})
                add(author.plan.desc.heightmapPath, object);
            for (const auto& path : {author.plan.desc.paintPath,
                author.plan.desc.paintPath.empty() ? std::string() : author.plan.desc.paintPath + ".meta.json"})
                for (const auto& object : {bindings.coverageIds, bindings.coverageWeights, bindings.localLayerWeights})
                    add(path, object);
            std::set<std::string> materialObjects;
            for (const auto& material : author.materials)
                for (const auto& binding : material->CaptureStateSnapshot().textures)
                {
                    const auto image = captured.find(binding.second.texture.get());
                    if (image == captured.end() || !binding.second.textureAssetIdentity)
                        continue;
                    const auto& asset = *binding.second.textureAssetIdentity;
                    add(asset, image->second);
                    for (const auto& source : LoadTextureAssetDesc(asset).sources)
                        add(source, image->second);
                    materialObjects.insert(image->second);
                }
            materialObjects.insert(generator.computeSource);
            for (const auto& dependency : author.authorManifest.at("dependencies"))
            {
                const auto path = dependency.at("path").get<std::string>();
                const auto shaderObject = capturedShaderPaths.find(path);
                if (shaderObject != capturedShaderPaths.end())
                    add(path, shaderObject->second);
                else if (path.rfind("shader/glsl/", 0) == 0 && dependency.at("mode") == "bytes")
                {
                    const auto roots = Roots();
                    const auto root = dependency.at("root") == "project" ? roots.projectRoot : roots.resourceRoot;
                    const auto bytes = ReadBinaryFile(root / path);
                    const auto digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
                    Require(digest.ToHex() == dependency.at("digest").get<std::string>(),
                        "author shader changed after the frozen dependency snapshot: " + path);
                    TerrainCompositedProductionObject object;
                    object.objectId = "author-shader-" + std::to_string(closure.objects.size());
                    object.kind = TerrainCompositedProductionObjectKind::ShaderSource;
                    object.bytes = bytes.size();
                    object.digest = digest;
                    object.relativePath = "inputs/" + digest.ToHex() + ".bin";
                    archive(object, bytes);
                    capturedShaderPaths.emplace(path, object.objectId);
                    closure.objects.push_back(object);
                    add(path, object.objectId);
                }
                if (dependency.at("mode") == "terrain-material-instance-static-json-v2" ||
                    dependency.at("mode") == "terrain-material-static-json-v2")
                    provenance[path].insert(materialObjects.begin(), materialObjects.end());
            }
            for (const auto& dependency : (*found)->authorManifest.at("dependencies"))
            {
                const auto path = dependency.at("path").get<std::string>();
                if (!provenance.count(path) || provenance.at(path).empty())
                {
                    const auto roots = Roots();
                    const auto root = dependency.at("root") == "project" ? roots.projectRoot : roots.resourceRoot;
                    const auto mode = dependency.at("mode").get<std::string>();
                    std::vector<uint8_t> bytes;
                    if (mode == "absent")
                    {
                        Require(!std::filesystem::exists(root / path), "optional author appeared during freeze: " + path);
                        const auto text = nlohmann::json{{"kind", "terrain-author-absent"},
                            {"dependency", dependency}}.dump();
                        bytes.assign(text.begin(), text.end());
                    }
                    else
                    {
                        bytes = ReadBinaryFile(root / path);
                        const std::string raw(bytes.begin(), bytes.end());
                        Require(TerrainCompositedDependencyEntryHash(raw, mode, path).ToHex() ==
                            dependency.at("digest").get<std::string>(),
                            "author input changed after frozen dependency capture: " + path);
                    }
                    TerrainCompositedProductionObject object;
                    object.kind = TerrainCompositedProductionObjectKind::RawAuthorSource;
                    object.objectId = "author-raw-" + std::to_string(closure.objects.size());
                    object.bytes = bytes.size();
                    object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
                    object.relativePath = "author-inputs/" + object.digest.ToHex() + ".bin";
                    const auto existing = std::find_if(closure.objects.begin(), closure.objects.end(),
                        [&](const auto& input) { return input.relativePath == object.relativePath; });
                    if (existing == closure.objects.end())
                    {
                        archive(object, bytes);
                        closure.objects.push_back(object);
                        add(path, object.objectId);
                    }
                    else
                        add(path, existing->objectId);
                }
                const auto mapped = provenance.find(path);
                production.authorDependencies.push_back({
                    dependency.at("root").get<std::string>(),
                    path,
                    dependency.at("mode").get<std::string>(),
                    dependency.at("digest").get<std::string>(),
                    std::vector<std::string>(mapped->second.begin(), mapped->second.end())});
            }
        }
        closure.Validate();
        for (auto& surface : surfaces)
        {
            surface->desc.dependencyHash = closure.SurfaceContentHash(surface->desc.surfaceId);
            surface->input.contentIdentity = surface->desc.ContentHash().ToHex();
            result.surfaces.push_back(surface->desc);
            const auto layout = surface->desc.Layout();
            for (uint32_t mip = std::min(frozen->packagedMinMip, layout.MipCount() - 1);
                mip < layout.MipCount(); ++mip)
                for (uint32_t pageY = 0; pageY < layout.Mip(mip).pagesY; ++pageY)
                    for (uint32_t pageX = 0; pageX < layout.Mip(mip).pagesX; ++pageX)
                        result.packagedPages.push_back({surface->desc.surfaceId, mip, pageX, pageY});
        }
        return result;
    }

    TerrainCompositedVTPageData Bake(const Surface& surface, const TerrainCompositedVTPageKey& key)
    {
        Baker::PageRequest request;
        request.virtualWidth = surface.desc.width;
        request.virtualHeight = surface.desc.height;
        request.virtualMip = key.mip;
        request.pageX = key.x;
        request.pageY = key.y;
        request.areaSamplesPerAxis = key.mip == 0 ? 1 : 4;
        Baker::PageBytes result;
        std::string error;
        Baker::AdjacencySnapshot world;
        world.sourceSurfaceId = surface.desc.surfaceId;
        world.graph = adjacency;
        for (const auto& neighbor : surfaces)
        {
            auto input = neighbor->input;
            input.surfaceId = neighbor->desc.surfaceId;
            input.adjacency.reset();
            world.surfaces.push_back({neighbor->desc.surfaceId, std::move(input)});
        }
        const auto outputUsage = vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
        const auto outputBytes = backend.QueryBufferMemoryRequirements(Baker::OutputBytes, outputUsage).size;
        const auto scratchExtent = Baker::RequiredScratchBytes(request.areaSamplesPerAxis);
        const uint64_t scratchBytes = scratchExtent ? backend.QueryBufferMemoryRequirements(
            scratchExtent, vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eIndirectBuffer).size : 0;
        const auto readbackBytes = backend.QueryBufferMemoryRequirements(
            Baker::OutputBytes, vk::BufferUsageFlagBits::eTransferDst).size;
        auto lease = frozen->runtime.owner->TryReserve(
            {outputBytes + scratchBytes + readbackBytes, readbackBytes, 0, 1},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        Require(lease.has_value(), "Cook output/scratch exceeds frozen workspace");
        world.cookWorkspaceLease = std::make_shared<const TerrainRvtBudgetLease>(std::move(*lease));
        Require(baker->BakePageSync(world, request, result, error), "GPU Cook failed: " + error);
        TerrainCompositedVTPageData page{key, {}};
        page.bytes.reserve(surface.desc.PageBytes());
        for (const auto* channel : {&result.albedoSrgb, &result.normalLocalUnorm, &result.ormLinear})
            page.bytes.insert(page.bytes.end(), channel->begin(), channel->end());
        Require(page.bytes.size() == surface.desc.PageBytes() &&
            result.contentIdentity == surface.input.contentIdentity, "GPU Cook returned mismatched page");
        return page;
    }

    void SnapshotStats()
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
};

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
        impl->qualityConvergenceFailed.store(true, std::memory_order_release);
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
        impl->qualityConvergenceFailed.store(true, std::memory_order_release);
        impl->interactiveReady.store(false, std::memory_order_release);
    }
    return impl->qualityConvergenceFailed.load(std::memory_order_acquire);
}

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
}
