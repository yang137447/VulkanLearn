#include "render/resource/terrainCompositedServiceDetail.h"

namespace VL
{
namespace TerrainServiceDetail
{
std::atomic<bool> GlobalCookMode{false};

std::atomic<bool> GlobalAuthoringMode{false};

std::atomic<uint64_t> NextGeneration{0};

std::atomic<bool> GlobalWorldHasBeenActivated{false};

std::mutex FrozenConfigMutex;

std::optional<TerrainCompositedFrozenConfig> FrozenConfig;

std::mutex EvidenceBudgetMutex;

std::optional<TerrainRvtBudgetLease> EvidenceProcessLease;

std::optional<TerrainRvtBudgetLease> FileTraceProcessLease;

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
    const std::shared_ptr<Texture>& authorWeights,
    const std::optional<std::string>& authoredWeightIdentity)
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
    vk::ImageLayout previousLayout)
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
    const std::string& name, vk::Filter filter, bool array)
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
} // namespace TerrainServiceDetail
} // namespace VL
