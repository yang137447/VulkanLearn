#include "composited_page_fixture.h"
#include "pipeline/pipelineFactory.h"
#include "vulkanManager.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <fstream>
#include <iostream>

namespace
{
using namespace VL;
using Baker = TerrainCompositedBakeGpu;
using Json = nlohmann::json;

struct Buffer
{
    RendererBackendVulkan* backend;
    vk::Buffer handle;
    vk::DeviceMemory memory;
    uint64_t bytes;
    uint64_t allocation;
    Buffer(RendererBackendVulkan& owner, uint64_t size, vk::BufferUsageFlags usage, bool host = false)
        : backend(&owner), bytes(size)
    {
        if (size == 0)
        {
            allocation = 0;
            return;
        }
        allocation = owner.QueryBufferMemoryRequirements(size, usage).size;
        const auto created = owner.CreateBuffer(size, usage, host ?
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent :
            vk::MemoryPropertyFlagBits::eDeviceLocal, "ActualBakerOracleBuffer");
        handle = created.first;
        memory = created.second;
    }
    ~Buffer() { if (handle) backend->DestroyBuffer(handle, memory); }
};

struct Task
{
    Buffer output, scratch, readback;
    Task(RendererBackendVulkan& backend) :
        output(backend, Baker::OutputBytes, vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc),
        scratch(backend, Baker::RequiredScratchBytes(4),
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer),
        readback(backend, Baker::OutputBytes, vk::BufferUsageFlagBits::eTransferDst, true) {}
};

struct Recorded
{
    std::string source;
    Baker::PageRequest page;
    bool inject = false;
    bool attachWorld = true;
};

struct QueryPoolOwner
{
    RendererBackendVulkan& backend;
    vk::QueryPool handle;
    QueryPoolOwner(RendererBackendVulkan& owner, uint32_t count) : backend(owner),
        handle(owner.CreateTimestampQueryPool(count, "ActualBakerTimestamps")) {}
    ~QueryPoolOwner() { if (handle) backend.DestroyQueryPool(handle); }
};

void Check(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}

Json RunBatch(RendererBackendVulkan& backend, Baker& baker, TerrainPageFixture::Fixture& fixture,
    const std::vector<Recorded>& requests, bool compare, uint32_t iterations,
    const std::filesystem::path& output)
{
    std::vector<std::unique_ptr<Task>> tasks;
    for (size_t index = 0; index < requests.size(); ++index)
        tasks.push_back(std::make_unique<Task>(backend));
    std::array<std::shared_ptr<Texture>, 3> atlas;
    for (auto& image : atlas)
    {
        Texture::EmptyArrayDesc desc;
        desc.width = desc.height = 136;
        desc.arrayLayers = 4;
        desc.debugName = "ActualBakerOracleAtlas";
        image = Texture::CreateEmptyArray(backend, desc);
    }
    const uint32_t diagnosticStride = Baker::DiagnosticQueryCount(16);
    QueryPoolOwner queryOwner(backend, uint32_t(requests.size()) * (4 + diagnosticStride));
    const auto query = queryOwner.handle;
    Json report;
    report["scope"] = requests.size() == 1 ? "one-production-page-per-submission" :
        "diagnostic-only multi-task command; not a production scheduler setting";
    uint64_t workspace = 0;
    for (const auto& task : tasks)
        workspace += task->output.allocation + task->scratch.allocation + task->readback.allocation;
    report["queriedTaskAllocationBytes"] = workspace;
    std::vector<std::vector<uint8_t>> references;
    if (compare)
        for (const auto& request : requests)
            references.push_back(request.inject ? std::vector<uint8_t>{} : TerrainPageReference::Page(
                fixture.snapshot.graph, fixture.reference, request.source, request.page));
    for (uint32_t iteration = 0; iteration < iterations; ++iteration)
    {
        auto command = backend.BeginSingleTimeCommands();
        command.resetQueryPool(query, 0, uint32_t(requests.size()) * (4 + diagnosticStride));
        for (uint32_t index = 0; index < requests.size(); ++index)
        {
            auto snapshot = std::make_shared<Baker::AdjacencySnapshot>(fixture.snapshot);
            if (requests[index].inject)
                for (auto& surface : snapshot->surfaces)
                    surface.input.globalToSourceSlice[3] = Baker::InvalidSlice;
            const auto source = std::find_if(snapshot->surfaces.begin(), snapshot->surfaces.end(),
                [&](const auto& surface) { return surface.surfaceId == requests[index].source; });
            Check(source != snapshot->surfaces.end(), "Unknown batch surface");
            source->input.diagnosticQueries = query;
            source->input.diagnosticFirstQuery = uint32_t(requests.size()) * 4 + index * diagnosticStride;
            auto input = source->input;
            input.adjacency = snapshot;
            if (!requests[index].attachWorld)
                input.adjacency.reset();
            input.adjacencyScratch = vk::DescriptorBufferInfo(
                tasks[index]->scratch.handle, 0, tasks[index]->scratch.bytes);
            if (requests[index].inject)
                for (auto& surface : snapshot->surfaces)
                {
                    surface.input.weightSource = Baker::WeightSource::CoverageAtlas;
                    surface.input.retainedFragment.heightBlendStrength = 0;
                }
            command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, index * 4);
            std::string error;
            const bool recorded = baker.Record(command, index, input, requests[index].page,
                {tasks[index]->output.handle, 0, Baker::OutputBytes}, error);
            Check(recorded, "actual Record: " + error);
            command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, index * 4 + 1);
            command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, index * 4 + 2);
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, index, 1);
                vk::ImageMemoryBarrier writable(vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferWrite,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, atlas[channel]->getImage(), range);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                    vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, writable);
                const vk::BufferImageCopy copy(channel * Baker::ChannelBytes, 0, 0,
                    {vk::ImageAspectFlagBits::eColor, 0, index, 1}, {0, 0, 0}, {136, 136, 1});
                command.copyBufferToImage(tasks[index]->output.handle, atlas[channel]->getImage(),
                    vk::ImageLayout::eTransferDstOptimal, copy);
                vk::ImageMemoryBarrier readable(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, atlas[channel]->getImage(), range);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eAllCommands, {}, nullptr, nullptr, readable);
            }
            command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, index * 4 + 3);
        }
        for (uint32_t index = 0; index < requests.size(); ++index)
        {
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, index, 1);
                vk::ImageMemoryBarrier readable(vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eShaderRead,
                    vk::AccessFlagBits::eTransferRead, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::ImageLayout::eTransferSrcOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    atlas[channel]->getImage(), range);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                    vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, readable);
                command.copyImageToBuffer(atlas[channel]->getImage(), vk::ImageLayout::eTransferSrcOptimal,
                    tasks[index]->readback.handle, vk::BufferImageCopy(channel * Baker::ChannelBytes, 0, 0,
                        {vk::ImageAspectFlagBits::eColor, 0, index, 1}, {0, 0, 0}, {136, 136, 1}));
                readable.setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
                    .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eAllCommands, {}, nullptr, nullptr, readable);
            }
            command.copyBuffer(tasks[index]->output.handle, tasks[index]->readback.handle,
                vk::BufferCopy(Baker::StatusOffset, Baker::StatusOffset, 4));
        }
        vk::MemoryBarrier host(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eHost, {}, host, nullptr, nullptr);
        backend.EndSingleTimeCommands(command);
        for (uint32_t index = 0; index < requests.size(); ++index)
        {
            std::array<uint64_t, 2> production{}, copy{};
            backend.ReadTimestampQueryPair(query, index * 4, production);
            backend.ReadTimestampQueryPair(query, index * 4 + 2, copy);
            Check(production[1] >= production[0] && copy[1] >= copy[0], "Nonmonotonic timestamps");
            const double scale = backend.GetTimestampPeriodNanoseconds() / 1.e6;
            const double productionMs = (production[1] - production[0]) * scale;
            const double copyMs = (copy[1] - copy[0]) * scale;
            std::array<uint64_t, 2> evaluation{}, padding{};
            const uint32_t diagnostic = uint32_t(requests.size()) * 4 + index * diagnosticStride;
            backend.ReadTimestampQueryPair(query, diagnostic, evaluation);
            backend.ReadTimestampQueryPair(query, diagnostic + 1, padding);
            auto* bytes = static_cast<const uint8_t*>(backend.MapMemory(tasks[index]->readback.memory, Baker::OutputBytes));
            uint32_t status;
            std::memcpy(&status, bytes + Baker::StatusOffset, 4);
            std::array<uint32_t, 3> maximum{};
            uint64_t failures = 0;
            if (compare && !requests[index].inject)
                for (uint32_t offset = 0; offset < Baker::PackedBytes; ++offset)
                {
                    const uint32_t difference = uint32_t(std::abs(int(bytes[offset]) - references[index][offset]));
                    maximum[offset / Baker::ChannelBytes] =
                        std::max(maximum[offset / Baker::ChannelBytes], difference);
                    failures += difference > 1;
                }
            if (iteration == 0 && compare)
            {
                std::ofstream file(output / ("atlas-" + std::to_string(index) + ".bin"), std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes), Baker::OutputBytes);
                if (!requests[index].inject)
                {
                    std::ofstream reference(output / ("reference-" + std::to_string(index) + ".bin"), std::ios::binary);
                    reference.write(reinterpret_cast<const char*>(references[index].data()), Baker::PackedBytes);
                }
            }
            backend.UnmapMemory(tasks[index]->readback.memory);
            report["pages"].push_back({{"iteration", iteration}, {"slot", index},
                {"surface", requests[index].source}, {"mip", requests[index].page.virtualMip},
                {"areaSamples", requests[index].page.areaSamplesPerAxis},
                {"worldTableAttached", requests[index].attachWorld},
                {"repeatSurface", requests[index].page.repeatSurface},
                {"productionMs", productionMs}, {"atlasCopyMs", copyMs},
                {"singleRouteEvaluateEncodeMs", (evaluation[1] - evaluation[0]) * scale},
                {"canonicalPaddingMs", (padding[1] - padding[0]) * scale},
                {"combinedMs", productionMs + copyMs}, {"status", status},
                {"injected", requests[index].inject}, {"maxChannelByteErrors", maximum},
                {"bytesAboveTolerance", failures}, {"numericCompared", compare && !requests[index].inject},
                {"timingPassed", productionMs <= 4 && copyMs <= 4 && productionMs + copyMs <= 6}});
            std::ofstream(output / "partial.json") << report.dump(2);
            Check(requests[index].inject ? (status & 4u) != 0 : status == 0, "Unexpected actual Baker GPU status");
        }
    }
    return report;
}
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3)
        return 2;
    const std::filesystem::path destination = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(destination);
    Json report;
    try
    {
        Check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        auto* window = SDL_CreateWindow("Actual Baker page oracle", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        Check(window != nullptr, SDL_GetError());
        uint32_t count;
        const auto extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        Check(extensions != nullptr, SDL_GetError());
        std::vector<const char*> instanceExtensions(extensions, extensions + count);
        const bool validation = argc == 2;
        Check(argc != 3 || std::string(argv[2]) == "--validation-off", "Unknown diagnostic argument");
        VulkanManager::GetInstance().SetValidationEnabled(validation);
        report["validationEnabled"] = validation;
        report["coldSamplesRetained"] = true;
        {
            RendererBackendVulkan backend;
            backend.Initialize(instanceExtensions, window, PresentModePreference::Immediate);
            Check(backend.SupportsTimestampQueries(), "GPU timestamps unavailable");
            const vk::ApplicationInfo capabilityApplication(
                "Actual Baker physical limits query", 1, "VL diagnostic", 1, VK_API_VERSION_1_2);
            const auto capabilityInstance = vk::createInstance(
                vk::InstanceCreateInfo({}, &capabilityApplication));
            const auto capabilityProperties = capabilityInstance.enumeratePhysicalDevices().at(0).getProperties();
            capabilityInstance.destroy();
            report["physicalDeviceZeroLimitsQuery"] = std::string(capabilityProperties.deviceName);
            std::string limitError;
            Check(Baker::ValidateDeviceLimits(capabilityProperties.limits, limitError),
                "World baker device limits: " + limitError);
            auto factory = backend.CreatePipelineFactory();
            const auto beforeAdmission = backend.CaptureResourceIdentityCounts();
            {
                Baker missing(backend, *factory, 1, 1);
                Check(!missing.Available(), "Missing limits admitted a world baker");
                report["deviceAdmission"].push_back({{"case", "missing-selected-limits"},
                    {"rejected", true}, {"reason", missing.UnavailableReason()}});
            }
            for (uint32_t category = 0; category < 8; ++category)
            {
                auto limits = capabilityProperties.limits;
                switch (category)
                {
                case 0: limits.maxPerStageDescriptorSamplers = 175; break;
                case 1: limits.maxPerStageDescriptorSampledImages = 175; break;
                case 2: limits.maxDescriptorSetSamplers = 175; break;
                case 3: limits.maxDescriptorSetSampledImages = 175; break;
                case 4: limits.maxPerStageResources = 178; break;
                case 5: limits.maxPerStageDescriptorStorageBuffers = 2; break;
                case 6: limits.maxDescriptorSetStorageBuffers = 2; break;
                default: limits.maxStorageBufferRange = Baker::WorldParameterBytes - 1; break;
                }
                Baker denied(backend, *factory, 1, 1, &limits);
                Check(!denied.Available(), "Insufficient limits admitted a single-surface world baker");
                Check(denied.UnavailableReason().find("176") != std::string::npos,
                    "Device limit rejection lost required descriptor counts");
                Check(beforeAdmission == backend.CaptureResourceIdentityCounts(),
                    "Rejected baker allocated Vulkan resources before admission");
                report["deviceAdmission"].push_back({{"category", category}, {"surfaceCapacity", 1},
                    {"rejected", true}, {"reason", denied.UnavailableReason()},
                    {"resourceIdentityCountsUnchanged", true}});
            }
            Baker baker(backend, *factory, 4, 16, &capabilityProperties.limits);
            Check(baker.Available(), baker.UnavailableReason());
            auto graphs = TerrainBakeRouteCases::Graphs();
            auto ids = graphs[0].SurfaceIds();
            ids.erase(std::remove(ids.begin(), ids.end(), "disconnected"), ids.end());
            graphs[0] = TerrainCompositedAdjacencyGraph(ids, graphs[0].Links());
            for (uint32_t graphIndex = 0; graphIndex < graphs.size(); ++graphIndex)
            {
                auto fixture = TerrainPageFixture::Build(backend, graphs[graphIndex], 2, 32);
                const auto& sources = graphs[graphIndex].SurfaceIds();
                std::vector<Recorded> requests;
                Baker::PageRequest fine;
                fine.virtualWidth = fine.virtualHeight = 256;
                fine.areaSamplesPerAxis = 1;
                fine.pageX = 1;
                Baker::PageRequest coarse;
                coarse.virtualWidth = coarse.virtualHeight = 8192;
                coarse.virtualMip = 13;
                coarse.areaSamplesPerAxis = 4;
                requests.push_back({sources.front(), fine});
                requests.push_back({sources.back(), fine});
                requests.push_back({sources.front(), coarse});
                requests.push_back({sources.back(), coarse});
                const auto batchDirectory = destination / ("numeric-" + std::to_string(graphIndex));
                std::filesystem::create_directories(batchDirectory);
                report["numeric"].push_back(RunBatch(backend, baker, fixture, requests, true, 1, batchDirectory));
            }
            for (uint32_t mode = 0; mode < 3; ++mode)
            {
                auto fixture = TerrainPageFixture::Build(backend, graphs[0], mode, 32, true);
                for (uint32_t group = 0; group < 4; ++group)
                {
                    std::vector<Recorded> requests;
                    for (uint32_t index = 0; index < 4; ++index)
                    {
                        Baker::PageRequest page;
                        page.virtualWidth = page.virtualHeight = 259;
                        page.pageX = page.pageY = 2;
                        page.areaSamplesPerAxis = 1;
                        if (group % 2)
                        {
                            page.virtualMip = 2;
                            page.pageX = page.pageY = 0;
                            page.areaSamplesPerAxis = 4;
                        }
                        requests.push_back({graphs[0].SurfaceIds().at(group * 4 + index), page});
                    }
                    const auto directory = destination /
                        ("spatial-mode" + std::to_string(mode) + "-group" + std::to_string(group));
                    std::filesystem::create_directories(directory);
                    report["spatialNumeric"].push_back(RunBatch(
                        backend, baker, fixture, requests, true, 1, directory));
                }
            }
            {
                TerrainCompositedAdjacencyGraph isolated({"isolated"}, {});
                auto fixture = TerrainPageFixture::Build(backend, isolated, 1, 32, true);
                Baker::PageRequest clamp;
                clamp.virtualWidth = clamp.virtualHeight = 256;
                clamp.areaSamplesPerAxis = 1;
                auto repeat = clamp;
                repeat.repeatSurface = true;
                const auto directory = destination / "singleton-clamp-repeat";
                std::filesystem::create_directories(directory);
                report["singletonNumeric"] = RunBatch(backend, baker, fixture,
                    {{"isolated", clamp}, {"isolated", clamp, false, false},
                     {"isolated", repeat}, {"isolated", repeat, false, false}}, true, 1, directory);
            }
            {
                TerrainCompositedAdjacencyGraph isolated({"repeat-tail"}, {});
                auto fixture = TerrainPageFixture::Build(backend, isolated, 0, 32, true);
                auto& surface = fixture.reference.at("repeat-tail");
                auto& input = surface.input;
                input.surfaceUvOffset = {0, 0};
                input.surfaceUvScale = {1, 1};
                input.uvScale = 1;
                input.retainedFragment.heightBlendStrength = 0;
                auto& albedo = surface.images.at("albedo");
                for (uint32_t layer = 0; layer < albedo.layers; ++layer)
                    for (uint32_t mip = 0; mip < albedo.mips; ++mip)
                    {
                        const uint32_t width = std::max(1u, albedo.width >> mip);
                        for (uint32_t pixel = 0; pixel < albedo.planes[layer * albedo.mips + mip].size(); ++pixel)
                            albedo.planes[layer * albedo.mips + mip][pixel] =
                                {double(30 + (pixel % width) * 6) / 255, 0, 0, 1};
                    }
                input.layerAlbedo = TerrainPageFixture::Upload(backend, fixture, albedo, false, true);
                fixture.snapshot.surfaces[0].input = input;
                Baker::PageRequest low;
                low.virtualWidth = low.virtualHeight = 129;
                low.pageX = 1;
                low.repeatSurface = true;
                low.areaSamplesPerAxis = 1;
                auto high = low;
                high.virtualMip = 1;
                high.pageX = 0;
                high.areaSamplesPerAxis = 4;
                const auto directory = destination / "repeat-129-tail-consumer";
                std::filesystem::create_directories(directory);
                report["repeatTailNumeric"] = RunBatch(backend, baker, fixture,
                    {{"repeat-tail", low}, {"repeat-tail", high}}, true, 1, directory);
                std::ifstream stream(directory / "atlas-0.bin", std::ios::binary);
                std::vector<uint8_t> bytes(Baker::OutputBytes);
                stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                Check(bool(stream), "Repeat countercase atlas readback missing");
                uint32_t maximum = 0;
                for (uint32_t column : {9u, 10u})
                {
                    const double horizontal = (128. + column - 4 + .5) / 129;
                    const auto direct = TerrainPageReference::Evaluate(surface,
                        {horizontal - std::floor(horizontal), 64.5 / 129},
                        {1, 0, 0, 1}, {1. / 129, 1. / 129});
                    const uint32_t offset = (68 * 136 + column) * 4;
                    for (uint32_t component = 0; component < 3; ++component)
                    {
                        const int expected = int(std::lround(
                            255 * TerrainPageReference::SrgbEncode(direct.colour[component])));
                        maximum = std::max(maximum, uint32_t(std::abs(int(bytes[offset + component]) - expected)));
                    }
                }
                Check(maximum <= 1, "Consumer-reachable Repeat stored9/10 differ from uncropped direct evaluator");
                Check(bytes[(68 * 136 + 9) * 4] != bytes[(68 * 136 + 8) * 4] ||
                    bytes[(68 * 136 + 10) * 4] != bytes[(68 * 136 + 8) * 4],
                    "Repeat countercase failed to distinguish prior replicated texel8");
                report["repeatTailConsumer"] = {{"targetMip", std::log2(1.5)},
                    {"tapCount", 8}, {"lastTapLocalX", 6}, {"storedX", {9, 10}},
                    {"nonzeroWeights", {.5, .5}}, {"row", 68},
                    {"directUncroppedEvaluatorMaxByteError", maximum},
                    {"priorReplicatedEightDistinguished", true}};
            }
            {
                auto fixture = TerrainPageFixture::Build(backend, graphs[0], 2, 256);
                Baker::PageRequest coarse;
                coarse.virtualWidth = coarse.virtualHeight = 8192;
                coarse.virtualMip = 13;
                coarse.areaSamplesPerAxis = 4;
                const auto directory = destination / "pressure";
                std::filesystem::create_directories(directory);
                report["pressure"] = RunBatch(backend, baker, fixture,
                    {{graphs[0].SurfaceIds().at(11), coarse}}, false, 64, directory);
                auto failed = coarse;
                const auto failureDirectory = destination / "status-injection";
                std::filesystem::create_directories(failureDirectory);
                report["statusInjection"] = RunBatch(backend, baker, fixture,
                    {{graphs[0].SurfaceIds().front(), failed, true}}, false, 1, failureDirectory);
            }
            for (uint32_t mode = 0; mode < 3; ++mode)
            {
                auto fixture = TerrainPageFixture::Build(backend, graphs[0], mode, 256);
                for (uint32_t mip : {0u, 6u, 13u})
                {
                    Baker::PageRequest page;
                    page.virtualWidth = page.virtualHeight = 8192;
                    page.virtualMip = mip;
                    page.areaSamplesPerAxis = mip == 0 ? 1 : 4;
                    const auto directory = destination /
                        ("pressure-mode" + std::to_string(mode) + "-mip" + std::to_string(mip));
                    std::filesystem::create_directories(directory);
                    report["pressureModes"].push_back(RunBatch(backend, baker, fixture,
                        {{graphs[0].SurfaceIds().at(11), page}}, false, 32, directory));
                }
            }
            backend.WaitIdle();
            report["status"] = "passed";
            bool numericPassed = true, performancePassed = true;
            const auto inspectTimes = [&](const Json& batch) {
                for (const auto& page : batch["pages"])
                {
                    if (!page["timingPassed"].get<bool>())
                        performancePassed = false;
                    if (page["bytesAboveTolerance"].get<uint64_t>() != 0)
                        numericPassed = false;
                }
            };
            inspectTimes(report["pressure"]);
            inspectTimes(report["singletonNumeric"]);
            inspectTimes(report["repeatTailNumeric"]);
            for (const auto key : {"numeric", "spatialNumeric", "pressureModes"})
                for (const auto& batch : report[key])
                    inspectTimes(batch);
            report["numericPassed"] = numericPassed;
            report["performancePassed"] = performancePassed;
            report["status"] = numericPassed
                ? (performancePassed ? "passed" : "failed-performance")
                : (performancePassed ? "failed-numeric" : "failed-numeric-and-performance");
            report["limitations"] = {"Synthetic source fixtures, not complete Full16 production input",
                "Finite pressure 352 pages plus 70 numeric pages; does not replace frozen 10936-page scope",
                "Multi-task command only tests slot isolation; production remains one page per frame",
                "Status diagnostic checked after fence; actual Service publication requires separate integration"};
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    catch (const std::exception& exception)
    {
        report["status"] = "failed";
        report["error"] = exception.what();
    }
    std::ofstream(destination / "report.json") << report.dump(2);
    std::cout << report.dump(2) << "\n";
    return report["status"] == "passed" ? 0 : 1;
}
