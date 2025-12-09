#include "renderPipline.h"
#include "vertexDataStruct.h"
#include "settings.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vulkan/vulkan_enums.hpp>
#include "commonFunction.h"
#include "shaderReflect.h"

RenderPipline::RenderPipline(vk::Device *device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::RenderPass* renderPass, const std::string& shaderName, vk::SampleCountFlagBits sampleCount)
{
    this->device = device;
    this->renderPass = renderPass;
    this->physicalDeviceMemoryProperties = physicalDeviceMemoryProperties;
    this->shaderName = shaderName;
    this->sampleCount = sampleCount;

    CreateShader();
    CreatePipelineLayout();
    initVertexAttribute();
    CreateGraphicsPipeline();
}

RenderPipline::~RenderPipline()
{
    DestroyGraphicsPipeline();
    DestroyShader();
    DestroyPipelineLayout();
}

RenderPipline::RenderPipline()
{
}

void RenderPipline::CreatePipelineLayout()
{
    // std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    // descriptorSetLayoutBindings.emplace_back(
    //     vk::DescriptorSetLayoutBinding()
    //     .setBinding(0)
    //     .setDescriptorType(vk::DescriptorType::eUniformBuffer)
    //     .setDescriptorCount(1)
    //     .setStageFlags(vk::ShaderStageFlagBits::eVertex)
    //     .setPImmutableSamplers(nullptr));
    // descriptorSetLayoutBindings.emplace_back(
    //     vk::DescriptorSetLayoutBinding()
    //     .setBinding(1)
    //     .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
    //     .setDescriptorCount(1)
    //     .setStageFlags(vk::ShaderStageFlagBits::eFragment)
    //     .setPImmutableSamplers(nullptr));
    std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    for(const auto& binding : shaderBindings)
    {
        vk::DescriptorSetLayoutBinding layoutBinding;
        layoutBinding
            .setBinding(binding.binding)
            .setDescriptorType(binding.type)
            .setDescriptorCount(1)
            .setStageFlags(binding.stageFlags)
            .setPImmutableSamplers(nullptr);
        descriptorSetLayoutBindings.push_back(layoutBinding);
    }

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

void RenderPipline::CreateShader()
{
    // 指定shader文件路径
    const std::string vertexShaderName = shaderName + "_vert.spv";
    const std::string fragmentShaderName = shaderName + "_frag.spv";
    const std::string vertexShaderPath = CommonFunction::Path(vertexShaderName);
    const std::string fragmentShaderPath = CommonFunction::Path(fragmentShaderName);

    // 读取shader文件内容
    std::string vertexShaderCode = CommonFunction::ReadFile(vertexShaderPath);
    std::string fragmentShaderCode = CommonFunction::ReadFile(fragmentShaderPath);

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

    // 反射shader,获取uniformbugfer和采样器信息
        //TODO: 后期这里直接读取shaderbinding, 生成步骤放在shader编译后，可能不止vert和frag参与，也可能包含其他阶段
    const std::string vertexDebugShaderName = shaderName + "_vert.debug";
    const std::string fragmentDebugShaderName = shaderName + "_frag.debug";
    const std::string vertexDebugShaderPath = CommonFunction::Path(vertexDebugShaderName);
    const std::string fragmentDebugShaderPath = CommonFunction::Path(fragmentDebugShaderName);
    std::string vertexDebugShaderCode = CommonFunction::ReadFile(vertexShaderPath);
    std::string fragmentDebugShaderCode = CommonFunction::ReadFile(fragmentShaderPath);
    std::vector<uint32_t> vertexDebugShaderCode32(reinterpret_cast<uint32_t*>(vertexDebugShaderCode.data()), reinterpret_cast<uint32_t*>(vertexDebugShaderCode.data() + vertexDebugShaderCode.size()));
    std::vector<uint32_t> fragmentDebugShaderCode32(reinterpret_cast<uint32_t*>(fragmentDebugShaderCode.data()), reinterpret_cast<uint32_t*>(fragmentDebugShaderCode.data() + fragmentDebugShaderCode.size()));
    
    std::vector<std::vector<uint32_t>> shaders = {vertexDebugShaderCode32, fragmentDebugShaderCode32};
    ShaderReflect shaderReflect(shaders);
    auto shaderBindings = shaderReflect.GetShaderBindings();
    this->shaderBindings = shaderBindings;

    // 按set和binding排序
    // 目前不用，vertex和fragment目前按顺序绑定，不会冲突
}

void RenderPipline::DestroyShader()
{
    device->destroyShaderModule(shaderStages[0].module, nullptr);
    device->destroyShaderModule(shaderStages[1].module, nullptr);  
}

void RenderPipline::initVertexAttribute()
{
    vertexInputBindingDescription = VertexInfo::vertexInputBindingDescription;

    vertexInputAttributeDescriptions.resize(VertexInfo::vertexInputAttributeDescriptions.size());
    vertexInputAttributeDescriptions = VertexInfo::vertexInputAttributeDescriptions;
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
        .setDepthCompareOp(vk::CompareOp::eLess)
        .setDepthBoundsTestEnable(false)
        .setMinDepthBounds(0.0f)
        .setMaxDepthBounds(1.0f)
        .setStencilTestEnable(false)
        .setBack(vk::StencilOpState())
        .setFront(vk::StencilOpState());
    
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        .setRasterizationSamples(sampleCount)
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
        .setPDepthStencilState(&pipelineDepthStencilStateCreateInfo)
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
