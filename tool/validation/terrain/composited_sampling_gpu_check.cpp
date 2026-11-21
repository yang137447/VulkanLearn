#define main ExistingPageHarnessMain
#include "composited_page_gpu_check.cpp"
#undef main
#include "pipeline/computePipeline.h"
#include "pipeline/shaderReflectionService.h"

namespace
{
struct DiagnosticSurface
{
    std::array<uint32_t, 4> page{}, coverage{}, counts{};
    std::array<float, 4> surfaceUv{}, coverageUv{}, frameUv{}, material{};
    std::array<uint32_t, 4> adjacency{};
    std::array<float, 16> normalToSource{};
    std::array<uint32_t, 256> globalToSlice{}, localToGlobal{};
};
struct DiagnosticWorld
{
    std::array<uint32_t, 4> page{}, control{};
    std::array<DiagnosticSurface, 16> surfaces{};
};

void WriteHost(RendererBackendVulkan& backend, Buffer& buffer, const void* data)
{
    auto* mapped = backend.MapMemory(buffer.memory, buffer.bytes);
    std::memcpy(mapped, data, buffer.bytes);
    backend.UnmapMemory(buffer.memory);
}

Json SampleCase(RendererBackendVulkan& backend, PipelineFactory& factory,
    const std::filesystem::path& directory)
{
    TerrainCompositedAdjacencyGraph isolated({"repeat-tail"}, {});
    auto fixture = TerrainPageFixture::Build(backend, isolated, 0, 32, true);
    auto& surface = fixture.reference.at("repeat-tail");
    surface.input.surfaceUvOffset = {0, 0};
    surface.input.surfaceUvScale = {1, 1};
    surface.input.uvScale = 1;
    auto& albedo = surface.images.at("albedo");
    for (uint32_t layer = 0; layer < albedo.layers; ++layer)
        for (uint32_t mip = 0; mip < albedo.mips; ++mip)
        {
            const uint32_t width = std::max(1u, albedo.width >> mip);
            for (uint32_t pixel = 0; pixel < albedo.planes[layer * albedo.mips + mip].size(); ++pixel)
                albedo.planes[layer * albedo.mips + mip][pixel] =
                    {double(30 + (pixel % width) * 6) / 255, 0, 0, 1};
        }
    const auto texture = TerrainPageFixture::Upload(backend, fixture, albedo, false, true);
    const auto& textureOwner = fixture.owners.back();
    DiagnosticWorld world;
    world.surfaces[0].surfaceUv = {0, 0, 1, 1};
    world.surfaces[0].material[0] = 1;
    world.surfaces[0].globalToSlice = surface.input.globalToSourceSlice;
    Buffer parameters(backend, sizeof(world), vk::BufferUsageFlagBits::eStorageBuffer, true);
    WriteHost(backend, parameters, &world);
    std::vector<std::array<float, 4>> queries(1);
    std::vector<std::array<uint32_t, 3>> queryKeys;
    for (const auto pixel : {std::array<uint32_t, 2>{7, 7}, {10, 19}, {17, 46}, {29, 93}, {9, 68}, {10, 68}})
        for (uint32_t identity : {3u, 7u, 19u})
        {
            const float rawU = (float(128 + pixel[0] - 4) + .5f) / 129.f;
            const float rawV = (float(pixel[1] - 4) + .5f) / 129.f;
            queries.push_back({rawU - std::floor(rawU), rawV - std::floor(rawV), 0, float(identity)});
            queryKeys.push_back({pixel[0], pixel[1], identity});
        }
    const std::array<uint32_t, 4> header{uint32_t(queries.size() - 1), 0, 0, 0};
    std::memcpy(queries.data(), header.data(), sizeof(header));
    Buffer queryBuffer(backend, queries.size() * 16, vk::BufferUsageFlagBits::eStorageBuffer, true);
    WriteHost(backend, queryBuffer, queries.data());
    Buffer results(backend, queryKeys.size() * 9 * 16, vk::BufferUsageFlagBits::eStorageBuffer, true);
    auto pipeline = factory.CreateComputePipelineCandidate("terrain/terrainCompositedBakeSamplingDiagnostic");
    const std::array<vk::DescriptorPoolSize, 2> sizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 32)};
    auto pool = backend.CreateDescriptorPool(
        vk::DescriptorPoolCreateInfo({}, 1, sizes), "SamplingDiagnosticPool");
    std::vector<vk::DescriptorSet> sets(1);
    backend.AllocateDescriptorSets(vk::DescriptorSetAllocateInfo(pool,
        pipeline->GetDescriptorSetLayouts()[0]), sets);
    const std::array<vk::DescriptorBufferInfo, 3> buffers{
        vk::DescriptorBufferInfo(parameters.handle, 0, parameters.bytes),
        vk::DescriptorBufferInfo(queryBuffer.handle, 0, queryBuffer.bytes),
        vk::DescriptorBufferInfo(results.handle, 0, results.bytes)};
    Json report;
    for (uint32_t samplerCase = 0; samplerCase < 2; ++samplerCase)
    {
        vk::Sampler exactSampler{};
        auto sourceDescriptor = texture.descriptor;
        if (samplerCase == 1)
        {
            vk::SamplerCreateInfo sampler;
            sampler.setMagFilter(vk::Filter::eLinear).setMinFilter(vk::Filter::eLinear)
                .setMipmapMode(vk::SamplerMipmapMode::eLinear)
                .setAddressModeU(vk::SamplerAddressMode::eRepeat)
                .setAddressModeV(vk::SamplerAddressMode::eRepeat)
                .setAddressModeW(vk::SamplerAddressMode::eRepeat)
                .setAnisotropyEnable(false).setMinLod(0).setMaxLod(VK_LOD_CLAMP_NONE);
            exactSampler = backend.CreateSampler(sampler, "SamplingDiagnosticNoAnisotropy");
            sourceDescriptor.sampler = exactSampler;
        }
        std::array<std::array<vk::DescriptorImageInfo, 16>, 2> images;
        images[0].fill(sourceDescriptor);
        images[1].fill(surface.input.layerCatalog.descriptor);
        std::vector<vk::WriteDescriptorSet> writes(5);
        for (uint32_t binding = 0; binding < 5; ++binding)
        {
            writes[binding].setDstSet(sets[0]).setDstBinding(binding).setDescriptorCount(1);
            if (binding == 1 || binding == 2)
                writes[binding].setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setImageInfo(images[binding - 1]);
            else
                writes[binding].setDescriptorType(vk::DescriptorType::eStorageBuffer)
                    .setBufferInfo(buffers[binding == 0 ? 0 : binding - 2]);
        }
        backend.UpdateDescriptorSets(writes);
        auto command = backend.BeginSingleTimeCommands();
        pipeline->Bind(command);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            pipeline->GetPipelineLayout(), 0, sets[0], nullptr);
        command.dispatch(uint32_t((queryKeys.size() + 15) / 16), 1, 1);
        vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eHost, {}, host, nullptr, nullptr);
        backend.EndSingleTimeCommands(command);
        auto* mapped = static_cast<const std::array<float, 4>*>(
            backend.MapMemory(results.memory, results.bytes));
        const auto binary = directory / (samplerCase == 0 ? "original-sampler.bin" : "no-anisotropy.bin");
        std::ofstream file(binary, std::ios::binary);
        file.write(reinterpret_cast<const char*>(mapped), results.bytes);
        Json records = Json::array();
        for (uint32_t index = 0; index < queryKeys.size(); ++index)
        {
            const auto* values = mapped + index * 9;
            const auto identity = queryKeys[index][2];
            const auto extra = surface.images.at("catalog").Pixel(0, 0, identity, 1);
            const double scale = extra[2], angle = extra[3];
            const double domainU = queries[index + 1][0], domainV = queries[index + 1][1];
            const auto cpu = albedo.Sample(
                (std::cos(angle) * domainU - std::sin(angle) * domainV) * scale,
                (std::sin(angle) * domainU + std::cos(angle) * domainV) * scale,
                surface.input.globalToSourceSlice[identity], 0);
            records.push_back({{"stored", {queryKeys[index][0], queryKeys[index][1]}},
                {"globalLayer", identity}, {"uvSliceId", values[0]}, {"positionFraction", values[1]},
                {"textureLod", values[2]}, {"fetches", {values[3], values[4], values[5], values[6]}},
                {"manualBilinear", values[7]}, {"textureMinusManual", values[8]},
                {"independentDoubleSample", cpu}});
        }
        backend.UnmapMemory(results.memory);
        report["cases"].push_back({{"originalProductionSampler", samplerCase == 0}, {"queries", records}});
        if (exactSampler)
            backend.DestroySampler(exactSampler);
    }
    backend.DestroyDescriptorPool(pool);
    return report;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    Json report;
    try
    {
        Check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        auto* window = SDL_CreateWindow("Source sampling diagnostic", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        Check(window != nullptr, SDL_GetError());
        uint32_t count;
        const auto extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        std::vector<const char*> instanceExtensions(extensions, extensions + count);
        VulkanManager::GetInstance().SetValidationEnabled(true);
        {
            RendererBackendVulkan backend;
            backend.Initialize(instanceExtensions, window, PresentModePreference::Immediate);
            auto factory = backend.CreatePipelineFactory();
            report = SampleCase(backend, *factory, directory);
            backend.WaitIdle();
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
        report["status"] = "measured";
    }
    catch (const std::exception& exception)
    {
        report["status"] = "failed";
        report["error"] = exception.what();
    }
    std::ofstream(directory / "report.json") << report.dump(2);
    std::cout << report.dump(2) << "\n";
    return report["status"] == "measured" ? 0 : 1;
}
