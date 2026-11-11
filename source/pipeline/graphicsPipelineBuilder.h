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
    // 旧版直接写 GBufferC.rgb 的 decal 模式；DBuffer decal 已改用
    // DecalDBuffer 写入覆盖率与 mask，本模式不再由 DecalRenderer 使用。
    DecalColor,
    // DBuffer decal：attachment 0..3 使用 straight-alpha 覆盖率混合，
    // attachment 4（DBufferMask）使用 additive One/One 累积响应位，
    // attachment 5（DBufferEmissive）使用预乘 over 合成。
    // 两组状态不同，依赖设备特性 independentBlend（设备创建时启用）。
    DecalDBuffer,
    // 颜色公式为 Add + Multiplier * Destination，第二颜色源提供逐通道 Multiplier。
    ThinTranslucentDualSource
};

struct GraphicsPipelineStateDesc
{
    bool bUseVertexInput = true;
    bool bDepthTestEnable = true;
    bool bDepthWriteEnable = true;
    vk::CompareOp depthCompareOp = vk::CompareOp::eLess;
    // stencil 默认关闭；front/back 使用同一组状态，当前 decal mask
    // 不依赖双面不同 compare/op。
    bool bStencilTestEnable = false;
    uint32_t stencilWriteMask = 0xFF;
    uint32_t stencilCompareMask = 0xFF;
    uint32_t stencilReference = 0;
    vk::StencilOp stencilFailOp = vk::StencilOp::eKeep;
    vk::StencilOp stencilPassOp = vk::StencilOp::eKeep;
    vk::StencilOp stencilDepthFailOp = vk::StencilOp::eKeep;
    vk::CompareOp stencilCompareOp = vk::CompareOp::eAlways;
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;
    GraphicsPipelineBlendMode blendMode = GraphicsPipelineBlendMode::Opaque;
};

struct GraphicsPipelineBuildDesc
{
    vk::RenderPass& renderPass;
    vk::PipelineLayout& pipelineLayout;
    const std::vector<vk::PipelineShaderStageCreateInfo>& shaderStages;
    const std::vector<vk::VertexInputBindingDescription>& vertexInputBindingDescriptions;
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
