#include "shaderReflect.h"
#include <iostream>
#include <Eigen/Dense>

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

    // 合并shaderModule的descriptorBindings
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

        // 检测set和binding, 相同的进行合并
        for(auto& descriptorBinding : shaderModuleDescriptorBindings)
        {
            bool found = false;
            for(auto& [existingBinding, existingStageFlags] : allDescriptorBindings)
            {
                if(existingBinding->set == descriptorBinding->set && existingBinding->binding == descriptorBinding->binding)
                {
                    found = true;
                    // 合并shaderStageFlags
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

    for(auto& [descriptorBinding, shaderStageFlags] : allDescriptorBindings)
    {
        int32_t set = descriptorBinding->set;
        int32_t binding = descriptorBinding->binding;
        int32_t size = descriptorBinding->block.size;
        int32_t offset = descriptorBinding->block.offset;
        SpvReflectDescriptorType type = descriptorBinding->descriptor_type;
        if(type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        {
            for(int32_t i = 0; i < descriptorBinding->block.member_count; i++)
            {
                const SpvReflectBlockVariable& member = descriptorBinding->block.members[i];
                int32_t membersize = member.size;
                int32_t memberoffset = member.offset;
                std::string memberName = member.name ? member.name : "Unknown";
                std::cout << "Member: " << memberName << " size: " << membersize << " offset: " << memberoffset << std::endl;
            }
        }
        int32_t descriptorCount = descriptorBinding->count;
        int32_t stageFlag = shaderStageFlags;
        std::string Name = descriptorBinding->name ? descriptorBinding->name : "Unknown";
        std::cout << "Descriptor binding: " << Name << " set: " << set << " binding: " << binding << " type: " << type << " count: " << descriptorCount << std::endl;
    }
    //push constants
    // uint32_t pushConstantBlocksCount = 0;
    // std::vector<SpvReflectBlockVariable*> pushConstantBlocks;
    // result = spvReflectEnumeratePushConstantBlocks(&shaderModule, &pushConstantBlocksCount, nullptr);
    // if (result != SPV_REFLECT_RESULT_SUCCESS)
    // {
    //     throw std::runtime_error("Failed to enumerate push constant blocks");
    // }
    // result = spvReflectEnumeratePushConstantBlocks(&shaderModule, &pushConstantBlocksCount, pushConstantBlocks.data());
    // if (result != SPV_REFLECT_RESULT_SUCCESS)
    // {
    //     throw std::runtime_error("Failed to enumerate push constant blocks");
    // }
    // for(auto& pushConstantBlock : pushConstantBlocks)
    // {
    //     int32_t offset = pushConstantBlock->offset;
    //     int32_t size = pushConstantBlock->size;
    //     int32_t stageFlag = shaderModule.shader_stage;
    //     std::string Name = pushConstantBlock->name ? pushConstantBlock->name : "Unknown";
    //     std::cout << "Push constant block: " << Name << " offset: " << offset << " size: " << size << std::endl;
    // }
    
    //     // vertex attributes
    //     uint32_t vertexInputCount = 0;
    //     std::vector<SpvReflectInterfaceVariable*> vertexInputs;
    //     result = spvReflectEnumerateInputVariables(&shaderModule, &vertexInputCount, nullptr);
    //     if (result != SPV_REFLECT_RESULT_SUCCESS)
    //     {
    //         throw std::runtime_error("Failed to enumerate vertex inputs");
    //     }
    //     vertexInputs.resize(vertexInputCount);
    //     result = spvReflectEnumerateInputVariables(&shaderModule, &vertexInputCount, vertexInputs.data());
    //     if (result != SPV_REFLECT_RESULT_SUCCESS)
    //     {
    //         throw std::runtime_error("Failed to enumerate vertex inputs");
    //     }
    //     for(auto& vertexInput : vertexInputs)
    //     {
    //         int32_t location = vertexInput->location;
    //         SpvReflectFormat format = vertexInput->format;
    //         int32_t count = vertexInput->array.dims_count ? vertexInput->array.dims[0] : 1;
    //         std::string Name = vertexInput->name ? vertexInput->name : "Unknown";
    //         std::cout << "Vertex input: " << Name << " location: " << location << " format: " << format << " count: " << count << std::endl;
    //     }
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
        }
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

vk::DescriptorType ShaderReflect::GetVulkanDescriptorType(SpvReflectDescriptorType type)
{
    switch (type)
    {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            return vk::DescriptorType::eSampler;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            return vk::DescriptorType::eCombinedImageSampler;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return vk::DescriptorType::eSampledImage;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return vk::DescriptorType::eStorageImage;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return vk::DescriptorType::eUniformTexelBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return vk::DescriptorType::eStorageTexelBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return vk::DescriptorType::eUniformBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return vk::DescriptorType::eStorageBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            return vk::DescriptorType::eUniformBufferDynamic;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return vk::DescriptorType::eStorageBufferDynamic;
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return vk::DescriptorType::eInputAttachment;
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return vk::DescriptorType::eAccelerationStructureKHR;
        default:
            throw std::runtime_error("Unknown descriptor type");
    }
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