#include <vulkan/vulkan.hpp>
#include "composited_adjacency_cases.h"
#include <cstring>
#include <fstream>
#include <iostream>

namespace
{
struct Buffer
{
    vk::Buffer handle;
    vk::DeviceMemory memory;
    uint64_t bytes = 0;
};

struct ComputeOracle
{
    vk::Instance instance;
    vk::Device device;
    vk::PhysicalDevice physical;
    vk::Queue queue;
    vk::CommandPool commands;
    vk::DescriptorSetLayout descriptors;
    vk::PipelineLayout layout;
    std::vector<Buffer> buffers;
    uint64_t allocatedBytes = 0;

    ComputeOracle()
    {
        vk::ApplicationInfo application("Terrain route oracle", 1, "VL validation", 1, VK_API_VERSION_1_2);
        instance = vk::createInstance(vk::InstanceCreateInfo({}, &application));
        physical = instance.enumeratePhysicalDevices().at(0);
        const auto families = physical.getQueueFamilyProperties();
        uint32_t family = 0;
        while (!(families.at(family).queueFlags & vk::QueueFlagBits::eCompute))
            ++family;
        const float priority = 1;
        const vk::DeviceQueueCreateInfo queueInfo({}, family, 1, &priority);
        device = physical.createDevice(vk::DeviceCreateInfo({}, queueInfo));
        queue = device.getQueue(family, 0);
        commands = device.createCommandPool(vk::CommandPoolCreateInfo({}, family));
        std::array<vk::DescriptorSetLayoutBinding, 4> bindings;
        for (uint32_t index = 0; index < 4; ++index)
            bindings[index] = vk::DescriptorSetLayoutBinding(index,
                vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute);
        descriptors = device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo({}, bindings));
        layout = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, descriptors));
        std::cout << "device=" << physical.getProperties().deviceName << "\n";
    }

    ~ComputeOracle()
    {
        if (device)
        {
            device.waitIdle();
            Clear();
            device.destroyPipelineLayout(layout);
            device.destroyDescriptorSetLayout(descriptors);
            device.destroyCommandPool(commands);
            device.destroy();
        }
        if (instance)
            instance.destroy();
    }

    void Clear()
    {
        for (const auto& buffer : buffers)
        {
            device.destroyBuffer(buffer.handle);
            device.freeMemory(buffer.memory);
        }
        buffers.clear();
        allocatedBytes = 0;
    }

    Buffer MakeBuffer(uint64_t bytes, const void* data)
    {
        Buffer buffer;
        buffer.bytes = bytes;
        buffer.handle = device.createBuffer(vk::BufferCreateInfo({}, bytes,
            vk::BufferUsageFlagBits::eStorageBuffer));
        const auto requirements = device.getBufferMemoryRequirements(buffer.handle);
        const auto properties = physical.getMemoryProperties();
        uint32_t memoryType = 0;
        const auto flags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        while (memoryType < properties.memoryTypeCount &&
            (!(requirements.memoryTypeBits & (1u << memoryType)) ||
                (properties.memoryTypes[memoryType].propertyFlags & flags) != flags))
            ++memoryType;
        if (memoryType == properties.memoryTypeCount)
            throw std::runtime_error("Oracle requires coherent host-visible storage memory");
        buffer.memory = device.allocateMemory(vk::MemoryAllocateInfo(requirements.size, memoryType));
        device.bindBufferMemory(buffer.handle, buffer.memory, 0);
        allocatedBytes += requirements.size;
        buffers.push_back(buffer);
        if (data)
        {
            auto mapped = device.mapMemory(buffer.memory, 0, bytes);
            std::memcpy(mapped, data, bytes);
            device.unmapMemory(buffer.memory);
        }
        return buffer;
    }

    vk::Pipeline Pipeline(const char* path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() <= 0 || size_t(stream.tellg()) % 4)
            throw std::runtime_error("Invalid oracle SPIR-V");
        std::vector<uint32_t> words(size_t(stream.tellg()) / 4);
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(words.data()), words.size() * 4);
        const auto module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, words));
        const auto result = device.createComputePipeline({},
            vk::ComputePipelineCreateInfo({},
                vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, module, "main"), layout));
        device.destroyShaderModule(module);
        return result.value;
    }

    void Dispatch(vk::Pipeline pipeline, const std::array<Buffer, 4>& resources, uint32_t groupsX,
        uint32_t groupsY = 1)
    {
        const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer, 4);
        const auto pool = device.createDescriptorPool(vk::DescriptorPoolCreateInfo({}, 1, size));
        const auto set = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(pool, descriptors)).at(0);
        std::array<vk::DescriptorBufferInfo, 4> infos;
        std::array<vk::WriteDescriptorSet, 4> writes;
        for (uint32_t index = 0; index < 4; ++index)
        {
            infos[index] = vk::DescriptorBufferInfo(resources[index].handle, 0, resources[index].bytes);
            writes[index].setDstSet(set).setDstBinding(index).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(infos[index]);
        }
        device.updateDescriptorSets(writes, nullptr);
        const auto command = device.allocateCommandBuffers(
            vk::CommandBufferAllocateInfo(commands, vk::CommandBufferLevel::ePrimary, 1)).at(0);
        command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, set, nullptr);
        command.dispatch(groupsX, groupsY, 1);
        vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eHost, {}, host, nullptr, nullptr);
        command.end();
        const auto fence = device.createFence({});
        queue.submit(vk::SubmitInfo({}, {}, command), fence);
        if (device.waitForFences(fence, true, 60000000000ull) != vk::Result::eSuccess)
            throw std::runtime_error("Oracle fence timed out");
        device.destroyFence(fence);
        device.freeCommandBuffers(commands, command);
        device.destroyDescriptorPool(pool);
    }
};
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: composited_adjacency_gpu_check route-oracle.spv production-address.spv\n";
        return 2;
    }
    try
    {
        ComputeOracle gpu;
        const auto routePipeline = gpu.Pipeline(argv[1]);
        const auto addressPipeline = gpu.Pipeline(argv[2]);
        const auto queries = TerrainBakeRouteCases::Queries();
        uint64_t routeChecks = 0, addressChecks = 0, maximumAllocation = 0;
        for (const auto& graph : TerrainBakeRouteCases::Graphs())
            for (const auto& source : graph.SurfaceIds())
            {
                const auto charts = VL::TerrainBakeAdjacency::Prepare(graph, source);
                std::vector<VL::TerrainBakeAdjacency::PackedChart> packed;
                uint32_t sourceIndex = 0;
                for (uint32_t index = 0; index < charts.size(); ++index)
                {
                    packed.push_back(VL::TerrainBakeAdjacency::Pack(charts[index]));
                    if (charts[index].surfaceId == source)
                        sourceIndex = index;
                }
                std::vector<std::array<float, 4>> queryPayload(queries.size() + 1);
                const bool rectangular = VL::TerrainBakeAdjacency::RectangularBounds(charts, packed);
                const std::array<uint32_t, 4> header{
                    uint32_t(queries.size()), sourceIndex, uint32_t(charts.size()), rectangular ? 2u : 1u};
                std::memcpy(queryPayload.data(), header.data(), 16);
                for (size_t index = 0; index < queries.size(); ++index)
                    queryPayload[index + 1] = {queries[index][0], queries[index][1], 0, 0};
                const auto params = gpu.MakeBuffer(queryPayload.size() * 16, queryPayload.data());
                const auto output = gpu.MakeBuffer(queries.size() * 4, nullptr);
                const auto chartBuffer = gpu.MakeBuffer(packed.size() * 64, packed.data());
                gpu.Dispatch(routePipeline, {params, output, chartBuffer, output},
                    uint32_t((queries.size() + 63) / 64));
                const auto* targets = static_cast<const uint32_t*>(gpu.device.mapMemory(
                    output.memory, 0, output.bytes));
                for (size_t index = 0; index < queries.size(); ++index)
                {
                    const auto expected = graph.Resolve(source, {queries[index][0], queries[index][1]});
                    if (targets[index] >= charts.size() || charts[targets[index]].surfaceId != expected.surfaceId)
                        throw std::runtime_error("GPU route differs from double Resolve: " + source +
                            " query " + std::to_string(index));
                    ++routeChecks;
                }
                gpu.device.unmapMemory(output.memory);
                maximumAllocation = std::max(maximumAllocation, gpu.allocatedBytes);
                gpu.Clear();
                if (source == graph.SurfaceIds().front() ||
                    (graph.SurfaceIds().size() > 4 && source == "tile5"))
                {
                    for (uint32_t samples : {4u, 8u})
                    {
                        std::array<uint32_t, 560> addressParams{};
                        addressParams[0] = addressParams[1] = 1;
                        addressParams[10] = samples;
                        addressParams[28] = rectangular ? 2 : 1;
                        addressParams[30] = uint32_t(charts.size());
                        addressParams[31] = sourceIndex;
                        const uint32_t routeVectors = (samples * samples + 15) / 16;
                        const uint32_t texels = 136 * 136;
                        const auto input = gpu.MakeBuffer(sizeof(addressParams), addressParams.data());
                        const auto scratch = gpu.MakeBuffer(texels * (48 + routeVectors * 16), nullptr);
                        const auto chartsGpu = gpu.MakeBuffer(packed.size() * 64, packed.data());
                        std::array<uint32_t, 2048> bounds{};
                        for (uint32_t target = 0; target < 256; ++target)
                            bounds[target * 4] = bounds[target * 4 + 1] = 136;
                        const auto work = gpu.MakeBuffer(sizeof(bounds), bounds.data());
                        gpu.Dispatch(addressPipeline, {input, scratch, chartsGpu, work}, 2, 2);
                        const auto* words = static_cast<const uint32_t*>(
                            gpu.device.mapMemory(scratch.memory, 0, scratch.bytes));
                        for (uint32_t pixel = 0; pixel < texels; ++pixel)
                            for (uint32_t sample = 0; sample < samples * samples; ++sample)
                            {
                                if (pixel % 136 >= 9 || pixel / 136 >= 9)
                                    continue;
                                const float horizontal = float(pixel % 136) - 4 +
                                    (float(sample % samples) + .5f) / samples;
                                const float vertical = float(pixel / 136) - 4 +
                                    (float(sample / samples) + .5f) / samples;
                                const uint32_t word = words[texels * 12 + pixel * routeVectors * 4 + sample / 4];
                                const uint32_t target = (word >> ((sample % 4) * 8)) & 255u;
                                const auto expected = graph.Resolve(source, {horizontal, vertical});
                                if (target >= charts.size() || charts[target].surfaceId != expected.surfaceId)
                                    throw std::runtime_error("Production packed route differs: " + source);
                                ++addressChecks;
                            }
                        gpu.device.unmapMemory(scratch.memory);
                        maximumAllocation = std::max(maximumAllocation, gpu.allocatedBytes);
                        gpu.Clear();
                    }
                }
            }
        gpu.device.destroyPipeline(routePipeline);
        gpu.device.destroyPipeline(addressPipeline);
        std::cout << "{\"routeChecks\":" << routeChecks << ",\"packedAddressChecks\":"
            << addressChecks << ",\"maximumOracleBufferAllocationBytes\":" << maximumAllocation
            << ",\"mismatches\":0,\"performanceMeasurement\":false}\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
