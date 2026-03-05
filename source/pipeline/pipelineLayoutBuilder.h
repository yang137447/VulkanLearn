#pragma once

#include <string>
#include <vector>
#include "../commonFunction.h"
#include "../shaderReflect.h"
#include "vulkan/vulkan.hpp"

class PipelineLayoutBuilder
{
public:
    static std::vector<vk::DescriptorSetLayout> CreateDescriptorSetLayouts(
        vk::Device& device,
        const std::vector<ShaderBinding>& shaderBindings,
        const std::string& pipelineName,
        uint32_t setCount = MAX_DESCRIPTOR_SETS);

    static vk::PipelineLayout CreatePipelineLayout(
        vk::Device& device,
        const std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts,
        const std::string& pipelineName);

    static void DestroyDescriptorSetLayouts(
        vk::Device& device,
        std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts);

    static void DestroyPipelineLayout(
        vk::Device& device,
        vk::PipelineLayout& pipelineLayout);
};
