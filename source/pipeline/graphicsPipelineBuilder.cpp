#include "graphicsPipelineBuilder.h"
#include "../commonFunction.h"
#include "../vulkanDebug.h"
#include "vulkanPipelineDiagnostics.h"

#include <stdexcept>

GraphicsPipelineBuildResult GraphicsPipelineBuilder::Build(
    vk::Device& device,
    const GraphicsPipelineBuildDesc& desc)
{
    std::vector<vk::DynamicState> dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };

    vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo;
    pipelineDynamicStateCreateInfo
        .setDynamicStates(dynamicStates);

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    if (desc.pipelineStateDesc.bUseVertexInput)
    {
        pipelineVertexInputStateCreateInfo
            .setVertexBindingDescriptions(desc.vertexInputBindingDescription)
            .setVertexAttributeDescriptions(desc.vertexInputAttributeDescriptions);
    }

    vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;
    pipelineInputAssemblyStateCreateInfo
        .setTopology(vk::PrimitiveTopology::eTriangleList)
        .setPrimitiveRestartEnable(false);

    vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo;
    pipelineRasterizationStateCreateInfo
        .setPolygonMode(vk::PolygonMode::eFill)
        .setCullMode(desc.pipelineStateDesc.cullMode)
        .setFrontFace(vk::FrontFace::eClockwise)
        .setDepthClampEnable(false)
        .setRasterizerDiscardEnable(false)
        .setDepthBiasEnable(false)
        .setLineWidth(1.0f);

    std::vector<vk::PipelineColorBlendAttachmentState> pipelineColorBlendAttachmentStates(desc.colorAttachmentCount);
    for (auto& attachmentState : pipelineColorBlendAttachmentStates)
    {
        attachmentState
            .setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)
            .setBlendEnable(false)
            .setColorBlendOp(vk::BlendOp::eAdd)
            .setSrcColorBlendFactor(vk::BlendFactor::eOne)
            .setDstColorBlendFactor(vk::BlendFactor::eZero)
            .setAlphaBlendOp(vk::BlendOp::eAdd)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eZero);
    }

    if (desc.pipelineStateDesc.blendMode == GraphicsPipelineBlendMode::AlphaBlend)
    {
        for (auto& attachmentState : pipelineColorBlendAttachmentStates)
        {
            attachmentState
                .setBlendEnable(true)
                .setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
                .setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
                .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
                .setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha);
        }
    }
    else if (desc.pipelineStateDesc.blendMode == GraphicsPipelineBlendMode::PremultipliedAlpha)
    {
        for (auto& attachmentState : pipelineColorBlendAttachmentStates)
        {
            attachmentState
                .setBlendEnable(true)
                .setSrcColorBlendFactor(vk::BlendFactor::eOne)
                .setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
                .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
                .setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha);
        }
    }
    else if (desc.pipelineStateDesc.blendMode == GraphicsPipelineBlendMode::Additive)
    {
        for (auto& attachmentState : pipelineColorBlendAttachmentStates)
        {
            attachmentState
                .setBlendEnable(true)
                .setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
                .setDstColorBlendFactor(vk::BlendFactor::eOne)
                .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
                .setDstAlphaBlendFactor(vk::BlendFactor::eOne);
        }
    }
    else if (desc.pipelineStateDesc.blendMode == GraphicsPipelineBlendMode::ThinTranslucentDualSource)
    {
        // 双源混合的两个 fragment 输出共享同一个 location，只用 index 区分，
        // 因此该模式的 pass 必须只有一个颜色附件，不能套用 Geometry 多 MRT 配置。
        if (desc.colorAttachmentCount != 1)
        {
            throw std::runtime_error(
                "Thin Translucent dual-source blending requires exactly one color attachment");
        }

        // 最终颜色为 Add + Multiplier * Destination：
        // index=0 写入 Add，index=1 通过 Src1Color/Src1Alpha 提供逐通道目标乘数。
        pipelineColorBlendAttachmentStates[0]
            .setBlendEnable(true)
            .setSrcColorBlendFactor(vk::BlendFactor::eOne)
            .setDstColorBlendFactor(vk::BlendFactor::eSrc1Color)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eSrc1Alpha);
    }

    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    pipelineColorBlendStateCreateInfo
        .setAttachments(pipelineColorBlendAttachmentStates)
        .setLogicOpEnable(false)
        .setLogicOp(vk::LogicOp::eCopy)
        .setBlendConstants({ 0.0f, 0.0f, 0.0f, 0.0f });

    vk::Viewport viewport;
    viewport
        .setX(0.0f)
        .setY(0.0f)
        .setWidth(static_cast<float>(CommonFunction::GetWindowSize().x()))
        .setHeight(static_cast<float>(CommonFunction::GetWindowSize().y()))
        .setMinDepth(0.0f)
        .setMaxDepth(1.0f);

    vk::Rect2D scissor;
    scissor
        .setOffset({ 0, 0 })
        .setExtent({
            static_cast<uint32_t>(CommonFunction::GetWindowSize().x()),
            static_cast<uint32_t>(CommonFunction::GetWindowSize().y()) });

    vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo;
    pipelineViewportStateCreateInfo
        .setViewportCount(1)
        .setPViewports(&viewport)
        .setScissorCount(1)
        .setPScissors(&scissor);

    vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;
    pipelineDepthStencilStateCreateInfo
        .setDepthTestEnable(desc.pipelineStateDesc.bDepthTestEnable)
        .setDepthWriteEnable(desc.pipelineStateDesc.bDepthWriteEnable)
        .setDepthCompareOp(desc.pipelineStateDesc.depthCompareOp)
        .setDepthBoundsTestEnable(false)
        .setMinDepthBounds(0.0f)
        .setMaxDepthBounds(1.0f)
        .setStencilTestEnable(false)
        .setBack(vk::StencilOpState())
        .setFront(vk::StencilOpState());

    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        .setRasterizationSamples(desc.sampleCount)
        .setSampleShadingEnable(false)
        .setMinSampleShading(1.0f)
        .setPSampleMask(nullptr)
        .setAlphaToCoverageEnable(false)
        .setAlphaToOneEnable(false);

    if (desc.bIsShadowPass)
    {
        pipelineMultisampleStateCreateInfo
            .setRasterizationSamples(vk::SampleCountFlagBits::e1);
    }

    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo;
    graphicsPipelineCreateInfo
        .setLayout(desc.pipelineLayout)
        .setPVertexInputState(&pipelineVertexInputStateCreateInfo)
        .setPInputAssemblyState(&pipelineInputAssemblyStateCreateInfo)
        .setPRasterizationState(&pipelineRasterizationStateCreateInfo)
        .setPColorBlendState(&pipelineColorBlendStateCreateInfo)
        .setPTessellationState(nullptr)
        .setPMultisampleState(&pipelineMultisampleStateCreateInfo)
        .setPDynamicState(&pipelineDynamicStateCreateInfo)
        .setPViewportState(&pipelineViewportStateCreateInfo)
        .setPDepthStencilState(&pipelineDepthStencilStateCreateInfo)
        .setStages(desc.shaderStages)
        .setRenderPass(desc.renderPass)
        .setSubpass(0);

    GraphicsPipelineBuildResult buildResult;

    vk::PipelineCacheCreateInfo pipelineCacheCreateInfo;
    pipelineCacheCreateInfo
        .setInitialDataSize(0)
        .setPInitialData(nullptr);

    vk::Result result = device.createPipelineCache(&pipelineCacheCreateInfo, nullptr, &buildResult.pipelineCache);
    VL::RequireVulkanPipelineSuccess(result, "Create pipeline cache", desc.pipelineName, "graphics pipeline");
    VulkanDebug::SetObjectName(device, buildResult.pipelineCache, vk::ObjectType::ePipelineCache, "PipelineCache: " + desc.pipelineName);

    result = device.createGraphicsPipelines(buildResult.pipelineCache, 1, &graphicsPipelineCreateInfo, nullptr, &buildResult.graphicsPipeline);
    if (result != vk::Result::eSuccess)
    {
        device.destroyPipelineCache(buildResult.pipelineCache, nullptr);
        buildResult.pipelineCache = nullptr;
        VL::RequireVulkanPipelineSuccess(
            result,
            "Create graphics pipeline",
            desc.pipelineName,
            "graphics pipeline");
    }
    VulkanDebug::SetObjectName(device, buildResult.graphicsPipeline, vk::ObjectType::ePipeline, "Pipeline: " + desc.pipelineName);

    return buildResult;
}
