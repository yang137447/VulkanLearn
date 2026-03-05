#include "graphicsPipeline.h"
#include "../vertexDataStruct.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vulkan/vulkan_enums.hpp>
#include "../commonFunction.h"
#include "graphicsPipelineBuilder.h"
#include "pipelineLayoutBuilder.h"
#include "../profiler.h"
#include "shaderReflectionService.h"
#include "../vulkanDebug.h"

GraphicsPipeline::GraphicsPipeline(vk::Device *device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::RenderPass* renderPass, const std::string& shaderName, vk::SampleCountFlagBits sampleCount, bool bIsPostProcess, bool bIsShadowPass)
{
    PROFILE_FUNCTION();
    this->device = device;
    this->renderPass = renderPass;
    this->physicalDeviceMemoryProperties = physicalDeviceMemoryProperties;
    this->shaderName = shaderName;
    this->sampleCount = sampleCount;
    this->bIsPostProcess = bIsPostProcess;
    this->bIsShadowPass = bIsShadowPass;

    CreateShader();
    CreateDescriptorSetLayouts();
    CreatePipelineLayout();
    initVertexAttribute();
    CreateGraphicsPipeline();
}

GraphicsPipeline::~GraphicsPipeline()
{
    DestroyGraphicsPipeline();
    DestroyDescriptorSetLayouts();
    DestroyShader();
    DestroyPipelineLayout();
}

GraphicsPipeline::GraphicsPipeline()
{
}

void GraphicsPipeline::CreateDescriptorSetLayouts()
{
    descriptorSetLayouts = PipelineLayoutBuilder::CreateDescriptorSetLayouts(*device, shaderBindings, shaderName);
}

void GraphicsPipeline::DestroyDescriptorSetLayouts()
{
    PipelineLayoutBuilder::DestroyDescriptorSetLayouts(*device, descriptorSetLayouts);
}

void GraphicsPipeline::CreatePipelineLayout()
{
    pipelineLayout = PipelineLayoutBuilder::CreatePipelineLayout(*device, descriptorSetLayouts, shaderName);
}

void GraphicsPipeline::DestroyPipelineLayout()
{
    PipelineLayoutBuilder::DestroyPipelineLayout(*device, pipelineLayout);
}

void GraphicsPipeline::CreateShader()
{
    const std::string vertexShaderName = shaderName + "_vert.spv";
    const std::string fragmentShaderName = shaderName + "_frag.spv";
    const std::string vertexShaderPath = CommonFunction::Path(vertexShaderName);
    const std::string fragmentShaderPath = CommonFunction::Path(fragmentShaderName);

    std::string vertexShaderCode = CommonFunction::ReadFile(vertexShaderPath);
    std::string fragmentShaderCode = CommonFunction::ReadFile(fragmentShaderPath);

    vk::ShaderModule vertexShaderModule;
    vk::ShaderModuleCreateInfo vertexShaderModuleCreateInfo;
    vertexShaderModuleCreateInfo
        .setCodeSize(vertexShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(vertexShaderCode.data()));
    vk::Result result = device->createShaderModule(&vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(*device, vertexShaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Vert: " + shaderName);

    vk::ShaderModule fragmentShaderModule;
    vk::ShaderModuleCreateInfo fragmentShaderModuleCreateInfo;
    fragmentShaderModuleCreateInfo
        .setCodeSize(fragmentShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(fragmentShaderCode.data()));
    result = device->createShaderModule(&fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(*device, fragmentShaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Frag: " + shaderName);
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

    this->shaderBindings = ShaderReflectionService::ReflectGraphicsFromDebugSpirv(shaderName);
}

void GraphicsPipeline::DestroyShader()
{
    device->destroyShaderModule(shaderStages[0].module, nullptr);
    device->destroyShaderModule(shaderStages[1].module, nullptr);  
}

void GraphicsPipeline::initVertexAttribute()
{
    vertexInputBindingDescription = VertexInfo::vertexInputBindingDescription;
    
    vertexInputAttributeDescriptions.resize(VertexInfo::vertexInputAttributeDescriptions.size());
    vertexInputAttributeDescriptions = VertexInfo::vertexInputAttributeDescriptions;
}

void GraphicsPipeline::CreateGraphicsPipeline()
{
    GraphicsPipelineBuildDesc buildDesc{
        *device,
        *renderPass,
        pipelineLayout,
        shaderStages,
        vertexInputBindingDescription,
        vertexInputAttributeDescriptions,
        shaderName,
        sampleCount,
        bIsPostProcess,
        bIsShadowPass
    };
    auto buildResult = GraphicsPipelineBuilder::Build(buildDesc);
    pipelineCache = buildResult.pipelineCache;
    graphicsPipeline = buildResult.graphicsPipeline;
}

void GraphicsPipeline::DestroyGraphicsPipeline()
{
    device->destroyPipeline(graphicsPipeline);
    device->destroyPipelineCache(pipelineCache);
}
