#pragma once
#include <spirv_reflect.h>
#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

#include "shader/shaderAbiSignature.h"

struct ShaderBinding
{
    uint32_t set = 0;
    uint32_t binding = 0;
    vk::DescriptorType type = vk::DescriptorType::eSampler;
    uint32_t descriptorCount = 1;
    vk::ShaderStageFlags stageFlags;
    uint32_t memberCount = 0;
    uint32_t size = 0;
    std::vector<uint32_t> members;
    std::vector<uint32_t> memberOffsets;
    std::vector<std::string> memberNames;
    std::vector<std::string> memberTypes;
    std::string name;
};

inline bool HasSameShaderBindingLayout(
    const ShaderBinding& lhs,
    const ShaderBinding& rhs)
{
    return lhs.set == rhs.set &&
        lhs.binding == rhs.binding &&
        lhs.type == rhs.type &&
        lhs.descriptorCount == rhs.descriptorCount &&
        lhs.memberCount == rhs.memberCount &&
        lhs.size == rhs.size &&
        lhs.members == rhs.members &&
        lhs.memberOffsets == rhs.memberOffsets &&
        lhs.memberNames == rhs.memberNames &&
        lhs.memberTypes == rhs.memberTypes &&
        lhs.name == rhs.name;
}

class ShaderReflect
{
public:
    ShaderReflect(const std::vector<std::vector<uint32_t>>& spirvs);
    ShaderReflect(const ShaderReflect&) = delete;
    ShaderReflect(ShaderReflect&&) = delete;
    ~ShaderReflect();

    std::vector<ShaderBinding> GetShaderBindings();
    VL::ShaderAbiSignature GetShaderAbiSignature();
private:
    vk::ShaderStageFlags GetVulkanShaderStage(SpvReflectShaderStageFlagBits stage);

    std::vector<SpvReflectShaderModule> shaderModules;
    std::vector<std::tuple<SpvReflectDescriptorBinding*, SpvReflectShaderStageFlagBits>> allDescriptorBindings;
};
