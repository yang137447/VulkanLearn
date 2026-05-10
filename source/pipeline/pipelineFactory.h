#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "vulkan/vulkan.hpp"
#include "graphicsPipelineBuilder.h"

class ComputePipeline;
class GraphicsPipeline;
namespace vk
{
    class Device;
}

struct GraphicsPipelineKey
{
    vk::RenderPass* renderPass;
    std::string shaderName;
    vk::SampleCountFlagBits sampleCount;
    GraphicsPipelineStateDesc pipelineStateDesc;
    bool bIsShadowPass;

    bool operator==(const GraphicsPipelineKey& other) const
    {
        return renderPass == other.renderPass &&
            shaderName == other.shaderName &&
            sampleCount == other.sampleCount &&
            pipelineStateDesc.bUseVertexInput == other.pipelineStateDesc.bUseVertexInput &&
            pipelineStateDesc.bDepthTestEnable == other.pipelineStateDesc.bDepthTestEnable &&
            pipelineStateDesc.bDepthWriteEnable == other.pipelineStateDesc.bDepthWriteEnable &&
            pipelineStateDesc.depthCompareOp == other.pipelineStateDesc.depthCompareOp &&
            pipelineStateDesc.cullMode == other.pipelineStateDesc.cullMode &&
            bIsShadowPass == other.bIsShadowPass;
    }
};

struct GraphicsPipelineKeyHash
{
    size_t operator()(const GraphicsPipelineKey& key) const;
};

class PipelineFactory
{
public:
    PipelineFactory(vk::Device* device);
    std::shared_ptr<ComputePipeline> CreateComputePipeline(const std::string& shaderName);
    std::shared_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties,
        vk::RenderPass* renderPass,
        const std::string& shaderName,
        vk::SampleCountFlagBits sampleCount,
        const GraphicsPipelineStateDesc& pipelineStateDesc,
        bool bIsShadowPass);
private:
    vk::Device* device;
    std::unordered_map<std::string, std::weak_ptr<ComputePipeline>> computePipelines;
    std::unordered_map<GraphicsPipelineKey, std::weak_ptr<GraphicsPipeline>, GraphicsPipelineKeyHash> graphicsPipelines;
};
