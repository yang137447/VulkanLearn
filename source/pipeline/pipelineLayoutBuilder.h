#pragma once

#include <string>
#include <vector>
#include "../commonFunction.h"
#include "../shaderReflect.h"
#include "vulkan/vulkan.hpp"

namespace VL
{
class RendererBackendVulkan;
}

class PipelineLayoutBuilder
{
public:
    static std::vector<vk::DescriptorSetLayout> CreateDescriptorSetLayouts(
        VL::RendererBackendVulkan& rendererBackend,
        const std::vector<ShaderBinding>& shaderBindings,
        const std::string& pipelineName,
        uint32_t setCount = MAX_DESCRIPTOR_SETS);

    static vk::PipelineLayout CreatePipelineLayout(
        vk::Device& device,
        const std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts,
        const std::string& pipelineName);

    static void DestroyDescriptorSetLayouts(
        VL::RendererBackendVulkan& rendererBackend,
        std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts);

    static void DestroyPipelineLayout(
        vk::Device& device,
        vk::PipelineLayout& pipelineLayout);
};
