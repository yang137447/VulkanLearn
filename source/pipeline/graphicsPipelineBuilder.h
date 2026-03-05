#pragma once

#include <string>
#include <vector>
#include "vulkan/vulkan.hpp"

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
    bool bIsPostProcess;
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
