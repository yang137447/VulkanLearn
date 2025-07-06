#include "RenderPipline.h"
#include "settings.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vulkan/vulkan_enums.hpp>
#include "DrawableObject.h"
#include "CommonFunction.h"

RenderPipline::RenderPipline(vk::Device &device, vk::RenderPass &renderPass, vk::PhysicalDeviceMemoryProperties &physicalDeviceMemoryProperties, const DrawableObject& drawableObject)
{
    this->device = &device;
    this->renderPass = &renderPass;
    this->physicalDeviceMemoryProperties = &physicalDeviceMemoryProperties;
    this->drawableObject = &drawableObject;

    CreateUniformBuffers();
    CreatePipelineLayout();
    CreateDescriptorSets();
    CreateShader();
    initVertexAttribute();
    CreateGraphicsPipeline();
}

RenderPipline::~RenderPipline()
{
    DestroyGraphicsPipeline();
    DestroyShader();
    DestroyDescriptorSets();
    DestroyPipelineLayout();
    DestroyUniformBuffers();
}

RenderPipline::RenderPipline()
{
}

void RenderPipline::CreateUniformBuffers()
{
    vk::DeviceSize uniformBufferSize = sizeof(UniformBufferObject);
    uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    uniformBufferMemories.resize(MAX_FRAMES_IN_FLIGHT);
    uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        std::tie(uniformBuffers[i], uniformBufferMemories[i]) = CommonFunction::CreateBuffer(
            *device, uniformBufferSize, usage, *physicalDeviceMemoryProperties, memoryPropertyFlags
        );
        uniformBuffersMapped[i] = device->mapMemory(uniformBufferMemories[i], 0, uniformBufferSize);
    }
    // 设置uniform缓冲区信息
    uniformBufferInfos.resize(MAX_FRAMES_IN_FLIGHT);
    for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        uniformBufferInfos[i]
            .setBuffer(uniformBuffers[i])
            .setOffset(0)
            .setRange(uniformBufferSize);
    }
}
void RenderPipline::DestroyUniformBuffers()
{
    for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        device->unmapMemory(uniformBufferMemories[i]);
        device->destroyBuffer(uniformBuffers[i]);
        device->freeMemory(uniformBufferMemories[i]);
    }
}

void RenderPipline::CreatePipelineLayout()
{
    vk::DescriptorSetLayoutBinding descriptorSetLayoutBindings;
    descriptorSetLayoutBindings
        .setBinding(0)
        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eVertex)
        .setPImmutableSamplers(nullptr);

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo
        .setBindings(descriptorSetLayoutBindings);
    
    vk::Result result = device->createDescriptorSetLayout(&descriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayout);
    assert(result == vk::Result::eSuccess);

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo
        .setSetLayouts(descriptorSetLayout);
    
    result = device->createPipelineLayout(&pipelineLayoutCreateInfo, nullptr, &pipelineLayout);
    assert(result == vk::Result::eSuccess);
}

void RenderPipline::DestroyPipelineLayout()
{
    device->destroyPipelineLayout(pipelineLayout, nullptr);
    device->destroyDescriptorSetLayout(descriptorSetLayout, nullptr);
}

void RenderPipline::CreateDescriptorSets()
{
    vk::DescriptorPoolSize descriptorPoolSize;
    descriptorPoolSize
        .setType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(MAX_FRAMES_IN_FLIGHT);

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(MAX_FRAMES_IN_FLIGHT)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSize);

    vk::Result result = device->createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result == vk::Result::eSuccess);

    std::vector<vk::DescriptorSetLayout> setLayouts(MAX_FRAMES_IN_FLIGHT, descriptorSetLayout);
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts);
    
    descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    result = device->allocateDescriptorSets(&descriptorSetAllocateInfo, descriptorSets.data());
    assert(result == vk::Result::eSuccess);
}

void RenderPipline::DestroyDescriptorSets()
{
    device->freeDescriptorSets(descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
    device->destroyDescriptorPool(descriptorPool, nullptr);
}

void RenderPipline::CreateShader()
{
    // 指定shader文件路径
    const std::string vertexShaderPath = filePath + "/shader/spv/test_vert.spv";
    const std::string fragmentShaderPath = filePath + "/shader/spv/test_frag.spv";

    // 读取shader文件内容
    std::vector<char> vertexShaderCode;
    std::ifstream vertexShaderFile(vertexShaderPath, std::ios::binary | std::ios::ate);
    if (!vertexShaderFile.is_open())
    {

        std::cerr << "Failed to open vertex shader file!" << std::endl;
        exit(1);
    }
    size_t vertShaderfileSize = (size_t)vertexShaderFile.tellg();
    if (vertShaderfileSize <= 0) {
        throw std::runtime_error("Shader file is empty or invalid: " + filePath);
    }

    vertexShaderCode.resize(vertShaderfileSize);
    vertexShaderFile.seekg(0);
    vertexShaderFile.read(vertexShaderCode.data(), vertexShaderCode.size());
    vertexShaderFile.close();

    std::vector<char> fragmentShaderCode;
    std::ifstream fragmentShaderFile(fragmentShaderPath, std::ios::binary | std::ios::ate);
    if (!fragmentShaderFile.is_open())
    {
        std::cerr << "Failed to open fragment shader file!" << std::endl;
        exit(1);
    }
    size_t fragmentShaderfileSize = (size_t)fragmentShaderFile.tellg();
    if (fragmentShaderfileSize <= 0) {
        throw std::runtime_error("Shader file is empty or invalid: " + filePath);
    }
    fragmentShaderCode.resize(fragmentShaderfileSize);
    fragmentShaderFile.seekg(0);
    fragmentShaderFile.read(fragmentShaderCode.data(), fragmentShaderCode.size());
    fragmentShaderFile.close();

    // 创建shader模块
    vk::ShaderModule vertexShaderModule;
    vk::ShaderModuleCreateInfo vertexShaderModuleCreateInfo;
    vertexShaderModuleCreateInfo
        .setCodeSize(vertexShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(vertexShaderCode.data()));
    vk::Result result = device->createShaderModule(&vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule);
    assert(result == vk::Result::eSuccess);
    vk::ShaderModule fragmentShaderModule;
    vk::ShaderModuleCreateInfo fragmentShaderModuleCreateInfo;
    fragmentShaderModuleCreateInfo
        .setCodeSize(fragmentShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(fragmentShaderCode.data()));
    result = device->createShaderModule(&fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule);
    assert(result == vk::Result::eSuccess);
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
    vertexInputBindingDescription = drawableObject->GetVertexInputBindingDescription();

    vertexInputAttributeDescriptions.resize(drawableObject->GetVertexInputAttributeDescriptions().size());
    vertexInputAttributeDescriptions = drawableObject->GetVertexInputAttributeDescriptions();
}

void RenderPipline::CreateGraphicsPipeline()
{
    std::vector<vk::DynamicState> dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };
    
    vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo;
    pipelineDynamicStateCreateInfo
        .setDynamicStates(dynamicStates);

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    pipelineVertexInputStateCreateInfo
        .setVertexBindingDescriptions(vertexInputBindingDescription)
        .setVertexAttributeDescriptions(vertexInputAttributeDescriptions);


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
        .setWidth(static_cast<float>(width))
        .setHeight(static_cast<float>(height))
        .setMinDepth(0.0f)
        .setMaxDepth(1.0f);
    vk::Rect2D scissor;
    scissor
        .setOffset({ 0, 0 })
        .setExtent({ static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
    vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo;
    pipelineViewportStateCreateInfo
        .setViewportCount(1)
        .setPViewports(&viewport)
        .setScissorCount(1)
        .setPScissors(&scissor);


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
        .setMinSampleShading(1.0f)
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
        //.setPDepthStencilState(&pipelineDepthStencilStateCreateInfo)
        .setPDepthStencilState(nullptr)
        .setStages(shaderStages)
        .setRenderPass(*renderPass)
        .setSubpass(0);

    vk::PipelineCacheCreateInfo pipelineCacheCreateInfo;
    pipelineCacheCreateInfo
        .setInitialDataSize(0)
        .setPInitialData(nullptr);
    
    vk::Result result = device->createPipelineCache(&pipelineCacheCreateInfo, nullptr, &pipelineCache);
    assert(result == vk::Result::eSuccess);

    result = device->createGraphicsPipelines(pipelineCache, 1, &graphicsPipelineCreateInfo, nullptr, &graphicsPipeline);
    assert(result == vk::Result::eSuccess);
}

void RenderPipline::DestroyGraphicsPipeline()
{
    device->destroyPipeline(graphicsPipeline);
    device->destroyPipelineCache(pipelineCache);
}
