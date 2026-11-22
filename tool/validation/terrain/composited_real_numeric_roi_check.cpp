// ROI-request-driven original-evaluator page producer (CPU-authored plan artefact).
//
// Same independent original evaluator and GPU page baker as
// composited_real_numeric_check.cpp, but the (surface, mip, x, y) request list is
// read from JSON instead of being hardcoded, so the exact pages the lit ROI
// actually samples can be baked without guessing.
//
// argv: <publication manifest.json> <weight mode> <requests.json> <destination>
//
// requests.json: {"requests":[{"surfaceId":"terrain|Landscape_00","mip":1,"x":31,"y":31}, ...]}
//
// NOT COMPILED OR EXECUTED YET: building it and running it needs the GPU window
// that this stage does not own. See page-driver/build-plan.json for the exact
// command produced by prepare_reference_page_driver.py.

#define main TerrainRealPressureArchivedEntry
#include "composited_real_pressure_check.cpp"
#undef main

namespace
{
struct RoiRequest
{
    std::string surfaceId;
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;
};

std::vector<RoiRequest> ReadRoiRequests(const std::filesystem::path& path)
{
    const Json document = ReadJson(path);
    Check(document.contains("requests") && document["requests"].is_array() &&
        !document["requests"].empty() && document["requests"].size() <= 4096,
        "ROI page requests must be a bounded non-empty array");
    std::vector<RoiRequest> requests;
    for (const auto& row : document["requests"])
    {
        RoiRequest request;
        request.surfaceId = row.at("surfaceId").get<std::string>();
        request.mip = row.at("mip").get<uint32_t>();
        request.x = row.at("x").get<uint32_t>();
        request.y = row.at("y").get<uint32_t>();
        requests.push_back(std::move(request));
    }
    return requests;
}

std::string RequestFileName(uint32_t surfaceIndex, const RoiRequest& request)
{
    return "page-surface" + std::to_string(surfaceIndex) + "-mip" + std::to_string(request.mip) +
        "-" + std::to_string(request.x) + "-" + std::to_string(request.y) + ".bin";
}

void RoiPages(const std::filesystem::path& manifest, uint32_t mode,
    const std::filesystem::path& requestPath, const std::filesystem::path& destination, Json& report)
{
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto& closure = store.Production();
    ValidateWorld(closure, mode);
    Check(closure.surfaces.size() == 16, "ROI page production requires the real 16-surface World");
    const auto requests = ReadRoiRequests(requestPath);
    report["manifestBlake3"] = ContentHasher::HashFile(manifest).ToHex();
    report["productionsBlake3"] = closure.ContentHash().ToHex();
    report["weightSource"] = mode;
    report["scope"] = "Real published Full16 pages at the exact (surface, mip, x, y) the ROI samples";
    report["performanceAcceptance"] = false;
    report["gpuExecuted"] = true;
    VulkanManager::GetInstance().SetValidationEnabled(true);
    Check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    auto* window = SDL_CreateWindow("ROI original pages", 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    Check(window != nullptr, SDL_GetError());
    {
        uint32_t extensionCount = 0;
        const auto names = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        Check(names != nullptr, SDL_GetError());
        std::vector<const char*> extensions(names, names + extensionCount);
        RendererBackendVulkan backend;
        backend.Initialize(extensions, window, PresentModePreference::Immediate);
        TerrainRvtBudgetOwner budget(Caps());
        auto metadata = Charge(budget, {4 * MiB, 16 * MiB, 0, 4});
        const auto allocations = Gpu::MakeAllocationQuery(backend, budget,
            {{0, sizeof(vk::DescriptorImageInfo), 0, 0},
             {0, sizeof(Texture) + sizeof(TerrainCompositedProductionObject), 0, 0},
             {0, sizeof(vk::Buffer) + sizeof(vk::DeviceMemory), 0, 0},
             {0, sizeof(vk::ImageCreateInfo) + sizeof(vk::MemoryRequirements), 0, 0}});
        auto owner = Gpu::Restore(backend, store, budget, allocations);
        Baker::AdjacencySnapshot snapshot;
        snapshot.graph = closure.adjacency;
        std::map<std::string, uint32_t> surfaceIndex;
        for (const auto& surface : closure.surfaces)
        {
            const auto& description = store.Surface(surface.surfaceId);
            Check(description.width == 8192 && description.height == 8192 &&
                description.addressMode == TerrainCompositedVTAddressMode::Clamp,
                "ROI page production requires the published R8192 Clamp layout");
            auto input = Gpu::MakeBakeInput(owner, surface.surfaceId).input;
            input.surfaceId = surface.surfaceId;
            snapshot.surfaces.push_back({surface.surfaceId, std::move(input)});
            surfaceIndex[surface.surfaceId] = uint32_t(snapshot.surfaces.size() - 1);
        }
        const uint64_t parameterBytes = 5 * (
            backend.QueryBufferMemoryRequirements(Baker::WorldParameterBytes,
                vk::BufferUsageFlagBits::eStorageBuffer).size +
            backend.QueryBufferMemoryRequirements(64 * 16,
                vk::BufferUsageFlagBits::eStorageBuffer).size);
        auto parameters = Charge(budget, {parameterBytes, parameterBytes, 0, 0});
        const auto limits = backend.GetSelectedDeviceLimits();
        auto factory = backend.CreatePipelineFactory();
        Baker baker(backend, *factory, 4, 16, &limits);
        Check(baker.Available(), baker.UnavailableReason());
        std::vector<std::unique_ptr<Buffer>> outputs;
        for (uint32_t slot = 0; slot < 4; ++slot)
            outputs.push_back(std::make_unique<Buffer>(backend, budget, Baker::OutputBytes,
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst));
        Buffer readback(backend, budget, Baker::OutputBytes, vk::BufferUsageFlagBits::eTransferDst, true);
        uint32_t sequence = 0;
        report["pages"] = Json::array();
        for (const auto& request : requests)
        {
            const auto found = surfaceIndex.find(request.surfaceId);
            Check(found != surfaceIndex.end(), "ROI request names an unpublished surface");
            snapshot.sourceSurfaceId = request.surfaceId;
            Baker::PageRequest page{8192, 8192, request.mip, request.x, request.y, false, 4};
            const uint32_t slot = sequence % 4;
            auto command = backend.BeginSingleTimeCommands();
            std::string error;
            const bool recorded = baker.Record(command, slot, snapshot, page,
                {outputs[slot]->handle, 0, Baker::OutputBytes}, {}, error);
            Check(recorded, "ROI page Record: " + error);
            command.copyBuffer(outputs[slot]->handle, readback.handle,
                vk::BufferCopy(0, 0, Baker::OutputBytes));
            const vk::MemoryBarrier host(vk::AccessFlagBits::eTransferWrite,
                vk::AccessFlagBits::eHostRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
            backend.EndSingleTimeCommands(command);
            std::vector<uint8_t> bytes(Baker::OutputBytes);
            std::memcpy(bytes.data(), backend.MapMemory(readback.memory, bytes.size()), bytes.size());
            backend.UnmapMemory(readback.memory);
            uint32_t status;
            std::memcpy(&status, bytes.data() + Baker::StatusOffset, 4);
            const auto filename = RequestFileName(found->second, request);
            std::ofstream binary(destination / filename, std::ios::binary);
            binary.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            binary.close();
            report["pages"].push_back({{"sample", sequence}, {"surfaceIndex", found->second},
                {"surfaceId", request.surfaceId}, {"weightSource", mode}, {"mip", request.mip},
                {"x", request.x}, {"y", request.y}, {"slot", slot}, {"status", status},
                {"file", filename},
                {"outputBlake3", ContentHasher::HashBytes(bytes.data(), bytes.size()).ToHex()}});
            std::ofstream(destination / "partial.json") << report.dump(2);
            Check(status == 0, "ROI page producer GPU status nonzero");
            ++sequence;
        }
        Check(sequence == requests.size(), "ROI page production is incomplete");
        report["pageCount"] = sequence;
        report["budget"] = BudgetStats(budget);
        report["status"] = "produced-roi-original-pages";
        backend.WaitIdle();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
}
}

int main(int argc, char** argv)
{
    if (argc != 5)
        return 2;
    const auto destination = std::filesystem::absolute(argv[4]);
    std::filesystem::create_directories(destination);
    Json report{{"gpuExecuted", true}, {"validationEnabled", true}};
    try
    {
        const auto mode = uint32_t(std::stoul(argv[2]));
        Check(mode <= 1, "Unknown real numeric weight mode");
        RoiPages(std::filesystem::absolute(argv[1]), mode, std::filesystem::absolute(argv[3]),
            destination, report);
    }
    catch (const std::exception& error)
    {
        report["status"] = "incomplete";
        report["error"] = error.what();
    }
    std::ofstream(destination / "report.json") << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return report.value("status", "") == "produced-roi-original-pages" ? 0 : 1;
}
