#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "vulkan/vulkan.hpp"
#include "graphicsPipelineBuilder.h"
#include "../shaderVariant.h"

class ComputePipeline;
class GraphicsPipeline;
namespace vk
{
    class Device;
}

struct GraphicsPipelineKey
{
    vk::RenderPass* renderPass;
    ShaderVariantKey shaderVariantKey;
    vk::SampleCountFlagBits sampleCount;
    uint32_t colorAttachmentCount;
    GraphicsPipelineStateDesc pipelineStateDesc;
    bool bIsShadowPass;

    bool operator==(const GraphicsPipelineKey& other) const
    {
        return renderPass == other.renderPass &&
            shaderVariantKey == other.shaderVariantKey &&
            sampleCount == other.sampleCount &&
            colorAttachmentCount == other.colorAttachmentCount &&
            pipelineStateDesc.bUseVertexInput == other.pipelineStateDesc.bUseVertexInput &&
            pipelineStateDesc.bDepthTestEnable == other.pipelineStateDesc.bDepthTestEnable &&
            pipelineStateDesc.bDepthWriteEnable == other.pipelineStateDesc.bDepthWriteEnable &&
            pipelineStateDesc.depthCompareOp == other.pipelineStateDesc.depthCompareOp &&
            pipelineStateDesc.cullMode == other.pipelineStateDesc.cullMode &&
            pipelineStateDesc.blendMode == other.pipelineStateDesc.blendMode &&
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
        vk::RenderPass* renderPass,
        const ShaderVariantKey& shaderVariantKey,
        vk::SampleCountFlagBits sampleCount,
        uint32_t colorAttachmentCount,
        const GraphicsPipelineStateDesc& pipelineStateDesc,
        bool bIsShadowPass);
private:
    vk::Device* device;
    std::unordered_map<std::string, std::weak_ptr<ComputePipeline>> computePipelines;
    std::unordered_map<GraphicsPipelineKey, std::weak_ptr<GraphicsPipeline>, GraphicsPipelineKeyHash> graphicsPipelines;
};
