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
        // 双源混合只占用 location 0；selectionMask 等普通输出可以追加在
        // 后续颜色附件上，但不能改变 location 0 的双源混合状态。
        if (desc.colorAttachmentCount == 0)
        {
            throw std::runtime_error(
                "Thin Translucent dual-source blending requires a color attachment");
        }

        // 最终颜色为 Add + Multiplier * Destination：
        // index=0 写入 Add，index=1 通过 Src1Color/Src1Alpha 提供逐通道目标乘数。
        pipelineColorBlendAttachmentStates[0]
            .setBlendEnable(true)
            .setSrcColorBlendFactor(vk::BlendFactor::eOne)
            .setDstColorBlendFactor(vk::BlendFactor::eSrc1Color)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eSrc1Alpha);

        // 未启用 independentBlend 时，所有颜色附件必须拥有完全相同的 blend state。
        // 双源材质不会写 selectionMask，但仍需让该占位附件复用同一状态以满足 Vulkan 合同。
        for (size_t attachmentIndex = 1;
             attachmentIndex < pipelineColorBlendAttachmentStates.size();
             ++attachmentIndex)
        {
            pipelineColorBlendAttachmentStates[attachmentIndex] =
                pipelineColorBlendAttachmentStates[0];
        }
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
