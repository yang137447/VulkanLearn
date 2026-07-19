#include "shaderReflect.h"

ShaderReflect::ShaderReflect(const std::vector<std::vector<uint32_t>>& spirvs)
{
    for(auto& spirv : spirvs)
    {
        SpvReflectShaderModule shaderModule;
        SpvReflectResult result= spvReflectCreateShaderModule(spirv.size() * sizeof(uint32_t), spirv.data(), &shaderModule);
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to create shader module");
        }
        shaderModules.emplace_back(shaderModule);
    }

    for(auto& shaderModule : shaderModules)
    {
        uint32_t descriptorBindingsCount = 0;
        std::vector<SpvReflectDescriptorBinding*> shaderModuleDescriptorBindings;
        SpvReflectResult result = spvReflectEnumerateDescriptorBindings(&shaderModule, &descriptorBindingsCount, nullptr);
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to enumerate descriptor bindings");
        }
        shaderModuleDescriptorBindings.resize(descriptorBindingsCount);
        result = spvReflectEnumerateDescriptorBindings(&shaderModule, &descriptorBindingsCount, shaderModuleDescriptorBindings.data());
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to enumerate descriptor bindings");
        }

        for(auto& descriptorBinding : shaderModuleDescriptorBindings)
        {
            bool found = false;
            for(auto& [existingBinding, existingStageFlags] : allDescriptorBindings)
            {
                if(existingBinding->set == descriptorBinding->set && existingBinding->binding == descriptorBinding->binding)
                {
                    found = true;
                    existingStageFlags = static_cast<SpvReflectShaderStageFlagBits>(
                        existingStageFlags | shaderModule.shader_stage);
                }
            }
            if(!found)
            {
                allDescriptorBindings.emplace_back(descriptorBinding, shaderModule.shader_stage);
            }
        }
    }
}

ShaderReflect::~ShaderReflect()
{
    for(auto& shaderModule : shaderModules)
    {
        spvReflectDestroyShaderModule(&shaderModule);
    }
}

std::vector<ShaderBinding> ShaderReflect::GetShaderBindings()
{
    std::vector<ShaderBinding> shaderBindings;
    for(auto& [descriptorBinding, shaderStageFlags] : allDescriptorBindings)
    {
        ShaderBinding binding;
        binding.set = descriptorBinding->set;
        binding.binding = descriptorBinding->binding;
        binding.type = static_cast<vk::DescriptorType>(descriptorBinding->descriptor_type);
        binding.stageFlags = GetVulkanShaderStage(shaderStageFlags);
        binding.memberCount = descriptorBinding->block.member_count;
        binding.size = descriptorBinding->block.size;
        for(int32_t i = 0; i < descriptorBinding->block.member_count; i++)
        {
            const SpvReflectBlockVariable& member = descriptorBinding->block.members[i];
            binding.members.push_back(member.size);
            binding.memberOffsets.push_back(member.offset);
            binding.memberNames.push_back(member.name ? member.name : "UnknownMember");
        }
        binding.name = descriptorBinding->name ? descriptorBinding->name : "Unknown";
        shaderBindings.push_back(binding);
    }
    // 按set和binding排序
    std::sort(shaderBindings.begin(), shaderBindings.end(), [](const ShaderBinding& a, const ShaderBinding& b) {
        if (a.set == b.set) {
            return a.binding < b.binding;
        }
        return a.set < b.set;
    });
    return shaderBindings;
}

vk::ShaderStageFlags ShaderReflect::GetVulkanShaderStage(SpvReflectShaderStageFlagBits stage)
{
    vk::ShaderStageFlags stageFlags;
    if(stage & SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
    {
        stageFlags |= vk::ShaderStageFlagBits::eTessellationControl;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
    {
        stageFlags |= vk::ShaderStageFlagBits::eTessellationEvaluation;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT)
    {
        stageFlags |= vk::ShaderStageFlagBits::eGeometry;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_VERTEX_BIT)
    {
        stageFlags |= vk::ShaderStageFlagBits::eVertex;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT)
    {
        stageFlags |= vk::ShaderStageFlagBits::eFragment;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT)
    {
        stageFlags |= vk::ShaderStageFlagBits::eCompute;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_TASK_BIT_NV)
    {
        stageFlags |= vk::ShaderStageFlagBits::eTaskNV;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_MESH_BIT_NV)
    {
        stageFlags |= vk::ShaderStageFlagBits::eMeshNV;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR)
    {
        stageFlags |= vk::ShaderStageFlagBits::eRaygenKHR;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR)
    {
        stageFlags |= vk::ShaderStageFlagBits::eAnyHitKHR;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
    {
        stageFlags |= vk::ShaderStageFlagBits::eClosestHitKHR;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR)
    {
        stageFlags |= vk::ShaderStageFlagBits::eMissKHR;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR)
    {
        stageFlags |= vk::ShaderStageFlagBits::eIntersectionKHR;
    }
    if(stage & SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR)
    {
        stageFlags |= vk::ShaderStageFlagBits::eCallableKHR;
    }
    return stageFlags;
}
