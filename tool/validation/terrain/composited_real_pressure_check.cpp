#include "render/resource/terrainCompositedProductionGpu.h"
#include "render/backend/rendererBackendVulkan.h"
#include "terrain/terrainCompositedVTStore.h"
#include "terrain/terrainAssetLoader.h"
#include "terrain/terrainCompositedWeightOverlay.h"
#include "textureAssetLoader.h"
#include "pipeline/pipelineFactory.h"
#include "commonFunction.h"
#include "vulkanManager.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <fstream>
#include <iostream>
#include <set>

namespace
{
using namespace VL;
using Baker = TerrainCompositedBakeGpu;
using Gpu = TerrainCompositedProductionGpu;
using Json = nlohmann::json;
constexpr uint64_t MiB = 1024 * 1024;
constexpr const char* SourceId = "terrain|Landscape_00";

void Check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Json ReadJson(const std::filesystem::path& path)
{
    std::ifstream file(path);
    Check(bool(file), "Cannot read JSON: " + path.string());
    Json result;
    file >> result;
    return result;
}

TerrainRvtBudgetCaps Caps()
{
    TerrainRvtBudgetCaps caps;
    caps.physicalPoolGpuBytes = 224 * MiB;
    caps.inputGpuBytes = 768 * MiB;
    caps.workspaceGpuBytes = 32 * MiB;
    caps.totalGpuBytes = 1024 * MiB;
    caps.totalCpuBytes = 512 * MiB;
    caps.physicalPoolCpuBytes = caps.inputCpuBytes = caps.workspaceCpuBytes = 512 * MiB;
    caps.physicalSlots = 512;
    caps.productionTasks = 4;
    return caps;
}

TerrainRvtBudgetLease Charge(TerrainRvtBudgetOwner& owner, TerrainRvtCost cost,
    TerrainRvtCostClass category = TerrainRvtCostClass::Workspace)
{
    auto lease = owner.TryReserve(cost, category, TerrainRvtResourcePhase::Active);
    Check(bool(lease), "Frozen real-pressure budget admission failed");
    return std::move(*lease);
}

Json BudgetStats(const TerrainRvtBudgetOwner& owner)
{
    const auto stats = owner.Statistics();
    Json result{{"gpuBytes", stats.current.gpuBytes}, {"cpuBytes", stats.current.cpuBytes},
        {"peakGpuBytes", stats.peak.gpuBytes}, {"peakCpuBytes", stats.peak.cpuBytes},
        {"productionTasks", stats.current.productionTasks}, {"rejected", stats.rejected}};
    for (size_t index = 0; index < stats.byClass.size(); ++index)
        result["classes"].push_back({{"gpuBytes", stats.byClass[index].gpuBytes},
            {"cpuBytes", stats.byClass[index].cpuBytes}});
    return result;
}

void ValidateWorld(const TerrainCompositedProductionClosure& closure, uint32_t mode)
{
    closure.Validate();
    Check(closure.surfaces.size() == 16, "Real pressure requires all 16 World input snapshots");
    for (const auto& surface : closure.surfaces)
        Check(uint32_t(surface.weightSource) == mode && surface.parameters.heightBlendStrength == 3 &&
            surface.parameters.frozenHeightBlendFade == 1, "Real weight mode/heightBlend differs from P0");
}

void FreezeLocal(const std::filesystem::path& manifest, const std::string& terrainPath,
    const std::filesystem::path& output)
{
    const auto store = TerrainCompositedVTStore::Open(manifest);
    auto closure = store.Production();
    ValidateWorld(closure, 0);
    TerrainRvtBudgetOwner budget(Caps());
    auto workspace = Charge(budget, {0, 256 * MiB, 0, 0});
    const auto plan = TerrainAssetLoader::Prepare(terrainPath);
    Check(plan.desc.heightmapMargin == 1 && bool(plan.layerPaint),
        "Real local pressure requires margin1 and the authored Paint snapshot");
    const auto material = ReadJson(CommonFunction::Path(plan.desc.materialInstancePath));
    const std::string weightAsset = material.at("textures").at("layerWeightMaps");
    auto description = LoadTextureAssetDesc(weightAsset);
    Check(description.textureArray && !description.sources.empty() &&
        description.transfer == TextureIO::LoadOptions::Transfer::Linear,
        "Local pressure must consume the real linear authored weight array");
    description.semantic = HostImage::TextureSemantic::Data;
    std::vector<std::filesystem::path> paths;
    for (const auto& source : description.sources)
        paths.push_back(CommonFunction::Path(source));
    auto imported = TextureIO::LoadArray(paths,
        Texture::MaterialLoadOptionsForPath(ToTextureCreateDesc(description), paths.front()));
    Json receipt{{"gpuExecuted", false}, {"scope", "unpublished real-input diagnostic snapshot"},
        {"baseManifest", std::filesystem::absolute(manifest).string()},
        {"baseProductionBlake3", closure.ContentHash().ToHex()}, {"heightmapMargin", 1},
        {"terrain", terrainPath}, {"weightAsset", weightAsset}};
    const auto record = [&](const std::string& path) {
        receipt["authorFiles"].push_back({{"path", path},
            {"blake3", ContentHasher::HashFile(CommonFunction::Path(path)).ToHex()}});
    };
    record(terrainPath);
    record(plan.desc.heightmapPath);
    record(plan.desc.paintPath);
    record(plan.desc.materialInstancePath);
    record(weightAsset);
    for (const auto& source : description.sources)
        record(source);
    std::filesystem::create_directories(output);
    std::map<std::vector<uint32_t>, std::string> frozen;
    for (auto& surface : closure.surfaces)
    {
        Check(surface.localToGlobal.size() == plan.desc.layers.size(), "TR/source map layer count mismatch");
        auto found = frozen.find(surface.localToGlobal);
        if (found == frozen.end())
        {
            std::vector<uint8_t> mapping(surface.localToGlobal.begin(), surface.localToGlobal.end());
            auto image = BuildTerrainCompositedWeightOverlay(plan, imported, mapping);
            TerrainCompositedProductionObject object;
            object.objectId = "pressure-local-weights-" + std::to_string(frozen.size());
            object.image = {"r32-float", image.width, image.height, image.arrayLayers, 1,
                TerrainCompositedProductionTransfer::Linear,
                TerrainCompositedProductionAddress::Clamp, TerrainCompositedProductionFilter::Linear};
            object = Gpu::CaptureCpuImage(object, image, budget,
                [&](const auto& metadata, const auto& bytes) {
                    auto stored = metadata;
                    stored.relativePath = "inputs/" + metadata.digest.ToHex() + ".bin";
                    Gpu::WriteInputObject(output, stored, bytes);
                });
            object.relativePath = "inputs/" + object.digest.ToHex() + ".bin";
            found = frozen.emplace(surface.localToGlobal, object.objectId).first;
            receipt["weights"].push_back({{"objectId", object.objectId},
                {"bytes", object.bytes}, {"blake3", object.digest.ToHex()}});
            closure.objects.push_back(std::move(object));
        }
        surface.weightSource = TerrainCompositedWeightSource::LocalLayerArray;
        surface.inputs.localLayerWeights = found->second;
    }
    ValidateWorld(closure, 1);
    receipt["localProductionBlake3"] = closure.ContentHash().ToHex();
    receipt["budget"] = BudgetStats(budget);
    std::ofstream(output / "local-closure.json") << TerrainCompositedProductionToJson(closure).dump(2);
    std::ofstream(output / "freeze-receipt.json") << receipt.dump(2);
    std::cout << receipt.dump(2) << std::endl;
}

struct Buffer
{
    RendererBackendVulkan& backend;
    TerrainRvtBudgetLease lease;
    vk::Buffer handle;
    vk::DeviceMemory memory;
    uint64_t bytes;
    Buffer(RendererBackendVulkan& owner, TerrainRvtBudgetOwner& budget, uint64_t size,
        vk::BufferUsageFlags usage, bool host = false)
        : backend(owner), lease(Charge(budget,
            {owner.QueryBufferMemoryRequirements(size, usage).size,
             host ? owner.QueryBufferMemoryRequirements(size, usage).size : 0, 0, 0})), bytes(size)
    {
        const auto created = backend.CreateBuffer(size, usage, host ?
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent :
            vk::MemoryPropertyFlagBits::eDeviceLocal, "RealPressureTaskBuffer");
        handle = created.first;
        memory = created.second;
    }
    ~Buffer() { if (handle) backend.DestroyBuffer(handle, memory); }
};

struct Task
{
    Buffer output, status;
    Task(RendererBackendVulkan& backend, TerrainRvtBudgetOwner& budget)
        : output(backend, budget, Baker::OutputBytes, vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst),
          status(backend, budget, 4, vk::BufferUsageFlagBits::eTransferDst, true) {}
};

struct Queries
{
    RendererBackendVulkan& backend;
    vk::QueryPool pool;
    explicit Queries(RendererBackendVulkan& owner)
        : backend(owner), pool(owner.CreateTimestampQueryPool(28, "RealPressureTimestamps")) {}
    ~Queries() { backend.DestroyQueryPool(pool); }
};

void AtlasTransition(vk::CommandBuffer command, Texture& texture, bool begin)
{
    const auto readers = vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader;
    vk::ImageMemoryBarrier barrier(
        begin ? vk::AccessFlagBits::eShaderRead : vk::AccessFlagBits::eTransferWrite,
        begin ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eShaderRead,
        begin ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eTransferDstOptimal,
        begin ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eShaderReadOnlyOptimal,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, texture.getImage(),
        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 512});
    command.pipelineBarrier(begin ? readers : vk::PipelineStageFlagBits::eTransfer,
        begin ? vk::PipelineStageFlagBits::eTransfer : readers, {}, {}, {}, barrier);
}

void Run(const std::filesystem::path& manifest, const std::filesystem::path& localClosure,
    uint32_t mode, const std::filesystem::path& destination, Json& report)
{
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto& description = store.Surface(SourceId);
    Check(description.width == 8192 && description.height == 8192 &&
        description.Layout().PageCount() == 5468, "Real tile00 pressure requires every R8192 mip0..13 page");
    auto closure = mode == 0 ? store.Production() :
        TerrainCompositedProductionFromJson(ReadJson(localClosure));
    ValidateWorld(closure, mode);
    Check(closure.adjacency.SurfaceIds() == store.Production().adjacency.SurfaceIds(),
        "Local pressure snapshot changes the real World surface set");
    report["inputProductionBlake3"] = closure.ContentHash().ToHex();
    report["baseManifestBlake3"] = ContentHasher::HashFile(manifest).ToHex();
    report["weightSource"] = mode;
    report["coldSamplesRetained"] = true;
    report["scope"] = "5468 unique real tile00 pages; same actual Baker Record; no Runtime publication test";
    VulkanManager::GetInstance().SetValidationEnabled(false);
    Check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    auto* window = SDL_CreateWindow("Real tile00 finite production", 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    Check(window != nullptr, SDL_GetError());
    {
        uint32_t extensionCount = 0;
        const auto extensionNames = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        Check(extensionNames != nullptr, SDL_GetError());
        RendererBackendVulkan backend;
        std::vector<const char*> extensions(extensionNames, extensionNames + extensionCount);
        backend.Initialize(extensions, window, PresentModePreference::Immediate);
        Check(backend.SupportsTimestampQueries(), "Actual GPU timestamps unavailable");
        TerrainRvtBudgetOwner budget(Caps());
        auto metadata = Charge(budget, {4 * MiB, 16 * MiB, 0, 4});
        const auto allocations = Gpu::MakeAllocationQuery(backend, budget,
            {{0, sizeof(vk::DescriptorImageInfo), 0, 0},
             {0, sizeof(Texture) + sizeof(TerrainCompositedProductionObject), 0, 0},
             {0, sizeof(vk::Buffer) + sizeof(vk::DeviceMemory), 0, 0},
             {0, sizeof(vk::ImageCreateInfo) + sizeof(vk::MemoryRequirements), 0, 0}});
        const auto reader = [&](const TerrainCompositedProductionObject& object) {
            if (object.objectId.find("pressure-local-weights-") != 0)
                return store.ReadInput(object.objectId);
            std::ifstream input(localClosure.parent_path() / object.relativePath, std::ios::binary);
            Check(bool(input), "Missing frozen real local weights");
            std::vector<uint8_t> bytes(size_t(object.bytes));
            input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
            Check(input.gcount() == std::streamsize(bytes.size()) &&
                ContentHasher::HashBytes(bytes.data(), bytes.size()) == object.digest,
                "Frozen local weight bytes changed");
            return bytes;
        };
        auto owner = Gpu::Restore(backend, closure, reader, budget, allocations);
        auto snapshot = std::make_shared<Baker::AdjacencySnapshot>();
        snapshot->graph = closure.adjacency;
        snapshot->sourceSurfaceId = SourceId;
        for (const auto& surface : closure.surfaces)
        {
            auto input = Gpu::MakeBakeInput(owner, surface.surfaceId).input;
            input.surfaceId = surface.surfaceId;
            snapshot->surfaces.push_back({surface.surfaceId, std::move(input)});
        }
        const uint64_t parameterBytes = 5 * (
            backend.QueryBufferMemoryRequirements(Baker::WorldParameterBytes,
                vk::BufferUsageFlagBits::eStorageBuffer).size +
            backend.QueryBufferMemoryRequirements(64 * 16, vk::BufferUsageFlagBits::eStorageBuffer).size);
        auto parameters = Charge(budget, {parameterBytes, parameterBytes, 0, 0});
        const auto limits = backend.GetSelectedDeviceLimits();
        auto factory = backend.CreatePipelineFactory();
        Baker baker(backend, *factory, 4, 16, &limits);
        Check(baker.Available(), baker.UnavailableReason());
        std::vector<std::unique_ptr<Task>> tasks;
        for (uint32_t slot = 0; slot < 4; ++slot)
            tasks.push_back(std::make_unique<Task>(backend, budget));
        const std::array<vk::Format, 3> formats{
            vk::Format::eR8G8B8A8Srgb, vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm};
        uint64_t atlasBytes = 0;
        for (const auto format : formats)
        {
            vk::ImageCreateInfo info({}, vk::ImageType::e2D, format, {136, 136, 1}, 1, 512,
                vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                vk::ImageUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
            atlasBytes += backend.QueryImageMemoryRequirements(info).size;
        }
        auto poolLease = Charge(budget, {atlasBytes, 0, 512, 0}, TerrainRvtCostClass::PhysicalPool);
        std::array<std::shared_ptr<Texture>, 3> atlas;
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            Texture::EmptyArrayDesc image;
            image.width = image.height = 136;
            image.arrayLayers = 512;
            image.format = formats[channel];
            image.debugName = "RealPressureWorldAtlas" + std::to_string(channel);
            atlas[channel] = Texture::CreateEmptyArray(backend, image);
        }
        Queries queries(backend);
        const double timestampScale = backend.GetTimestampPeriodNanoseconds() / 1.e6;
        std::ofstream raw(destination / "production-raw.jsonl");
        Check(bool(raw), "Cannot write raw real-pressure samples");
        uint32_t sequence = 0, overProduction = 0, overCopy = 0, overCombined = 0;
        double maximumProduction = 0, maximumCopy = 0, maximumCombined = 0;
        for (uint32_t mip = 0; mip < 14; ++mip)
        {
            const auto extent = description.Layout().Mip(mip);
            for (uint32_t pageY = 0; pageY < extent.pagesY; ++pageY)
                for (uint32_t pageX = 0; pageX < extent.pagesX; ++pageX)
                {
                    const uint32_t slot = sequence % 4;
                    auto& task = *tasks[slot];
                    Baker::PageRequest page{8192, 8192, mip, pageX, pageY, false, mip == 0 ? 1u : 4u};
                    const uint32_t first = slot * 7;
                    auto command = backend.BeginSingleTimeCommands();
                    command.resetQueryPool(queries.pool, first, 7);
                    for (auto& surface : snapshot->surfaces)
                    {
                        surface.input.diagnosticQueries = queries.pool;
                        surface.input.diagnosticFirstQuery = first + 4;
                    }
                    command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries.pool, first);
                    std::string error;
                    const bool recorded = baker.Record(command, slot, *snapshot, page,
                        {task.output.handle, 0, Baker::OutputBytes}, {}, error);
                    Check(recorded, "Actual real-input Record: " + error);
                    command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries.pool, first + 1);
                    command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries.pool, first + 2);
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        AtlasTransition(command, *atlas[channel], true);
                        command.copyBufferToImage(task.output.handle, atlas[channel]->getImage(),
                            vk::ImageLayout::eTransferDstOptimal,
                            vk::BufferImageCopy(channel * Baker::ChannelBytes, 0, 0,
                                {vk::ImageAspectFlagBits::eColor, 0, sequence % 512, 1},
                                {0, 0, 0}, {136, 136, 1}));
                        AtlasTransition(command, *atlas[channel], false);
                    }
                    command.copyBuffer(task.output.handle, task.status.handle,
                        vk::BufferCopy(Baker::StatusOffset, 0, 4));
                    const vk::MemoryBarrier host(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
                    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
                    command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries.pool, first + 3);
                    backend.EndSingleTimeCommands(command);
                    std::array<uint64_t, 2> production{}, copy{};
                    backend.ReadTimestampQueryPair(queries.pool, first, production);
                    backend.ReadTimestampQueryPair(queries.pool, first + 2, copy);
                    Check(production[1] > production[0] && copy[1] > copy[0],
                        "Missing, zero-duration or invalid GPU timestamp pair");
                    const double productionMs = (production[1] - production[0]) * timestampScale;
                    const double copyMs = (copy[1] - copy[0]) * timestampScale;
                    Check(std::isfinite(productionMs) && std::isfinite(copyMs), "Nonfinite GPU timestamp");
                    uint32_t status;
                    std::memcpy(&status, backend.MapMemory(task.status.memory, 4), 4);
                    backend.UnmapMemory(task.status.memory);
                    const Json row{{"sample", sequence}, {"weightSource", mode}, {"surfaceId", SourceId},
                        {"mip", mip}, {"x", pageX}, {"y", pageY}, {"slot", slot},
                        {"heightBlend", 3}, {"areaSamples", page.areaSamplesPerAxis},
                        {"productionMs", productionMs}, {"atlasCopyMs", copyMs},
                        {"combinedMs", productionMs + copyMs}, {"status", status},
                        {"productionTimestamps", production}, {"atlasCopyTimestamps", copy}};
                    raw << row.dump() << '\n';
                    raw.flush();
                    ++sequence;
                    maximumProduction = std::max(maximumProduction, productionMs);
                    maximumCopy = std::max(maximumCopy, copyMs);
                    maximumCombined = std::max(maximumCombined, productionMs + copyMs);
                    overProduction += productionMs > 4;
                    overCopy += copyMs > 4;
                    overCombined += productionMs + copyMs > 6;
                    report["samples"] = sequence;
                    report["lastPage"] = row;
                    Check(status == 0, "Actual real-input producer status is nonzero");
                }
        }
        Check(sequence == 5468, "Real-input pressure omitted unique pages");
        report["maximumProductionMs"] = maximumProduction;
        report["maximumAtlasCopyMs"] = maximumCopy;
        report["maximumCombinedMs"] = maximumCombined;
        report["overProduction4"] = overProduction;
        report["overCopy4"] = overCopy;
        report["overCombined6"] = overCombined;
        report["budget"] = BudgetStats(budget);
        report["status"] = overProduction || overCopy || overCombined ? "failed-performance" : "passed-mode";
        backend.WaitIdle();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
}
}

int main(int argc, char** argv)
{
    if (argc == 5 && std::string(argv[1]) == "--freeze-local")
    {
        try { FreezeLocal(argv[2], argv[3], std::filesystem::absolute(argv[4])); return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
    }
    if (argc != 6 || std::string(argv[1]) != "--run")
        return 2;
    const auto destination = std::filesystem::absolute(argv[5]);
    std::filesystem::create_directories(destination);
    Json report{{"gpuExecuted", true}, {"validationEnabled", false}};
    try
    {
        Run(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]),
            uint32_t(std::stoul(argv[4])), destination, report);
    }
    catch (const std::exception& error)
    {
        report["status"] = "incomplete";
        report["error"] = error.what();
    }
    std::ofstream(destination / "report.json") << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return report.value("status", "") == "passed-mode" ? 0 : 1;
}
