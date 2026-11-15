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
#include <stdexcept>

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
    this->hasTessellation = shaderArtifact.hasTessellation;
    this->tessControlSpirv = shaderArtifact.tessControlSpirv;
    this->tessEvaluationSpirv = shaderArtifact.tessEvaluationSpirv;
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
    shaderStages.clear();
    try
    {
        auto createStageModule = [](
            vk::Device& device,
            const std::vector<uint32_t>& spirv,
            const char* action,
            const std::string& displayName,
            const char* debugPrefix) {
            vk::ShaderModuleCreateInfo createInfo;
            createInfo
                .setCodeSize(spirv.size() * sizeof(uint32_t))
                .setPCode(spirv.data());
            vk::ShaderModule module;
            const vk::Result result = device.createShaderModule(
                &createInfo,
                nullptr,
                &module);
            VL::RequireVulkanPipelineSuccess(
                result,
                action,
                displayName,
                "graphics pipeline");
            VulkanDebug::SetObjectName(
                device,
                module,
                vk::ObjectType::eShaderModule,
                debugPrefix + displayName);
            return module;
        };
        auto appendStage = [this](
            vk::ShaderStageFlagBits stage,
            vk::ShaderModule module) {
            vk::PipelineShaderStageCreateInfo stageInfo;
            stageInfo
                .setStage(stage)
                .setModule(module)
                .setPName("main")
                .setPSpecializationInfo(nullptr);
            shaderStages.push_back(stageInfo);
        };

        appendStage(
            vk::ShaderStageFlagBits::eVertex,
            createStageModule(
                *device,
                vertexSpirv,
                "Create vertex shader module",
                shaderDisplayName,
                "ShaderModule_Vert: "));
        appendStage(
            vk::ShaderStageFlagBits::eFragment,
            createStageModule(
                *device,
                fragmentSpirv,
                "Create fragment shader module",
                shaderDisplayName,
                "ShaderModule_Frag: "));
        if (hasTessellation)
        {
            if (tessControlSpirv.empty() ||
                tessEvaluationSpirv.empty())
            {
                throw std::runtime_error(
                    "Tessellation shader variant is missing tesc/tese SPIR-V: " +
                    shaderDisplayName);
            }
            appendStage(
                vk::ShaderStageFlagBits::eTessellationControl,
                createStageModule(
                    *device,
                    tessControlSpirv,
                    "Create tessellation control shader module",
                    shaderDisplayName,
                    "ShaderModule_TessControl: "));
            appendStage(
                vk::ShaderStageFlagBits::eTessellationEvaluation,
                createStageModule(
                    *device,
                    tessEvaluationSpirv,
                    "Create tessellation evaluation shader module",
                    shaderDisplayName,
                    "ShaderModule_TessEval: "));
        }
    }
    catch (...)
    {
        // 已创建的模块都挂在 shaderStages 上，统一按现有销毁路径释放。
        DestroyShader();
        throw;
    }
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
    vertexInputBindingDescriptions =
        VertexInfo::vertexInputBindingDescriptions;
    
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
        vertexInputBindingDescriptions,
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
