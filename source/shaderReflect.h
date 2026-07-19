#pragma once
#include <spirv_reflect.h>
#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

struct ShaderBinding
{
    uint32_t set;
    uint32_t binding;
    vk::DescriptorType type;
    vk::ShaderStageFlags stageFlags;
    uint32_t memberCount;
    uint32_t size;
    std::vector<uint32_t> members;
    std::vector<uint32_t> memberOffsets;
    std::vector<std::string> memberNames;
    std::string name;
};

inline bool HasSameShaderBindingLayout(
    const ShaderBinding& lhs,
    const ShaderBinding& rhs)
{
    return lhs.set == rhs.set &&
        lhs.binding == rhs.binding &&
        lhs.type == rhs.type &&
        lhs.memberCount == rhs.memberCount &&
        lhs.size == rhs.size &&
        lhs.members == rhs.members &&
        lhs.memberOffsets == rhs.memberOffsets &&
        lhs.memberNames == rhs.memberNames &&
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
private:
    vk::ShaderStageFlags GetVulkanShaderStage(SpvReflectShaderStageFlagBits stage);

    std::vector<SpvReflectShaderModule> shaderModules;
    std::vector<std::tuple<SpvReflectDescriptorBinding*, SpvReflectShaderStageFlagBits>> allDescriptorBindings;
};
