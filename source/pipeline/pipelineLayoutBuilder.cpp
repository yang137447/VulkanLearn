#include "pipelineLayoutBuilder.h"
#include "../vulkanDebug.h"
#include "vulkanPipelineDiagnostics.h"

std::vector<vk::DescriptorSetLayout> PipelineLayoutBuilder::CreateDescriptorSetLayouts(
    vk::Device& device,
    const std::vector<ShaderBinding>& shaderBindings,
    const std::string& pipelineName,
    uint32_t setCount)
{
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    descriptorSetLayouts.resize(setCount);
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
                .setDescriptorCount(1)
                .setStageFlags(binding.stageFlags)
                .setPImmutableSamplers(nullptr);
            descriptorSetLayoutBindings.push_back(layoutBinding);
        }

        vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
        descriptorSetLayoutCreateInfo
            .setBindings(descriptorSetLayoutBindings);

        vk::Result result = device.createDescriptorSetLayout(&descriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayouts[i]);
        VL::RequireVulkanPipelineSuccess(
            result,
            "Create descriptor set layout " + std::to_string(i),
            pipelineName,
            "pipeline layout");
        VulkanDebug::SetObjectName(device, descriptorSetLayouts[i], vk::ObjectType::eDescriptorSetLayout, "SetLayout_" + std::to_string(i) + ": " + pipelineName);
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
    vk::Device& device,
    std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts)
{
    for (auto& layout : descriptorSetLayouts)
    {
        device.destroyDescriptorSetLayout(layout, nullptr);
    }
}

void PipelineLayoutBuilder::DestroyPipelineLayout(
    vk::Device& device,
    vk::PipelineLayout& pipelineLayout)
{
    device.destroyPipelineLayout(pipelineLayout, nullptr);
}
