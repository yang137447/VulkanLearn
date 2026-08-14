#include "computePipeline.h"
#include "../commonFunction.h"
#include "../vulkanDebug.h"
#include "pipelineLayoutBuilder.h"
#include "render/backend/rendererBackendVulkan.h"
#include "shaderReflectionService.h"
#include "vulkanPipelineDiagnostics.h"

ComputePipeline::ComputePipeline(
    VL::RendererBackendVulkan* rendererBackend,
    vk::Device& device,
    const std::string& shaderName)
{
    this->rendererBackend = rendererBackend;
    this->device = &device;
    this->shaderName = shaderName;

    const std::string computeShaderPath = CommonFunction::Path(shaderName + "_comp.spv");
    std::string computeShaderCode = CommonFunction::ReadFile(computeShaderPath);
    std::vector<uint32_t> spirv(
        reinterpret_cast<const uint32_t*>(computeShaderCode.data()),
        reinterpret_cast<const uint32_t*>(
            computeShaderCode.data() + computeShaderCode.size()));

    shaderBindings = ShaderReflectionService::ReflectComputeFromDebugSpirv(shaderName);
    CreateShader(spirv);
    CreateDescriptorSetLayouts();
    CreatePipelineLayout();
    CreateComputePipeline();
}

ComputePipeline::ComputePipeline(
    VL::RendererBackendVulkan* rendererBackend,
    vk::Device& device,
    const ComputeShaderArtifact& artifact)
{
    this->rendererBackend = rendererBackend;
    this->device = &device;
    this->shaderName = artifact.shaderName;
    shaderBindings = artifact.shaderBindings;

    CreateShader(artifact.runtimeSpirv);
    CreateDescriptorSetLayouts();
    CreatePipelineLayout();
    CreateComputePipeline();
}

ComputePipeline::~ComputePipeline()
{
    DestroyComputePipeline();
    DestroyDescriptorSetLayouts();
    DestroyShader();
    DestroyPipelineLayout();
}

void ComputePipeline::CreateShader(const std::vector<uint32_t>& spirv)
{
    vk::ShaderModuleCreateInfo shaderModuleCreateInfo;
    shaderModuleCreateInfo
        .setCodeSize(spirv.size() * sizeof(uint32_t))
        .setPCode(spirv.data());

    vk::Result result = device->createShaderModule(&shaderModuleCreateInfo, nullptr, &shaderModule);
    VL::RequireVulkanPipelineSuccess(result, "Create shader module", shaderName, "compute pipeline");
    VulkanDebug::SetObjectName(*device, shaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Comp: " + shaderName);

}

void ComputePipeline::DestroyShader()
{
    device->destroyShaderModule(shaderModule, nullptr);
}

void ComputePipeline::CreateDescriptorSetLayouts()
{
    descriptorSetLayouts =
        PipelineLayoutBuilder::CreateDescriptorSetLayouts(
            *rendererBackend,
            shaderBindings,
            shaderName);
}

void ComputePipeline::DestroyDescriptorSetLayouts()
{
    PipelineLayoutBuilder::DestroyDescriptorSetLayouts(
        *rendererBackend,
        descriptorSetLayouts);
}

void ComputePipeline::CreatePipelineLayout()
{
    pipelineLayout = PipelineLayoutBuilder::CreatePipelineLayout(*device, descriptorSetLayouts, shaderName);
}

void ComputePipeline::DestroyPipelineLayout()
{
    PipelineLayoutBuilder::DestroyPipelineLayout(*device, pipelineLayout);
}

void ComputePipeline::CreateComputePipeline()
{
    vk::PipelineShaderStageCreateInfo stageCreateInfo;
    stageCreateInfo
        .setStage(vk::ShaderStageFlagBits::eCompute)
        .setModule(shaderModule)
        .setPName("main");

    vk::ComputePipelineCreateInfo pipelineCreateInfo;
    pipelineCreateInfo
        .setStage(stageCreateInfo)
        .setLayout(pipelineLayout);

    vk::PipelineCacheCreateInfo pipelineCacheCreateInfo;
    pipelineCacheCreateInfo
        .setInitialDataSize(0)
        .setPInitialData(nullptr);

    vk::Result result = device->createPipelineCache(&pipelineCacheCreateInfo, nullptr, &pipelineCache);
    VL::RequireVulkanPipelineSuccess(result, "Create pipeline cache", shaderName, "compute pipeline");
    VulkanDebug::SetObjectName(*device, pipelineCache, vk::ObjectType::ePipelineCache, "ComputePipelineCache: " + shaderName);

    result = device->createComputePipelines(pipelineCache, 1, &pipelineCreateInfo, nullptr, &computePipeline);
    VL::RequireVulkanPipelineSuccess(result, "Create compute pipeline", shaderName, "compute pipeline");
    VulkanDebug::SetObjectName(*device, computePipeline, vk::ObjectType::ePipeline, "ComputePipeline: " + shaderName);
}

void ComputePipeline::DestroyComputePipeline()
{
    device->destroyPipeline(computePipeline, nullptr);
    device->destroyPipelineCache(pipelineCache, nullptr);
}

void ComputePipeline::Bind(vk::CommandBuffer commandBuffer) const
{
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, computePipeline);
}

void ComputePipeline::Dispatch(vk::CommandBuffer commandBuffer, uint32_t groupX, uint32_t groupY, uint32_t groupZ) const
{
    commandBuffer.dispatch(groupX, groupY, groupZ);
}
