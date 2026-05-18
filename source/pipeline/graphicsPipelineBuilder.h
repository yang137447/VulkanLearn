#pragma once

#include <string>
#include <vector>
#include "vulkan/vulkan.hpp"

enum class GraphicsPipelineBlendMode
{
    Opaque,
    AlphaBlend,
    Additive
};

struct GraphicsPipelineStateDesc
{
    bool bUseVertexInput = true;
    bool bDepthTestEnable = true;
    bool bDepthWriteEnable = true;
    vk::CompareOp depthCompareOp = vk::CompareOp::eLess;
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;
    GraphicsPipelineBlendMode blendMode = GraphicsPipelineBlendMode::Opaque;
};

struct GraphicsPipelineBuildDesc
{
    vk::Device& device;
    vk::RenderPass& renderPass;
    vk::PipelineLayout& pipelineLayout;
    const std::vector<vk::PipelineShaderStageCreateInfo>& shaderStages;
    const vk::VertexInputBindingDescription& vertexInputBindingDescription;
    const std::vector<vk::VertexInputAttributeDescription>& vertexInputAttributeDescriptions;
    const std::string& pipelineName;
    vk::SampleCountFlagBits sampleCount;
    GraphicsPipelineStateDesc pipelineStateDesc;
    bool bIsShadowPass;
};

struct GraphicsPipelineBuildResult
{
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};

class GraphicsPipelineBuilder
{
public:
    static GraphicsPipelineBuildResult Build(const GraphicsPipelineBuildDesc& desc);
};
