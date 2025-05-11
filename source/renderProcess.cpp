#include "renderProcess.h"
#include "shaderModule.h"
#include "swapchain.h"
#include "settings.h"

RenderProcess::RenderProcess(const vk::Device &device,
                             const vk::RenderPass &renderPass,
                             const ShaderModule &shaderModule,
                             const Swapchain &swapchain)
{
    this->device = &device;
    this->renderPass = &renderPass;
    this->shaderModule = &shaderModule;
    this->swapchain = &swapchain;

    CreateVkPipelineLayout();
    CreateVkGraphicsPipeline();
}

RenderProcess::~RenderProcess()
{
    DestroyVkGraphicsPipeline();
    DestroyVkPipelineLayout();
}

void RenderProcess::CreateVkGraphicsPipeline()
{
    // vertex input
    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;

    // vertex assembly
    vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;
    pipelineInputAssemblyStateCreateInfo
        .setPrimitiveRestartEnable(VK_FALSE)
        .setTopology(vk::PrimitiveTopology::eTriangleList);

    // shader
    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::PipelineShaderStageCreateInfo vertexShaderStageCreateInfo;
    vertexShaderStageCreateInfo
        .setStage(vk::ShaderStageFlagBits::eVertex)
        .setModule(shaderModule->GetVertexShaderModule())
        .setPName("main");
    vk::PipelineShaderStageCreateInfo fragmentShaderStageCreateInfo;
    fragmentShaderStageCreateInfo
        .setStage(vk::ShaderStageFlagBits::eFragment)
        .setModule(shaderModule->GetFragmentShaderModule())
        .setPName("main");

    shaderStages.emplace_back(std::move(vertexShaderStageCreateInfo));
    shaderStages.emplace_back(std::move(fragmentShaderStageCreateInfo));

    // viewport
    vk::Viewport viewport;
    viewport
        .setX(0.0f)
        .setY(0.0f)
        .setWidth(static_cast<float>(width))
        .setHeight(static_cast<float>(height))
        .setMinDepth(0.0f)
        .setMaxDepth(1.0f);
    vk::Rect2D scissor;
    scissor
        .setOffset({0, 0})
        .setExtent({width, height});
    vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo;
    pipelineViewportStateCreateInfo
        .setViewportCount(1)
        .setPViewports(&viewport)
        .setScissorCount(1)
        .setPScissors(&scissor);

    // rasterizer
    vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo;
    pipelineRasterizationStateCreateInfo
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eClockwise)
        .setLineWidth(1.0f)
        .setPolygonMode(vk::PolygonMode::eFill)
        .setRasterizerDiscardEnable(VK_FALSE);

    // multisampling
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        .setSampleShadingEnable(VK_FALSE)
        .setRasterizationSamples(vk::SampleCountFlagBits::e1);

    // depth stencil
    vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;

    // color blending
    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
    pipelineColorBlendAttachmentState
        .setColorWriteMask(
            vk::ColorComponentFlagBits::eR |
            vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB |
            vk::ColorComponentFlagBits::eA)
        .setBlendEnable(VK_FALSE);

    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    pipelineColorBlendStateCreateInfo
        .setLogicOpEnable(VK_FALSE)
        .setAttachments(pipelineColorBlendAttachmentState);

    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo;
    graphicsPipelineCreateInfo
        .setPVertexInputState(&pipelineVertexInputStateCreateInfo)
        .setPInputAssemblyState(&pipelineInputAssemblyStateCreateInfo)
        .setStageCount(shaderStages.size())
        .setPStages(shaderStages.data())
        .setPViewportState(&pipelineViewportStateCreateInfo)
        .setPRasterizationState(&pipelineRasterizationStateCreateInfo)
        .setPMultisampleState(&pipelineMultisampleStateCreateInfo)
        .setPDepthStencilState(&pipelineDepthStencilStateCreateInfo)
        .setPColorBlendState(&pipelineColorBlendStateCreateInfo)
        .setRenderPass(*renderPass)
        .setLayout(pipelineLayout);

    vk::ResultValue resultValue = device->createGraphicsPipeline(nullptr, graphicsPipelineCreateInfo);
    if (resultValue.result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to create graphics pipeline!");
    }
    graphicsPipeline = resultValue.value;
}

void RenderProcess::DestroyVkGraphicsPipeline()
{
    device->destroyPipeline(graphicsPipeline);
}

void RenderProcess::CreateVkPipelineLayout()
{
    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo
        .setSetLayoutCount(0)
        .setPushConstantRangeCount(0);

    pipelineLayout = device->createPipelineLayout(pipelineLayoutCreateInfo);
}

void RenderProcess::DestroyVkPipelineLayout()
{
    device->destroyPipelineLayout(pipelineLayout);
}