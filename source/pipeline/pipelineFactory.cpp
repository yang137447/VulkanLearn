#include "pipelineFactory.h"
#include "computePipeline.h"
#include "graphicsPipeline.h"
#include <cstdint>

size_t GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const
{
    const size_t renderPassHash = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(key.renderPass));
    const size_t shaderVariantHash = ShaderVariantKeyHash{}(key.shaderVariantKey);
    const size_t sampleCountHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.sampleCount));
    const size_t useVertexInputHash = std::hash<bool>{}(key.pipelineStateDesc.bUseVertexInput);
    const size_t depthTestHash = std::hash<bool>{}(key.pipelineStateDesc.bDepthTestEnable);
    const size_t depthWriteHash = std::hash<bool>{}(key.pipelineStateDesc.bDepthWriteEnable);
    const size_t depthCompareHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.pipelineStateDesc.depthCompareOp));
    const size_t cullModeHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.pipelineStateDesc.cullMode));
    const size_t blendModeHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.pipelineStateDesc.blendMode));
    const size_t shadowPassHash = std::hash<bool>{}(key.bIsShadowPass);

    size_t hash = renderPassHash;
    hash ^= shaderVariantHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= sampleCountHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= useVertexInputHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= depthTestHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= depthWriteHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= depthCompareHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= cullModeHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= blendModeHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
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
    const ShaderVariantKey& shaderVariantKey,
    vk::SampleCountFlagBits sampleCount,
    const GraphicsPipelineStateDesc& pipelineStateDesc,
    bool bIsShadowPass)
{
    GraphicsPipelineKey key{
        renderPass,
        shaderVariantKey,
        sampleCount,
        pipelineStateDesc,
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

    auto pipeline = std::make_shared<GraphicsPipeline>(device, gpuMemoryProperties, renderPass, shaderVariantKey, sampleCount, pipelineStateDesc, bIsShadowPass);
    graphicsPipelines[key] = pipeline;
    return pipeline;
}
