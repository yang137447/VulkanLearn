#include "render/resource/terrainCompositedProductionGpu.h"
#include "render/backend/rendererBackendVulkan.h"
#include "vulkanManager.h"
#include "terrain/terrainCompositedVTStore.h"
#include "texture.h"
#include "resource/device/deviceTextureFactory.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace
{
using namespace VL;
using Gpu = TerrainCompositedProductionGpu;
using Json = nlohmann::json;

void Check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function>
void Reject(Json& report, const std::string& name, Function function)
{
    try
    {
        function();
    }
    catch (const std::exception& error)
    {
        report["negativeChecks"].push_back({{"name", name}, {"rejected", true},
            {"error", error.what()}});
        return;
    }
    throw std::runtime_error("Expected rejection: " + name);
}

TerrainRvtBudgetCaps Caps()
{
    TerrainRvtBudgetCaps caps;
    caps.inputGpuBytes = caps.workspaceGpuBytes = caps.totalGpuBytes = 1ull << 30;
    caps.inputCpuBytes = caps.workspaceCpuBytes = caps.totalCpuBytes = 1ull << 30;
    caps.workspaceGpuBytes = 32ull * 1024 * 1024;
    caps.inputGpuBytes = 805306368;
    caps.workspaceCpuBytes = 256ull * 1024 * 1024;
    caps.totalCpuBytes = 512ull * 1024 * 1024;
    caps.inputCpuBytes = caps.totalCpuBytes;
    return caps;
}

TerrainCompositedProductionAllocationOverheads Overheads()
{
    TerrainCompositedProductionAllocationOverheads result;
    result.imageViewAndSampler = {4096, 4096, 0, 0};
    result.imageMetadata = {0, 4096, 0, 0};
    result.transferMetadata = {0, 4096, 0, 0};
    result.queryMetadata = {0, 4096, 0, 0};
    return result;
}

struct Fixture
{
    TerrainCompositedProductionClosure closure;
    std::map<std::string, std::vector<uint8_t>> payloads;
};

HostImage Pixels(const TerrainCompositedProductionObject& object)
{
    auto image = Gpu::ImageLayout(object.image);
    image.data.resize(image.GetTotalBytes());
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
        for (uint32_t mip = 0; mip < image.mipLevels; ++mip)
        {
            auto* bytes = static_cast<uint8_t*>(image.GetMipData(mip, layer));
            const auto size = image.GetMipLayerBytes(mip);
            for (size_t offset = 0; offset < size; ++offset)
                bytes[offset] = uint8_t((offset * 37 + layer * 53 + mip * 71 + 19) & 255);
            if (object.image.format == "r32-float" || object.image.format == "rgba32-float")
                for (size_t offset = 0; offset < size; offset += 4)
                {
                    float value = float((offset / 4 + layer * 17 + mip * 31) % 257) / 256.f;
                    if (object.objectId == "catalog")
                        value = offset % 16 == 8 ? 1.f : 0.f;
                    if (object.objectId == "frame-normal")
                        value = offset % 16 == 4 ? 1.f : 0.f;
                    if (object.objectId == "frame-tangent")
                        value = offset % 16 == 0 || offset % 16 == 12 ? 1.f : 0.f;
                    std::memcpy(bytes + offset, &value, 4);
                }
        }
    return image;
}

Fixture BuildFixture(const std::filesystem::path& parent, TerrainRvtBudgetOwner& budget,
    TerrainCompositedWeightSource mode, bool large)
{
    std::filesystem::create_directories(parent);
    Fixture result;
    auto& closure = result.closure;
    const auto add = [&](const std::string& id, const std::string& format,
        uint32_t width, uint32_t height, uint32_t layers = 1, uint32_t mips = 1)
    {
        TerrainCompositedProductionObject object;
        object.objectId = id;
        object.image = {format, width, height, layers, mips};
        if (format == "rgba8-srgb" || format == "bc7-srgb")
            object.image.transfer = TerrainCompositedProductionTransfer::Srgb;
        object.image.filter = TerrainCompositedProductionFilter::Linear;
        object.image.address = id == "albedo" || id == "normal" || id == "orm"
            ? TerrainCompositedProductionAddress::Repeat : TerrainCompositedProductionAddress::Clamp;
        object = Gpu::CaptureCpuImage(object, Pixels(object), budget,
            [&](const auto& captured, const auto& bytes)
            { result.payloads[captured.objectId] = bytes; });
        object.relativePath = "objects/" + object.digest.ToHex() + "-" + id + ".bin";
        Gpu::WriteInputObject(parent, object, result.payloads.at(id));
        closure.objects.push_back(object);
        return id;
    };
    TerrainCompositedProductionBindings bindings;
    bindings.coveragePageTable = add("coverage-table", "rgba8-unorm", large ? 32 : 1, large ? 32 : 1);
    bindings.coverageIds = add("coverage-ids", "rgba8-unorm", large ? 4096 : 128, large ? 4096 : 128);
    bindings.coverageWeights = add("coverage-weights", "rgba8-unorm", large ? 4096 : 128, large ? 4096 : 128);
    bindings.layerCatalog = add("catalog", "rgba32-float", 256, 4);
    bindings.layerAlbedo = add("albedo", large ? "bc7-srgb" : "rgba8-srgb",
        large ? 4097 : 17, large ? 1031 : 11, 3, large ? 13 : 5);
    bindings.layerNormal = add("normal", large ? "bc5-unorm" : "rgba8-unorm",
        large ? 4097 : 17, large ? 1031 : 11, 3, large ? 13 : 5);
    bindings.layerOrm = add("orm", large ? "bc7-unorm" : "rgba8-unorm",
        large ? 4097 : 17, large ? 1031 : 11, 3, large ? 13 : 5);
    bindings.surfaceLocalNormal = add("frame-normal", "rgba32-float", 513, 513);
    bindings.surfaceLocalTangent = add("frame-tangent", "rgba32-float", 513, 513);
    if (mode == TerrainCompositedWeightSource::LocalLayerArray)
        bindings.localLayerWeights = add("local-weights", "r32-float", large ? 513 : 7, large ? 513 : 7, 3);
    add("r32-mips", "r32-float", large ? 513 : 9, large ? 515 : 7, 1, large ? 10 : 4);
    add("bc5-raw", "bc5-unorm", 13, 9, 1, 4);
    add("bc7-raw", "bc7-unorm", 13, 9, 1, 4);
    add("bc7-srgb-raw", "bc7-srgb", 13, 9, 1, 4);
    const auto shader = [&](const std::string& id, TerrainCompositedProductionObjectKind kind)
    {
        TerrainCompositedProductionObject object;
        object.objectId = id;
        object.kind = kind;
        object.relativePath = "objects/" + id + ".bin";
        const std::string text = "fixture-identity-only-not-executed";
        object.bytes = text.size();
        object.digest = ContentHasher::HashString(text);
        std::ofstream(parent / object.relativePath, std::ios::binary) << text;
        closure.objects.push_back(object);
        return id;
    };
    closure.generator = {TerrainCompositedGeneratorVersion, "M_terrain", TerrainCompositedFilterRule,
        TerrainCompositedEncoding, "fixture-not-a-bake-dispatch",
        shader("compute", TerrainCompositedProductionObjectKind::ShaderSource), {},
        {shader("artifact", TerrainCompositedProductionObjectKind::ShaderArtifact)}};
    std::vector<std::string> ids;
    for (uint32_t placement = 0; placement < 2; ++placement)
    {
        TerrainCompositedSurfaceProduction surface;
        surface.surfaceId = "terrain|placement-" + std::to_string(placement);
        surface.inputs = bindings;
        surface.weightSource = mode;
        surface.localToGlobal = {19, 3, 7};
        surface.globalToSourceSlice[19] = 2;
        surface.globalToSourceSlice[3] = 0;
        surface.globalToSourceSlice[7] = 1;
        surface.localToWorld = {0, 0, -2, 0, 0, 2, 0, 0, 2, 0, 0, 0,
            double(2500 + placement * 100), 64, -128, 1};
        surface.sampling.surfaceUvOffset = {0.125, 0.25};
        surface.sampling.surfaceUvScale = {0.5, 0.75};
        surface.sampling.coverageUvOffset = {0.25, 0.125};
        surface.sampling.coverageUvScale = {0.75, 0.5};
        surface.sampling.frameUvOffset = {0.125, 0.375};
        surface.sampling.frameUvScale = {0.875, 0.625};
        surface.parameters.uvScale = 64;
        surface.runtimeMaterialInstancePath = "runtime/MI_production.json";
        std::filesystem::create_directories(parent / "runtime");
        std::ofstream(parent / surface.runtimeMaterialInstancePath) <<
            R"({"type":"materialInstance","material":"shader/glsl/M_terrain.json","parameters":{}})";
        ids.push_back(surface.surfaceId);
        closure.surfaces.push_back(surface);
    }
    closure.adjacency = TerrainCompositedAdjacencyGraph(ids, {});
    closure.Validate();
    TerrainCompositedVTPublication publication;
    publication.production = closure;
    std::vector<TerrainCompositedVTPageData> pages;
    for (const auto& surface : closure.surfaces)
    {
        TerrainCompositedVTBuildDesc desc;
        desc.surfaceId = surface.surfaceId;
        desc.width = desc.height = 1;
        desc.generatorVersion = TerrainCompositedGeneratorVersion;
        desc.dependencyHash = closure.SurfaceContentHash(surface.surfaceId);
        desc.encoding = TerrainCompositedEncoding;
        desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
            TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
        publication.surfaces.push_back(desc);
        const TerrainCompositedVTPageKey key{surface.surfaceId, 0, 0, 0};
        publication.packagedPages.push_back(key);
        pages.push_back({key, std::vector<uint8_t>(desc.PageBytes(), 0)});
    }
    TerrainCompositedVTStore::Publish(parent / "manifest.json", publication, pages);
    return result;
}

void CompareImage(Json& report, const std::string& name, RendererBackendVulkan& backend,
    TerrainRvtBudgetOwner& budget, const TerrainCompositedProductionAllocationQuery& allocations,
    const TerrainCompositedProductionObject& object, std::shared_ptr<Texture> texture,
    const std::vector<uint8_t>& expected)
{
    report["lastStage"] = {{"operation", "CaptureGpuImage"}, {"objectId", object.objectId},
        {"case", name}, {"width", object.image.width}, {"height", object.image.height},
        {"layers", object.image.layers}, {"mips", object.image.mipLevels},
        {"expectedBytes", expected.size()}};
    Json subresources = Json::array();
    uint64_t actualBytes = 0;
    const auto captured = Gpu::CaptureGpuImage(backend, object, texture, budget,
        [&](const auto& metadata, const auto& bytes)
        {
            Check(bytes == expected && metadata.digest == object.digest, "Roundtrip bytes differ: " + name);
            actualBytes = bytes.size();
            const auto workspace = budget.Statistics().byClass[size_t(TerrainRvtCostClass::Workspace)];
            const auto previous = report.value("observedWorkspaceGpuBytes", uint64_t(0));
            report["observedWorkspaceGpuBytes"] = std::max(previous, workspace.gpuBytes);
            auto layout = Gpu::ImageLayout(object.image);
            uint64_t offset = 0;
            for (uint32_t layer = 0; layer < layout.arrayLayers; ++layer)
                for (uint32_t mip = 0; mip < layout.mipLevels; ++mip)
                {
                    const auto size = layout.GetMipLayerBytes(mip);
                    subresources.push_back({{"layer", layer}, {"mip", mip}, {"bytes", size},
                        {"blake3", ContentHasher::HashBytes(bytes.data() + offset, size).ToHex()}});
                    offset += size;
                }
            Check(offset == bytes.size(), "Readback digest layout mismatch");
        }, allocations);
    Check(captured.digest == object.digest, "Roundtrip digest mismatch");
    Check(texture->GetDescriptorInfo().imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal,
        "Descriptor layout not readable");
    uint64_t chunkCount = 0;
    uint64_t maximumChunk = 0;
    Gpu::VisitTransferChunks(object.image, [&](const auto& chunk) {
        ++chunkCount;
        maximumChunk = std::max(maximumChunk, chunk.bytes);
    });
    report["roundtrips"].push_back({{"name", name}, {"format", object.image.format},
        {"bytes", actualBytes}, {"mismatchBytes", 0}, {"digest", captured.digest.ToHex()},
        {"transferChunks", chunkCount}, {"maximumChunkBytes", maximumChunk},
        {"subresources", subresources}});
}

void RunMode(Json& report, const std::filesystem::path& root, RendererBackendVulkan& backend,
    TerrainCompositedWeightSource mode, bool large = false)
{
    const std::string name = std::string(mode == TerrainCompositedWeightSource::CoverageAtlas
        ? "coverage" : "local") + (large ? "-large" : "");
    const auto parent = root / name;
    TerrainRvtBudgetOwner budget(Caps());
    std::optional<TerrainRvtBudgetLease> shared;
    if (large)
        shared = budget.TryReserve({28ull * 1024 * 1024, 0, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    Check(!large || bool(shared), "Could not reserve existing shared workspace");
    const auto baseline = budget.Statistics().current;
    auto allocations = Gpu::MakeAllocationQuery(backend, budget, Overheads());
    const auto transferQuery = allocations.transfer;
    uint64_t maximumQuote = 0;
    uint64_t maximumStage = 0;
    allocations.transfer = [&](uint64_t bytes)
    {
        maximumQuote = std::max(maximumQuote, bytes);
        Check(bytes <= Gpu::MaxTransferBytes, "Whole-image staging quote used");
        const auto cost = transferQuery(bytes);
        maximumStage = std::max(maximumStage, cost.gpuBytes);
        return cost;
    };
    const auto fixture = BuildFixture(parent, budget, mode, large);
    auto store = TerrainCompositedVTStore::Open(parent / "manifest.json");
    store.ValidateAll();
    const auto initialResources = backend.CaptureResourceIdentityCounts();
    auto owner = Gpu::Restore(backend, store, budget, allocations);
    const auto charged = budget.Statistics().current;
    Check(charged.gpuBytes > 0 && charged.cpuBytes > 0, "Input budget not charged");
    Check(budget.Statistics().byClass[size_t(TerrainRvtCostClass::Workspace)] == baseline,
        "Workspace lease leaked");
    for (const auto& object : fixture.closure.objects)
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
        {
            CompareImage(report, name + "/" + object.objectId, backend, budget, allocations,
                object, owner->Image(object.objectId), fixture.payloads.at(object.objectId));
            CompareImage(report, name + "/" + object.objectId + "/repeat", backend, budget, allocations,
                object, owner->Image(object.objectId), fixture.payloads.at(object.objectId));
        }
    for (const auto& surface : fixture.closure.surfaces)
    {
        auto owned = Gpu::MakeBakeInput(owner, surface.surfaceId);
        const auto& input = owned.input;
        Check(input.weightSource == (mode == TerrainCompositedWeightSource::CoverageAtlas
            ? TerrainCompositedBakeGpu::WeightSource::CoverageAtlas
            : TerrainCompositedBakeGpu::WeightSource::LocalLayerArray), "Weight mode mismatch");
        Check(input.localLayerCount == 3 && input.globalToSourceSlice == surface.globalToSourceSlice,
            "Global/source mapping mismatch");
        for (size_t local = 0; local < 3; ++local)
            Check(input.localToGlobalLayer[local] == surface.localToGlobal[local], "Local mapping mismatch");
        for (size_t index = 0; index < 16; ++index)
            Check(input.localToWorld[index] == float(surface.localToWorld[index]), "Placement mismatch");
        const auto pair = [](const auto& actual, const auto& expected)
        { return actual[0] == float(expected[0]) && actual[1] == float(expected[1]); };
        Check(pair(input.surfaceUvOffset, surface.sampling.surfaceUvOffset) &&
            pair(input.surfaceUvScale, surface.sampling.surfaceUvScale) &&
            pair(input.coverageUvOffset, surface.sampling.coverageUvOffset) &&
            pair(input.coverageUvScale, surface.sampling.coverageUvScale) &&
            pair(input.frameUvOffset, surface.sampling.frameUvOffset) &&
            pair(input.frameUvScale, surface.sampling.frameUvScale), "Sampling mismatch");
        Check(input.layerAlbedo.encoding == TerrainCompositedBakeGpu::Encoding::SrgbView &&
            input.layerAlbedo.descriptor == owner->Image(surface.inputs.layerAlbedo)->GetDescriptorInfo(),
            "Albedo encoding/descriptor mismatch");
        const std::vector<std::pair<const TerrainCompositedBakeGpu::SampledTexture*, std::string>> samples{
            {&input.coveragePageTable, surface.inputs.coveragePageTable},
            {&input.coverageIds, surface.inputs.coverageIds},
            {&input.coverageWeights, surface.inputs.coverageWeights},
            {&input.layerCatalog, surface.inputs.layerCatalog},
            {&input.layerAlbedo, surface.inputs.layerAlbedo},
            {&input.layerNormal, surface.inputs.layerNormal}, {&input.layerOrm, surface.inputs.layerOrm},
            {&input.surfaceLocalNormal, surface.inputs.surfaceLocalNormal},
            {&input.surfaceLocalTangent, surface.inputs.surfaceLocalTangent},
            {&input.localLayerWeights, surface.inputs.localLayerWeights}};
        for (const auto& sample : samples)
            if (!sample.second.empty())
            {
                const auto& desc = fixture.closure.Object(sample.second).image;
                Check(sample.first->descriptor == owner->Image(sample.second)->GetDescriptorInfo() &&
                    sample.first->width == desc.width && sample.first->height == desc.height &&
                    sample.first->layers == desc.layers && sample.first->mipLevels == desc.mipLevels,
                    "Descriptor metadata mismatch: " + sample.second);
            }
        Check(input.contentIdentity == fixture.closure.SurfaceContentHash(surface.surfaceId).ToHex(),
            "Content identity mismatch");
        report["snapshots"].push_back({{"mode", name}, {"surface", surface.surfaceId},
            {"mapping", surface.localToGlobal}, {"contentIdentity", input.contentIdentity},
            {"descriptorAndMetadataMatch", true},
            {"runtimeMiAbsolutePath", (parent / surface.runtimeMaterialInstancePath).string()},
            {"runtimeMiExists", std::filesystem::exists(parent / surface.runtimeMaterialInstancePath)}});
        auto image = Gpu::SourceMapImage(surface);
        TerrainCompositedProductionObject object;
        object.objectId = "source-map-" + surface.surfaceId;
        object.image = {"rgba32-float", 256, 2};
        Gpu::PopulateCapturedImageMetadata(object, image.data);
        CompareImage(report, name + "/source-map/" + surface.surfaceId, backend, budget, allocations,
            object, owner->SourceMap(surface.surfaceId), image.data);
    }
    Reject(report, name + "/missing-closure-dependency", [&] {
        auto broken = fixture.closure;
        broken.objects.erase(std::remove_if(broken.objects.begin(), broken.objects.end(),
            [](const auto& object) { return object.objectId == "albedo"; }), broken.objects.end());
        Gpu::Restore(backend, broken, [&](const auto& object) {
            return fixture.payloads.at(object.objectId); }, budget, allocations);
    });
    for (const std::string fault : {"digest", "truncated", "reader"})
    {
        const auto before = backend.CaptureResourceIdentityCounts();
        Reject(report, name + "/" + fault, [&] {
            Gpu::Restore(backend, fixture.closure, [&](const auto& object) {
                auto bytes = fixture.payloads.at(object.objectId);
                if (object.objectId == "frame-normal")
                {
                    if (fault == "digest") bytes.at(0) ^= 1;
                    if (fault == "truncated") bytes.pop_back();
                    if (fault == "reader") throw std::runtime_error("missing immutable input");
                }
                return bytes;
            }, budget, allocations);
        });
        Check(backend.CaptureResourceIdentityCounts() == before, "Failed restore leaked GPU objects");
        Check(budget.Statistics().current == charged, "Failed restore leaked budget/changed active owner");
    }
    Reject(report, name + "/no-allocation-query", [&] {
        Gpu::Restore(backend, fixture.closure,
            [&](const auto& object) { return fixture.payloads.at(object.objectId); }, budget);
    });
    auto caps = Caps();
    caps.inputGpuBytes = 1;
    TerrainRvtBudgetOwner lowBudget(caps);
    const auto lowQuery = Gpu::MakeAllocationQuery(backend, lowBudget, Overheads());
    const auto beforeLow = backend.CaptureResourceIdentityCounts();
    uint32_t readerCalls = 0;
    Reject(report, name + "/input-budget", [&] {
        Gpu::Restore(backend, fixture.closure, [&](const auto& object) {
            ++readerCalls;
            return fixture.payloads.at(object.objectId); }, lowBudget, lowQuery);
    });
    Check(readerCalls == 0 && backend.CaptureResourceIdentityCounts() == beforeLow &&
        lowBudget.Statistics().current == TerrainRvtCost{}, "Budget rejection allocated/read inputs");
    caps = Caps();
    caps.workspaceGpuBytes = 1;
    TerrainRvtBudgetOwner lowWorkspace(caps);
    const auto workspaceQuery = Gpu::MakeAllocationQuery(backend, lowWorkspace, Overheads());
    Reject(report, name + "/capture-workspace-budget", [&] {
        Gpu::CaptureGpuImage(backend, fixture.closure.Object("albedo"), owner->Image("albedo"),
            lowWorkspace, [](const auto&, const auto&) {}, workspaceQuery);
    });
    Check(lowWorkspace.Statistics().current == TerrainRvtCost{}, "Capture failure leaked workspace");
    Reject(report, name + "/capture-metadata", [&] {
        auto object = fixture.closure.Object("albedo");
        object.image.width += 1;
        Gpu::CaptureGpuImage(backend, object, owner->Image("albedo"), budget,
            [](const auto&, const auto&) {}, allocations);
    });
    Reject(report, name + "/capture-writer", [&] {
        Gpu::CaptureGpuImage(backend, fixture.closure.Object("albedo"), owner->Image("albedo"), budget,
            [](const auto&, const auto&) { throw std::runtime_error("writer failure"); }, allocations);
    });
    Check(budget.Statistics().current == charged, "Capture failure leaked budget");
    Reject(report, name + "/unknown-surface", [&] { Gpu::MakeBakeInput(owner, "not-a-surface"); });
    std::weak_ptr<TerrainCompositedProductionGpuResources> weak = owner;
    auto alias = owner->Image("albedo");
    auto mapAlias = owner->SourceMap(fixture.closure.surfaces.front().surfaceId);
    auto inFlight = Gpu::MakeBakeInput(owner, fixture.closure.surfaces.front().surfaceId);
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    Check(budget.Statistics().byPhase[size_t(TerrainRvtResourcePhase::Active)] == charged,
        "Active lease phase mismatch");
    owner->SetPhase(TerrainRvtResourcePhase::Retiring);
    owner.reset();
    Check(!weak.expired() && budget.Statistics().current == charged &&
        budget.Statistics().byPhase[size_t(TerrainRvtResourcePhase::Retiring)].gpuBytes ==
            charged.gpuBytes - baseline.gpuBytes &&
        budget.Statistics().byPhase[size_t(TerrainRvtResourcePhase::Retiring)].cpuBytes == charged.cpuBytes,
        "Owner retired while consumers still alive");
    inFlight = {};
    alias.reset();
    Check(!weak.expired() && budget.Statistics().current == charged, "SourceMap alias did not keep owner alive");
    mapAlias.reset();
    backend.WaitIdle();
    Check(weak.expired() && budget.Statistics().current == baseline &&
        backend.CaptureResourceIdentityCounts() == initialResources, "Final consumer leaked owner/resources/budget");
    shared.reset();
    Check(budget.Statistics().current == TerrainRvtCost{}, "Shared workspace leaked");
    report["leases"].push_back({{"mode", name}, {"chargedGpuBytes", charged.gpuBytes},
        {"chargedCpuBytes", charged.cpuBytes}, {"inputSnapshotKeepsOwner", true},
        {"textureAndSourceMapAliasesKeepOwner", true}, {"activeRetiringPhases", true},
        {"finalGpuBytes", 0}, {"finalCpuBytes", 0}, {"resourcesReturnToBaseline", true},
        {"workspaceGpuCapBytes", Caps().workspaceGpuBytes}, {"sharedWorkspaceGpuBytes", baseline.gpuBytes},
        {"maximumTransferQueryBytes", maximumQuote}, {"maximumActualStagingGpuBytes", maximumStage}});
}
}

int main(int argc, char** argv)
{
    if (argc != 2 && !(argc == 3 && std::string(argv[2]) == "--large-atlas"))
        return 2;
    nlohmann::json report;
    try
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
            throw std::runtime_error(SDL_GetError());
        auto* window = SDL_CreateWindow("ProductionGpu diagnostic", 64, 64,
            SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!window)
            throw std::runtime_error(SDL_GetError());
        uint32_t extensionCount = 0;
        const auto extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        if (!extensions)
            throw std::runtime_error(SDL_GetError());
        std::vector<const char*> instanceExtensions(extensions, extensions + extensionCount);
        VulkanManager::GetInstance().SetValidationEnabled(true);
        {
            VL::RendererBackendVulkan backend;
            backend.Initialize(instanceExtensions, window, VL::PresentModePreference::Immediate);
            report["deviceScope"] = "actual backend selection; device details in backend stdout";
            report["scope"] = "actual ProductionGpu; backend only; no World, Cook or performance claim";
            VL::TerrainRvtBudgetOwner budget(Caps());
            const auto overheads = Overheads();
            report["overheadScope"] = "explicit diagnostic reservations, not P0 opaque allocation measurements";
            const auto allocations = VL::TerrainCompositedProductionGpu::MakeAllocationQuery(
                backend, budget, overheads);
            Reject(report, "production-input-512-layer-category-misuse", [&] {
                TerrainCompositedProductionImageDesc invalid{"rgba8-unorm", 136, 136, 512, 1};
                allocations.image(invalid, true);
            });
            for (const auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb})
            {
                vk::ImageCreateInfo create;
                create.setImageType(vk::ImageType::e2D).setExtent({136, 136, 1})
                    .setArrayLayers(512).setMipLevels(1).setSamples(vk::SampleCountFlagBits::e1)
                    .setFormat(format).setTiling(vk::ImageTiling::eOptimal)
                    .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                        vk::ImageUsageFlagBits::eTransferSrc)
                    .setSharingMode(vk::SharingMode::eExclusive).setInitialLayout(vk::ImageLayout::eUndefined);
                const auto requirements = backend.QueryImageMemoryRequirements(create);
                report["allocationQueries"].push_back({
                    {"api", "RendererBackendVulkan::QueryImageMemoryRequirements"},
                    {"scope", "physical pool, not Production source-input desc"},
                    {"format", vk::to_string(format)}, {"layers", 512},
                    {"payloadBytes", uint64_t(136) * 136 * 512 * 4},
                    {"actualVkAllocationBytes", requirements.size}, {"alignment", requirements.alignment},
                    {"expectedP0Bytes", 75497472}, {"matchesP0", requirements.size == 75497472}});
                Check(requirements.size == 75497472, "Actual backend physical pool query differs from P0");
            }
            TerrainCompositedProductionImageDesc frame{"rgba32-float", 513, 513};
            const auto frameCost = allocations.image(frame, false);
            const auto frameBytes = frameCost.gpuBytes - overheads.imageViewAndSampler.gpuBytes;
            report["allocationQueries"].push_back({
                {"api", "TerrainCompositedProductionGpu::MakeAllocationQuery.image"},
                {"scope", "Production input; explicit opaque overhead subtracted"},
                {"format", frame.format}, {"payloadBytes", frame.ByteSize()},
                {"actualVkAllocationBytes", frameBytes}, {"expectedP0Bytes", 5283840},
                {"matchesP0", frameBytes == 5283840}});
            Check(frameBytes == 5283840, "Actual ProductionGpu frame allocation differs from P0");
            const auto root = std::filesystem::path(argv[1]).parent_path() / "fixtures";
            RunMode(report, root, backend, TerrainCompositedWeightSource::CoverageAtlas);
            RunMode(report, root, backend, TerrainCompositedWeightSource::LocalLayerArray);
            if (argc == 3)
            {
                RunMode(report, root, backend, TerrainCompositedWeightSource::CoverageAtlas, true);
                RunMode(report, root, backend, TerrainCompositedWeightSource::LocalLayerArray, true);
            }
            backend.WaitIdle();
            report["bakeDispatchScope"] = "no Baker definition linked/tested; transfers and MakeBakeInput only";
            report["status"] = "passed";
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    catch (const std::exception& error)
    {
        report["status"] = "failed";
        report["error"] = error.what();
    }
    std::ofstream(argv[1]) << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return report["status"] == "failed" ? 1 : 0;
}
