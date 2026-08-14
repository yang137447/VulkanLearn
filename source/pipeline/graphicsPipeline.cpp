#include "graphicsPipeline.h"
#include "../vertexDataStruct.h"
#include <cstdint>
#include "graphicsPipelineBuilder.h"
#include "pipelineLayoutBuilder.h"
#include "render/backend/rendererBackendVulkan.h"
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
    VL::RendererBackendVulkan* rendererBackend,
    vk::Device& device,
    vk::RenderPass* renderPass,
    const GraphicsShaderVariantArtifact& shaderArtifact,
    vk::SampleCountFlagBits sampleCount,
    uint32_t colorAttachmentCount,
    const GraphicsPipelineStateDesc& pipelineStateDesc,
    bool bIsShadowPass,
    const GraphicsPipelineLayoutDesc& pipelineLayoutDesc)
{
    PROFILE_FUNCTION();
    this->rendererBackend = rendererBackend;
    this->device = &device;
    this->shaderDisplayName = shaderArtifact.displayName;
    this->vertexSpirv = shaderArtifact.vertexSpirv;
    this->fragmentSpirv = shaderArtifact.fragmentSpirv;
    this->shaderBindings = shaderArtifact.shaderBindings;

    try
    {
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
    catch (...)
    {
        DestroyGraphicsPipeline();
        DestroyDescriptorSetLayouts();
        DestroyShader();
        DestroyPipelineLayout();
        throw;
    }
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
        *rendererBackend,
        descriptorLayoutBindings,
        shaderDisplayName);
}

void GraphicsPipeline::DestroyDescriptorSetLayouts()
{
    PipelineLayoutBuilder::DestroyDescriptorSetLayouts(
        *rendererBackend,
        descriptorSetLayouts);
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
    vk::ShaderModule vertexShaderModule;
    vk::ShaderModuleCreateInfo vertexShaderModuleCreateInfo;
    vertexShaderModuleCreateInfo
        .setCodeSize(vertexSpirv.size() * sizeof(uint32_t))
        .setPCode(vertexSpirv.data());
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
        .setCodeSize(fragmentSpirv.size() * sizeof(uint32_t))
        .setPCode(fragmentSpirv.data());
    result = device->createShaderModule(&fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule);
    if (result != vk::Result::eSuccess)
    {
        device->destroyShaderModule(vertexShaderModule, nullptr);
        VL::RequireVulkanPipelineSuccess(
            result,
            "Create fragment shader module",
            shaderDisplayName,
            "graphics pipeline");
    }
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
    for (vk::PipelineShaderStageCreateInfo& stage : shaderStages)
    {
        if (stage.module)
        {
            device->destroyShaderModule(stage.module, nullptr);
            stage.module = nullptr;
        }
    }
    shaderStages.clear();
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
    auto buildResult =
        GraphicsPipelineBuilder::Build(*device, buildDesc);
    pipelineCache = buildResult.pipelineCache;
    graphicsPipeline = buildResult.graphicsPipeline;
}

void GraphicsPipeline::DestroyGraphicsPipeline()
{
    if (graphicsPipeline)
    {
        device->destroyPipeline(graphicsPipeline);
        graphicsPipeline = nullptr;
    }
    if (pipelineCache)
    {
        device->destroyPipelineCache(pipelineCache);
        pipelineCache = nullptr;
    }
}
