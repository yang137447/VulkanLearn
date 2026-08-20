#pragma once

#include <string>
#include <vector>
#include "vulkan/vulkan.hpp"

enum class GraphicsPipelineBlendMode
{
    Opaque,
    AlphaBlend,
    PremultipliedAlpha,
    Additive,
    // 颜色公式为 Add + Multiplier * Destination，第二颜色源提供逐通道 Multiplier。
    ThinTranslucentDualSource
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
    vk::RenderPass& renderPass;
    vk::PipelineLayout& pipelineLayout;
    const std::vector<vk::PipelineShaderStageCreateInfo>& shaderStages;
    const vk::VertexInputBindingDescription& vertexInputBindingDescription;
    const std::vector<vk::VertexInputAttributeDescription>& vertexInputAttributeDescriptions;
    const std::string& pipelineName;
    vk::SampleCountFlagBits sampleCount;
    uint32_t colorAttachmentCount;
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
    static GraphicsPipelineBuildResult Build(
        vk::Device& device,
        const GraphicsPipelineBuildDesc& desc);
};
