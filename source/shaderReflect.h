#pragma once
#include <spirv_reflect.h>
#include <vulkan/vulkan.hpp>
#include <string>

struct ShaderBinding
{
    uint32_t set;
    uint32_t binding;
    vk::DescriptorType type;
    vk::ShaderStageFlags stageFlags;
    uint32_t memberCount;
    uint32_t size;
    std::vector<uint32_t> members;
    std::vector<std::string> memberNames;
    std::string name;
};

class ShaderReflect
{
public:
    ShaderReflect(const std::vector<std::vector<uint32_t>>& spirvs);
    ShaderReflect(const ShaderReflect&) = delete;
    ShaderReflect(ShaderReflect&&) = delete;
    ~ShaderReflect();

    std::vector<ShaderBinding> GetShaderBindings();
private:
    vk::ShaderStageFlags GetVulkanShaderStage(SpvReflectShaderStageFlagBits stage);

    std::vector<SpvReflectShaderModule> shaderModules;
    std::vector<std::tuple<SpvReflectDescriptorBinding*, SpvReflectShaderStageFlagBits>> allDescriptorBindings;
};
