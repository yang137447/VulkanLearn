#include "graphicsPipeline.h"
#include "../vertexDataStruct.h"
#include <cstdint>
#include "../commonFunction.h"
#include "graphicsPipelineBuilder.h"
#include "pipelineLayoutBuilder.h"
#include "../profiler.h"
#include "vulkanPipelineDiagnostics.h"
#include "../vulkanDebug.h"
#include <algorithm>

namespace
{

bool ComparePipelineLayoutBindings(
    const ShaderBinding& lhs,
    const ShaderBinding& rhs)
{
    if (lhs.set != rhs.set)
    {
        return lhs.set < rhs.set;
    }
    return lhs.binding < rhs.binding;
}

std::vector<ShaderBinding> BuildPipelineLayoutBindings(
    const std::vector<ShaderBinding>& shaderBindings,
    const GraphicsPipelineLayoutDesc& layoutDesc)
{
    std::vector<ShaderBinding> bindings;
    for (const ShaderBinding& binding : shaderBindings)
    {
        if (!layoutDesc.HasSetOverride(binding.set))
        {
            bindings.push_back(binding);
        }
    }

    for (uint32_t setIndex = 0; setIndex < MAX_DESCRIPTOR_SETS; ++setIndex)
    {
        if (!layoutDesc.HasSetOverride(setIndex))
        {
            continue;
        }
        bindings.insert(
            bindings.end(),
            layoutDesc.setBindings[setIndex].begin(),
            layoutDesc.setBindings[setIndex].end());
    }

    std::sort(bindings.begin(), bindings.end(), ComparePipelineLayoutBindings);
    return bindings;
}

} // namespace

GraphicsPipeline::GraphicsPipeline(
    vk::Device *device,
    vk::RenderPass* renderPass,
    const GraphicsShaderVariantArtifact& shaderArtifact,
    vk::SampleCountFlagBits sampleCount,
    uint32_t colorAttachmentCount,
    const GraphicsPipelineStateDesc& pipelineStateDesc,
    bool bIsShadowPass,
    const GraphicsPipelineLayoutDesc& pipelineLayoutDesc)
{
    PROFILE_FUNCTION();
    this->device = device;
    this->shaderDisplayName = shaderArtifact.displayName;
    this->vertexSpvPath = shaderArtifact.vertexSpvPath;
    this->fragmentSpvPath = shaderArtifact.fragmentSpvPath;
    this->shaderBindings = shaderArtifact.shaderBindings;

    CreateShader();
    CreateDescriptorSetLayouts(pipelineLayoutDesc);
    CreatePipelineLayout();
    initVertexAttribute();
    CreateGraphicsPipeline(
        *renderPass,
        sampleCount,
        colorAttachmentCount,
        pipelineStateDesc,
        bIsShadowPass);
}

GraphicsPipeline::~GraphicsPipeline()
{
    DestroyGraphicsPipeline();
    DestroyDescriptorSetLayouts();
    DestroyShader();
    DestroyPipelineLayout();
}

void GraphicsPipeline::CreateDescriptorSetLayouts(
    const GraphicsPipelineLayoutDesc& pipelineLayoutDesc)
{
    // Material ShadowCaster shaders may read only a subset of Surface resources,
    // while binding descriptor sets allocated from the full Surface contract.
    // Keep shaderBindings as reflection truth and apply overrides only to layout creation.
    descriptorLayoutBindings =
        BuildPipelineLayoutBindings(shaderBindings, pipelineLayoutDesc);
    descriptorSetLayouts = PipelineLayoutBuilder::CreateDescriptorSetLayouts(
        *device,
        descriptorLayoutBindings,
        shaderDisplayName);
}

void GraphicsPipeline::DestroyDescriptorSetLayouts()
{
    PipelineLayoutBuilder::DestroyDescriptorSetLayouts(*device, descriptorSetLayouts);
}

void GraphicsPipeline::CreatePipelineLayout()
{
    pipelineLayout = PipelineLayoutBuilder::CreatePipelineLayout(*device, descriptorSetLayouts, shaderDisplayName);
}

void GraphicsPipeline::DestroyPipelineLayout()
{
    PipelineLayoutBuilder::DestroyPipelineLayout(*device, pipelineLayout);
}

void GraphicsPipeline::CreateShader()
{
    std::string vertexShaderCode = CommonFunction::ReadFile(vertexSpvPath);
    std::string fragmentShaderCode = CommonFunction::ReadFile(fragmentSpvPath);

    vk::ShaderModule vertexShaderModule;
    vk::ShaderModuleCreateInfo vertexShaderModuleCreateInfo;
    vertexShaderModuleCreateInfo
        .setCodeSize(vertexShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(vertexShaderCode.data()));
    vk::Result result = device->createShaderModule(&vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule);
    VL::RequireVulkanPipelineSuccess(
        result,
        "Create vertex shader module",
        shaderDisplayName,
        "graphics pipeline");
    VulkanDebug::SetObjectName(*device, vertexShaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Vert: " + shaderDisplayName);

    vk::ShaderModule fragmentShaderModule;
    vk::ShaderModuleCreateInfo fragmentShaderModuleCreateInfo;
    fragmentShaderModuleCreateInfo
        .setCodeSize(fragmentShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(fragmentShaderCode.data()));
    result = device->createShaderModule(&fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule);
    VL::RequireVulkanPipelineSuccess(
        result,
        "Create fragment shader module",
        shaderDisplayName,
        "graphics pipeline");
    VulkanDebug::SetObjectName(*device, fragmentShaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Frag: " + shaderDisplayName);
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

void GraphicsPipeline::CreateGraphicsPipeline(
    vk::RenderPass& renderPass,
    vk::SampleCountFlagBits sampleCount,
    uint32_t colorAttachmentCount,
    const GraphicsPipelineStateDesc& pipelineStateDesc,
    bool bIsShadowPass)
{
    GraphicsPipelineBuildDesc buildDesc{
        *device,
        renderPass,
        pipelineLayout,
        shaderStages,
        vertexInputBindingDescription,
        vertexInputAttributeDescriptions,
        shaderDisplayName,
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
