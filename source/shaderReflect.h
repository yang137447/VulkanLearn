#pragma once
#include <spirv_reflect.h>
#include <vulkan/vulkan.hpp>

struct ShaderBinding
{
    uint32_t set;
    uint32_t binding;
    vk::DescriptorType type;
    vk::ShaderStageFlagBits stageFlags;
    uint32_t memberCount;
    uint32_t size;
    std::vector<uint32_t> members;
};

class ShaderReflect
{
public:
    ShaderReflect(const std::vector<uint32_t>& spirv);
    ShaderReflect(const ShaderReflect&) = delete;
    ShaderReflect(ShaderReflect&&) = delete;
    ~ShaderReflect();

    std::vector<ShaderBinding> GetShaderBindings();
private:
    ShaderReflect();
    vk::DescriptorType GetVulkanDescriptorType(SpvReflectDescriptorType type);
    vk::ShaderStageFlagBits GetVulkanShaderStage(SpvReflectShaderStageFlagBits stage);

    SpvReflectShaderModule shaderModule;
};