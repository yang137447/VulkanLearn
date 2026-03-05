#include "pipelineFactory.h"
#include "computePipeline.h"
#include "graphicsPipeline.h"
#include <cstdint>

size_t GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const
{
    const size_t renderPassHash = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(key.renderPass));
    const size_t shaderNameHash = std::hash<std::string>{}(key.shaderName);
    const size_t sampleCountHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.sampleCount));
    const size_t postProcessHash = std::hash<bool>{}(key.bIsPostProcess);
    const size_t shadowPassHash = std::hash<bool>{}(key.bIsShadowPass);

    size_t hash = renderPassHash;
    hash ^= shaderNameHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= sampleCountHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= postProcessHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= shadowPassHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
}

PipelineFactory::PipelineFactory(vk::Device* device)
{
    this->device = device;
}

std::shared_ptr<ComputePipeline> PipelineFactory::CreateComputePipeline(const std::string& shaderName)
{
    auto it = computePipelines.find(shaderName);
    if (it != computePipelines.end())
    {
        auto pipeline = it->second.lock();
        if (pipeline)
        {
            return pipeline;
        }
    }

    auto pipeline = std::make_shared<ComputePipeline>(device, shaderName);
    computePipelines[shaderName] = pipeline;
    return pipeline;
}

std::shared_ptr<GraphicsPipeline> PipelineFactory::CreateGraphicsPipeline(
    vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties,
    vk::RenderPass* renderPass,
    const std::string& shaderName,
    vk::SampleCountFlagBits sampleCount,
    bool bIsPostProcess,
    bool bIsShadowPass)
{
    GraphicsPipelineKey key{
        renderPass,
        shaderName,
        sampleCount,
        bIsPostProcess,
        bIsShadowPass
    };
    auto it = graphicsPipelines.find(key);
    if (it != graphicsPipelines.end())
    {
        auto pipeline = it->second.lock();
        if (pipeline)
        {
            return pipeline;
        }
    }

    auto pipeline = std::make_shared<GraphicsPipeline>(device, gpuMemoryProperties, renderPass, shaderName, sampleCount, bIsPostProcess, bIsShadowPass);
    graphicsPipelines[key] = pipeline;
    return pipeline;
}
