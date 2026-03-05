#include "graphicsPipelineBuilder.h"
#include "../commonFunction.h"
#include "../vulkanDebug.h"

GraphicsPipelineBuildResult GraphicsPipelineBuilder::Build(const GraphicsPipelineBuildDesc& desc)
{
    std::vector<vk::DynamicState> dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };

    vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo;
    pipelineDynamicStateCreateInfo
        .setDynamicStates(dynamicStates);

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    if (!desc.bIsPostProcess)
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
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eClockwise)
        .setDepthClampEnable(false)
        .setRasterizerDiscardEnable(false)
        .setDepthBiasEnable(false)
        .setLineWidth(1.0f);

    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState[1];
    pipelineColorBlendAttachmentState[0]
        .setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)
        .setBlendEnable(false)
        .setColorBlendOp(vk::BlendOp::eAdd)
        .setSrcColorBlendFactor(vk::BlendFactor::eOne)
        .setDstColorBlendFactor(vk::BlendFactor::eZero)
        .setAlphaBlendOp(vk::BlendOp::eAdd)
        .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
        .setDstAlphaBlendFactor(vk::BlendFactor::eZero);

    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    pipelineColorBlendStateCreateInfo
        .setAttachments(pipelineColorBlendAttachmentState)
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
    if (!desc.bIsPostProcess)
    {
        pipelineDepthStencilStateCreateInfo
            .setDepthTestEnable(true)
            .setDepthWriteEnable(true)
            .setDepthCompareOp(vk::CompareOp::eLess)
            .setDepthBoundsTestEnable(false)
            .setMinDepthBounds(0.0f)
            .setMaxDepthBounds(1.0f)
            .setStencilTestEnable(false)
            .setBack(vk::StencilOpState())
            .setFront(vk::StencilOpState());
    }

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

    vk::Result result = desc.device.createPipelineCache(&pipelineCacheCreateInfo, nullptr, &buildResult.pipelineCache);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(desc.device, buildResult.pipelineCache, vk::ObjectType::ePipelineCache, "PipelineCache: " + desc.pipelineName);

    result = desc.device.createGraphicsPipelines(buildResult.pipelineCache, 1, &graphicsPipelineCreateInfo, nullptr, &buildResult.graphicsPipeline);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(desc.device, buildResult.graphicsPipeline, vk::ObjectType::ePipeline, "Pipeline: " + desc.pipelineName);

    return buildResult;
}
