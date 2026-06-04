#include "computePipeline.h"
#include "../commonFunction.h"
#include "../vulkanDebug.h"
#include "pipelineLayoutBuilder.h"
#include "shaderReflectionService.h"
#include "vulkanPipelineDiagnostics.h"

ComputePipeline::ComputePipeline(vk::Device* device, const std::string& shaderName)
{
    this->device = device;
    this->shaderName = shaderName;

    CreateShader();
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

void ComputePipeline::CreateShader()
{
    const std::string computeShaderPath = CommonFunction::Path(shaderName + "_comp.spv");
    std::string computeShaderCode = CommonFunction::ReadFile(computeShaderPath);

    vk::ShaderModuleCreateInfo shaderModuleCreateInfo;
    shaderModuleCreateInfo
        .setCodeSize(computeShaderCode.size())
        .setPCode(reinterpret_cast<const uint32_t*>(computeShaderCode.data()));

    vk::Result result = device->createShaderModule(&shaderModuleCreateInfo, nullptr, &shaderModule);
    VL::RequireVulkanPipelineSuccess(result, "Create shader module", shaderName, "compute pipeline");
    VulkanDebug::SetObjectName(*device, shaderModule, vk::ObjectType::eShaderModule, "ShaderModule_Comp: " + shaderName);

    shaderBindings = ShaderReflectionService::ReflectComputeFromDebugSpirv(shaderName);
}

void ComputePipeline::DestroyShader()
{
    device->destroyShaderModule(shaderModule, nullptr);
}

void ComputePipeline::CreateDescriptorSetLayouts()
{
    descriptorSetLayouts = PipelineLayoutBuilder::CreateDescriptorSetLayouts(*device, shaderBindings, shaderName);
}

void ComputePipeline::DestroyDescriptorSetLayouts()
{
    PipelineLayoutBuilder::DestroyDescriptorSetLayouts(*device, descriptorSetLayouts);
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
