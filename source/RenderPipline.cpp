#include "RenderPipline.h"
#include "settings.h"
#include <fstream>
#include <iostream>

RenderPipline::RenderPipline(vk::Device &device, vk::RenderPass &renderPass, vk::PhysicalDeviceMemoryProperties &gpuMemoryProperties)
{
    this->device = &device;
    this->renderPass = &renderPass;
    this->gpuMemoryProperties = &gpuMemoryProperties;

    CreateUniformBuffer();
    CreatePipelineLayout();
    initDescriptorSet();
    CreateShader();
    initVertexAttribute();
    CreateGraphicsPipeline();
}

RenderPipline::~RenderPipline()
{
    DestroyGraphicsPipeline();
    DestroyShader();
    DestroyGraphicsPipeline();
    DestroyUniformBuffer();
}

RenderPipline::RenderPipline()
{
}

void RenderPipline::CreateUniformBuffer()
{
    uniformBufferSize = sizeof(float) * 16;
    vk::BufferCreateInfo bufferCreateInfo;
    bufferCreateInfo
        .setSize(uniformBufferSize)
        .setUsage(vk::BufferUsageFlagBits::eUniformBuffer)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setFlags(vk::BufferCreateFlagBits::eSparseBinding);
    
    vk::Result result = device->createBuffer(&bufferCreateInfo, nullptr, &uniformBuffer);
    assert(result != vk::Result::eSuccess);

    vk::MemoryRequirements memoryRequirements;
    device->getBufferMemoryRequirements(uniformBuffer, &memoryRequirements);

    vk::MemoryAllocateInfo memoryAllocateInfo;
    memoryAllocateInfo
        .setAllocationSize(memoryRequirements.size)
        .setMemoryTypeIndex(0);

    vk::Flags requiredMask = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    for (uint32_t i = 0; i < gpuMemoryProperties->memoryTypeCount; i++)
    {
        if ((memoryRequirements.memoryTypeBits & (1 << i)) && (gpuMemoryProperties->memoryTypes[i].propertyFlags & requiredMask) == requiredMask)
        {
            memoryAllocateInfo.setMemoryTypeIndex(i);
            break;
        }
    }

    result = device->allocateMemory(&memoryAllocateInfo, nullptr, &uniformBufferMemory);
    assert(result != vk::Result::eSuccess);

    device->bindBufferMemory(uniformBuffer, uniformBufferMemory, 0);
    void* data = device->mapMemory(uniformBufferMemory, 0, uniformBufferSize);
    device->unmapMemory(uniformBufferMemory);
    device->bindBufferMemory(uniformBuffer, uniformBufferMemory, 0);

    uniformBufferInfo
        .setBuffer(uniformBuffer)
        .setOffset(0)
        .setRange(uniformBufferSize);
}

void RenderPipline::DestroyUniformBuffer()
{
    device->destroyBuffer(uniformBuffer, nullptr);
    device->freeMemory(uniformBufferMemory, nullptr);
}

void RenderPipline::CreatePipelineLayout()
{
    vk::DescriptorSetLayoutBinding descriptorSetLayoutBindings[1];
    descriptorSetLayoutBindings[0]
        .setBinding(0)
        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eVertex)
        .setPImmutableSamplers(nullptr);

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo
        .setBindingCount(1)
        .setPBindings(descriptorSetLayoutBindings);
    
    vk::Result result = device->createDescriptorSetLayout(&descriptorSetLayoutCreateInfo, nullptr, descriptorSetLayouts.data());
    assert(result != vk::Result::eSuccess);

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo
        .setSetLayoutCount(static_cast<uint32_t>(descriptorSetLayouts.size()))
        .setPSetLayouts(descriptorSetLayouts.data())
        .setPushConstantRangeCount(0)
        .setPPushConstantRanges(nullptr);
    
    result = device->createPipelineLayout(&pipelineLayoutCreateInfo, nullptr, &pipelineLayout);
    assert(result != vk::Result::eSuccess);
}

void RenderPipline::DestroyPipelineLayout()
{
    device->destroyPipelineLayout(pipelineLayout, nullptr);
    for (auto descriptorSetLayout : descriptorSetLayouts)
    {
        device->destroyDescriptorSetLayout(descriptorSetLayout, nullptr);
    }
    descriptorSetLayouts.clear();
}

void RenderPipline::initDescriptorSet()
{
    vk::DescriptorPoolSize descriptorPoolSize[1];
    descriptorPoolSize[0]
        .setType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(1);

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(1)
        .setPoolSizeCount(1)
        .setPPoolSizes(descriptorPoolSize);

    vk::Result result = device->createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result != vk::Result::eSuccess);

    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.push_back(descriptorSetLayouts[0]);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo[1];
    descriptorSetAllocateInfo[0]
        .setDescriptorPool(descriptorPool)
        .setDescriptorSetCount(1)
        .setPSetLayouts(layouts.data());
    
    result = device->allocateDescriptorSets(descriptorSetAllocateInfo, descriptorSets.data());
    assert(result != vk::Result::eSuccess);

    writeDescriptorSet[0]
        .setDstSet(descriptorSets[0])
        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(1)
        .setPBufferInfo(&uniformBufferInfo)
        .setDstBinding(0)
        .setDstArrayElement(0);
}

void RenderPipline::CreateShader()
{
    // 指定shader文件路径
    const std::string vertexShaderPath = filePath + "shader/spv/test_vert.spv";
    const std::string fragmentShaderPath = filePath + "shader/spv/test_frag.spv";

    // 读取shader文件内容
    std::vector<uint32_t> vertexShaderCode;
    std::ifstream vertexShaderFile(vertexShaderPath, std::ios::binary | std::ios::ate);
    if (vertexShaderFile.is_open())
    {
        vertexShaderCode.resize(vertexShaderFile.tellg());
        vertexShaderFile.seekg(0);
        vertexShaderFile.read(reinterpret_cast<char*>(vertexShaderCode.data()), vertexShaderCode.size());
        vertexShaderFile.close();
    }
    else
    {
        std::cerr << "Failed to open vertex shader file!" << std::endl;
        return;
    }
    std::vector<uint32_t> fragmentShaderCode;
    std::ifstream fragmentShaderFile(fragmentShaderPath, std::ios::binary | std::ios::ate);
    if (fragmentShaderFile.is_open())
    {
        fragmentShaderCode.resize(fragmentShaderFile.tellg());
        fragmentShaderFile.seekg(0);
        fragmentShaderFile.read(reinterpret_cast<char*>(fragmentShaderCode.data()), fragmentShaderCode.size());
        fragmentShaderFile.close();
    }
    else
    {
        std::cerr << "Failed to open fragment shader file!" << std::endl;
        return;
    }
    // 创建shader模块
    vk::ShaderModule vertexShaderModule;
    vk::ShaderModuleCreateInfo vertexShaderModuleCreateInfo;
    vertexShaderModuleCreateInfo
        .setCodeSize(vertexShaderCode.size() * sizeof(uint32_t))
        .setPCode(vertexShaderCode.data());
    vk::Result result = device->createShaderModule(&vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule);
    assert(result != vk::Result::eSuccess);
    vk::ShaderModule fragmentShaderModule;
    vk::ShaderModuleCreateInfo fragmentShaderModuleCreateInfo;
    fragmentShaderModuleCreateInfo
        .setCodeSize(fragmentShaderCode.size() * sizeof(uint32_t))
        .setPCode(fragmentShaderCode.data());
    result = device->createShaderModule(&fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule);
    assert(result != vk::Result::eSuccess);
    // 创建shader阶段
    shaderStages.resize(2);
    shaderStages[0]
        .setStage(vk::ShaderStageFlagBits::eVertex)
        .setModule(vertexShaderModule)
        .setPName("main")
        .setPSpecializationInfo(nullptr);
    shaderStages[1]
        .setStage(vk::ShaderStageFlagBits::eFragment)
        .setModule(fragmentShaderModule)
        .setPName("main")
        .setPSpecializationInfo(nullptr);
}

void RenderPipline::DestroyShader()
{
    device->destroyShaderModule(shaderStages[0].module, nullptr);
    device->destroyShaderModule(shaderStages[1].module, nullptr);  
}

void RenderPipline::initVertexAttribute()
{
    vertexInputBindingDescription
        .setBinding(0)
        .setStride(sizeof(float) * 6)
        .setInputRate(vk::VertexInputRate::eVertex);

    vertexInputAttributeDescriptions.resize(2);
    vertexInputAttributeDescriptions[0]
        .setBinding(0)
        .setLocation(0)
        .setFormat(vk::Format::eR32G32B32Sfloat)
        .setOffset(0);
    vertexInputAttributeDescriptions[1]
        .setBinding(0)
        .setLocation(1)
        .setFormat(vk::Format::eR32G32Sfloat)
        .setOffset(sizeof(float) * 12);
}

void RenderPipline::CreateGraphicsPipeline()
{
    std::vector<vk::DynamicState> dynamicState;
    
    vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo;
    pipelineDynamicStateCreateInfo
        .setDynamicStateCount(dynamicState.size())
        .setPDynamicStates(dynamicState.data());

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    pipelineVertexInputStateCreateInfo
        .setVertexBindingDescriptionCount(1)
        .setPVertexBindingDescriptions(&vertexInputBindingDescription)
        .setVertexAttributeDescriptionCount(static_cast<uint32_t>(vertexInputAttributeDescriptions.size()))
        .setPVertexAttributeDescriptions(vertexInputAttributeDescriptions.data());

    vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;
    pipelineInputAssemblyStateCreateInfo
        .setTopology(vk::PrimitiveTopology::eTriangleList)
        .setPrimitiveRestartEnable(false);
    
    vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo;
    pipelineRasterizationStateCreateInfo
        .setPolygonMode(vk::PolygonMode::eFill)
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setDepthClampEnable(true)
        .setRasterizerDiscardEnable(false)
        .setDepthBiasEnable(false)
        .setLineWidth(1.0f);

    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState[1];
    pipelineColorBlendAttachmentState[0]
        .setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)
        .setBlendEnable(false)
        .setColorBlendOp(vk::BlendOp::eAdd)
        .setAlphaBlendOp(vk::BlendOp::eAdd)
        .setSrcColorBlendFactor(vk::BlendFactor::eZero)
        .setDstColorBlendFactor(vk::BlendFactor::eZero)
        .setSrcAlphaBlendFactor(vk::BlendFactor::eZero)
        .setDstAlphaBlendFactor(vk::BlendFactor::eZero);

    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    pipelineColorBlendStateCreateInfo
        .setAttachmentCount(1)
        .setPAttachments(pipelineColorBlendAttachmentState)
        .setLogicOpEnable(false)
        .setLogicOp(vk::LogicOp::eNoOp)
        .setBlendConstants({ 1.0f, 1.0f, 1.0f, 1.0f });

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
        .setOffset({ 0, 0 })
        .setExtent({ width, height });
    vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo;
    pipelineViewportStateCreateInfo
        .setViewportCount(1)
        .setScissorCount(1)
        .setPScissors(&scissor)
        .setPViewports(&viewport);

    vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;
    pipelineDepthStencilStateCreateInfo
        .setDepthTestEnable(true)
        .setDepthWriteEnable(true)
        .setDepthCompareOp(vk::CompareOp::eLessOrEqual)
        .setDepthBoundsTestEnable(false)
        .setMinDepthBounds(0.0f)
        .setMaxDepthBounds(1.0f)
        .setStencilTestEnable(false)
        .setBack(vk::StencilOpState())
        .setFront(vk::StencilOpState());
    
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        .setRasterizationSamples(vk::SampleCountFlagBits::e1)
        .setSampleShadingEnable(false)
        .setMinSampleShading(0.0f)
        .setPSampleMask(nullptr)
        .setAlphaToCoverageEnable(false)
        .setAlphaToOneEnable(false);
    
    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo;
    graphicsPipelineCreateInfo
        .setLayout(pipelineLayout)
        .setPVertexInputState(&pipelineVertexInputStateCreateInfo)
        .setPInputAssemblyState(&pipelineInputAssemblyStateCreateInfo)
        .setPRasterizationState(&pipelineRasterizationStateCreateInfo)
        .setPColorBlendState(&pipelineColorBlendStateCreateInfo)
        .setPTessellationState(nullptr)
        .setPMultisampleState(&pipelineMultisampleStateCreateInfo)
        .setPDynamicState(&pipelineDynamicStateCreateInfo)
        .setPViewportState(&pipelineViewportStateCreateInfo)
        .setPDepthStencilState(&pipelineDepthStencilStateCreateInfo)
        .setStageCount(static_cast<uint32_t>(shaderStages.size()))
        .setPStages(shaderStages.data())
        .setRenderPass(*renderPass)
        .setSubpass(0);

    vk::PipelineCacheCreateInfo pipelineCacheCreateInfo;
    pipelineCacheCreateInfo
        .setInitialDataSize(0)
        .setPInitialData(nullptr)
        .setFlags(vk::PipelineCacheCreateFlagBits::eExternallySynchronized);
    
    vk::Result result = device->createPipelineCache(&pipelineCacheCreateInfo, nullptr, &pipelineCache);
    assert(result != vk::Result::eSuccess);

    result = device->createGraphicsPipelines(pipelineCache, 1, &graphicsPipelineCreateInfo, nullptr, &graphicsPipeline);
    assert(result != vk::Result::eSuccess);
}

void RenderPipline::DestroyGraphicsPipeline()
{
    device->destroyPipeline(graphicsPipeline);
    device->destroyPipelineCache(pipelineCache);
}
