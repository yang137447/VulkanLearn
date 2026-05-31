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
#include "../shaderCompiler.h"

GraphicsPipeline::GraphicsPipeline(vk::Device *device, vk::RenderPass* renderPass, const ShaderVariantKey& shaderVariantKey, vk::SampleCountFlagBits sampleCount, uint32_t colorAttachmentCount, const GraphicsPipelineStateDesc& pipelineStateDesc, bool bIsShadowPass)
{
    PROFILE_FUNCTION();
    this->device = device;
    this->renderPass = renderPass;
    this->shaderVariantKey = shaderVariantKey;
    this->sampleCount = sampleCount;
    this->colorAttachmentCount = colorAttachmentCount;
    this->pipelineStateDesc = pipelineStateDesc;
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
    descriptorSetLayouts = PipelineLayoutBuilder::CreateDescriptorSetLayouts(*device, shaderBindings, shaderVariantKey.GetDisplayName());
}

void GraphicsPipeline::DestroyDescriptorSetLayouts()
{
    PipelineLayoutBuilder::DestroyDescriptorSetLayouts(*device, descriptorSetLayouts);
}

void GraphicsPipeline::CreatePipelineLayout()
{
    pipelineLayout = PipelineLayoutBuilder::CreatePipelineLayout(*device, descriptorSetLayouts, shaderVariantKey.GetDisplayName());
}

void GraphicsPipeline::DestroyPipelineLayout()
{
    PipelineLayoutBuilder::DestroyPipelineLayout(*device, pipelineLayout);
}

void GraphicsPipeline::CreateShader()
{
    ShaderCompiler::EnsureGraphicsVariantCompiled(shaderVariantKey);
    const std::string vertexShaderPath = CommonFunction::Path(shaderVariantKey.GetStageSpvRelativePath("vert"));
    const std::string fragmentShaderPath = CommonFunction::Path(shaderVariantKey.GetStageSpvRelativePath("frag"));

    std::string vertexShaderCode = CommonFunction::ReadFile(vertexShaderPath);
    std::string fragmentShaderCode = CommonFunction::ReadFile(fragmentShaderPath);

    vk::ShaderModule vertexShaderModule;
    vk::ShaderModuleCreateInfo vertexShaderModuleCreateInfo;
    vertexShaderModuleCreateInfo
        .setCodeSize(vertexShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(vertexShaderCode.data()));
    vk::Result result = device->createShaderModule(&vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(*device, vertexShaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Vert: " + shaderVariantKey.GetDisplayName());

    vk::ShaderModule fragmentShaderModule;
    vk::ShaderModuleCreateInfo fragmentShaderModuleCreateInfo;
    fragmentShaderModuleCreateInfo
        .setCodeSize(fragmentShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(fragmentShaderCode.data()));
    result = device->createShaderModule(&fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(*device, fragmentShaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Frag: " + shaderVariantKey.GetDisplayName());
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

    this->shaderBindings = ShaderReflectionService::ReflectGraphicsFromDebugSpirv(shaderVariantKey);
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
        shaderVariantKey.GetDisplayName(),
        sampleCount,
        colorAttachmentCount,
        pipelineStateDesc,
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
