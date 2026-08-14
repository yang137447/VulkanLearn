#include "pipelineLayoutBuilder.h"
#include "render/backend/rendererBackendVulkan.h"
#include "../vulkanDebug.h"
#include "vulkanPipelineDiagnostics.h"

std::vector<vk::DescriptorSetLayout> PipelineLayoutBuilder::CreateDescriptorSetLayouts(
    VL::RendererBackendVulkan& rendererBackend,
    const std::vector<ShaderBinding>& shaderBindings,
    const std::string& pipelineName,
    uint32_t setCount)
{
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    descriptorSetLayouts.resize(setCount);
    try
    {
        for (uint32_t i = 0; i < setCount; i++)
        {
            std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
            for (const auto& binding : shaderBindings)
            {
                if (binding.set != i)
                {
                    continue;
                }
                vk::DescriptorSetLayoutBinding layoutBinding;
                layoutBinding
                    .setBinding(binding.binding)
                    .setDescriptorType(binding.type)
                    .setDescriptorCount(binding.descriptorCount)
                    .setStageFlags(binding.stageFlags)
                    .setPImmutableSamplers(nullptr);
                descriptorSetLayoutBindings.push_back(layoutBinding);
            }

            vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
            descriptorSetLayoutCreateInfo
                .setBindings(descriptorSetLayoutBindings);

            descriptorSetLayouts[i] =
                rendererBackend.CreateDescriptorSetLayout(
                    descriptorSetLayoutCreateInfo,
                    "SetLayout_" + std::to_string(i) +
                        ": " + pipelineName);
        }
    }
    catch (...)
    {
        DestroyDescriptorSetLayouts(
            rendererBackend,
            descriptorSetLayouts);
        throw;
    }
    return descriptorSetLayouts;
}

vk::PipelineLayout PipelineLayoutBuilder::CreatePipelineLayout(
    vk::Device& device,
    const std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts,
    const std::string& pipelineName)
{
    vk::PipelineLayout pipelineLayout;
    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo
        .setSetLayouts(descriptorSetLayouts);

    vk::Result result = device.createPipelineLayout(&pipelineLayoutCreateInfo, nullptr, &pipelineLayout);
    VL::RequireVulkanPipelineSuccess(result, "Create pipeline layout", pipelineName, "pipeline layout");
    VulkanDebug::SetObjectName(device, pipelineLayout, vk::ObjectType::ePipelineLayout, "PipelineLayout: " + pipelineName);
    return pipelineLayout;
}

void PipelineLayoutBuilder::DestroyDescriptorSetLayouts(
    VL::RendererBackendVulkan& rendererBackend,
    std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts)
{
    for (auto& layout : descriptorSetLayouts)
    {
        if (layout)
        {
            rendererBackend.DestroyDescriptorSetLayout(
                layout);
        }
    }
    descriptorSetLayouts.clear();
}

void PipelineLayoutBuilder::DestroyPipelineLayout(
    vk::Device& device,
    vk::PipelineLayout& pipelineLayout)
{
    if (pipelineLayout)
    {
        device.destroyPipelineLayout(pipelineLayout, nullptr);
        pipelineLayout = nullptr;
    }
}
