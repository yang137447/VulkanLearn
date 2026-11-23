#define main TerrainRealPressureArchivedEntry
#include "composited_real_pressure_check.cpp"
#undef main

namespace
{
std::vector<Baker::PageRequest> NumericRequests()
{
    std::vector<Baker::PageRequest> requests;
    for (uint32_t pageY : {0u, 31u, 63u})
        for (uint32_t pageX : {0u, 31u, 63u})
            requests.push_back({8192, 8192, 0, pageX, pageY, false, 1});
    requests.push_back({8192, 8192, 3, 3, 3, false, 4});
    requests.push_back({8192, 8192, 6, 0, 0, false, 4});
    requests.push_back({8192, 8192, 13, 0, 0, false, 4});
    return requests;
}

void RealNumeric(const std::filesystem::path& manifest, uint32_t mode,
    const std::filesystem::path& destination, Json& report)
{
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto& closure = store.Production();
    ValidateWorld(closure, mode);
    Check(closure.surfaces.size() == 16, "Numeric World must have exactly 16 surfaces");
    report["manifestBlake3"] = ContentHasher::HashFile(manifest).ToHex();
    report["productionBlake3"] = closure.ContentHash().ToHex();
    report["weightSource"] = mode;
    report["scope"] = "real published Full16; 12 distinct pages per surface; full three-channel bytes";
    report["performanceAcceptance"] = false;
    VulkanManager::GetInstance().SetValidationEnabled(true);
    Check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    auto* window = SDL_CreateWindow("Real Full16 numeric", 1280, 720,
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
        for (const auto& surface : closure.surfaces)
        {
            const auto& description = store.Surface(surface.surfaceId);
            Check(description.width == 8192 && description.height == 8192 &&
                description.addressMode == TerrainCompositedVTAddressMode::Clamp,
                "Full16 real numeric requires the published R8192 Clamp layout");
            auto input = Gpu::MakeBakeInput(owner, surface.surfaceId).input;
            input.surfaceId = surface.surfaceId;
            snapshot.surfaces.push_back({surface.surfaceId, std::move(input)});
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
        const auto requests = NumericRequests();
        uint32_t sequence = 0;
        report["pages"] = Json::array();
        for (const auto& surface : closure.surfaces)
        {
            snapshot.sourceSurfaceId = surface.surfaceId;
            for (const auto& page : requests)
            {
                const uint32_t slot = sequence % 4;
                auto command = backend.BeginSingleTimeCommands();
                std::string error;
                const bool recorded = baker.Record(command, slot, snapshot, page,
                    {outputs[slot]->handle, 0, Baker::OutputBytes}, {}, error);
                Check(recorded, "Real numeric Record: " + error);
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
                const auto filename = "page-" + std::to_string(sequence) + ".bin";
                std::ofstream binary(destination / filename, std::ios::binary);
                binary.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                binary.close();
                report["pages"].push_back({{"sample", sequence}, {"surfaceId", surface.surfaceId},
                    {"weightSource", mode}, {"mip", page.virtualMip}, {"x", page.pageX}, {"y", page.pageY},
                    {"areaSamples", page.areaSamplesPerAxis}, {"heightBlend", 3}, {"slot", slot},
                    {"status", status}, {"file", filename},
                    {"outputBlake3", ContentHasher::HashBytes(bytes.data(), bytes.size()).ToHex()}});
                std::ofstream(destination / "partial.json") << report.dump(2);
                Check(status == 0, "Real numeric producer/reference GPU status nonzero");
                ++sequence;
            }
        }
        Check(sequence == 192, "Real numeric missing surface/page coverage");
        report["pageCount"] = sequence;
        report["budget"] = BudgetStats(budget);
        report["diagnosticOracleBufferAccounting"] =
            "Independent CPU-address oracle SSBOs are diagnostic-only; not a Runtime workspace claim";
        report["status"] = "produced-real-numeric-pages";
        backend.WaitIdle();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return 2;
    const auto destination = std::filesystem::absolute(argv[3]);
    std::filesystem::create_directories(destination);
    Json report{{"gpuExecuted", true}, {"validationEnabled", true}};
    try
    {
        const auto mode = uint32_t(std::stoul(argv[2]));
        Check(mode <= 1, "Unknown real numeric weight mode");
        RealNumeric(std::filesystem::absolute(argv[1]), mode, destination, report);
    }
    catch (const std::exception& error)
    {
        report["status"] = "incomplete";
        report["error"] = error.what();
    }
    std::ofstream(destination / "report.json") << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return report.value("status", "") == "produced-real-numeric-pages" ? 0 : 1;
}
