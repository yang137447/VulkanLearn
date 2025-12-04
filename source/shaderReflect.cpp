#include "shaderReflect.h"
#include <iostream>
#include <Eigen/Dense>

ShaderReflect::ShaderReflect(const std::vector<uint32_t>& spirv)
{
    SpvReflectResult result= spvReflectCreateShaderModule(spirv.size() * sizeof(uint32_t), spirv.data(), &shaderModule);
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to create shader module");
    }
    uint32_t descriptorBindingsCount = 0;
    std::vector<SpvReflectDescriptorBinding*> descriptorBindings;
    result = spvReflectEnumerateDescriptorBindings(&shaderModule, &descriptorBindingsCount, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to enumerate descriptor bindings");
    }

    // bindings
    descriptorBindings.resize(descriptorBindingsCount);
    result = spvReflectEnumerateDescriptorBindings(&shaderModule, &descriptorBindingsCount, descriptorBindings.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to enumerate descriptor bindings");
    }
    for(auto& descriptorBinding : descriptorBindings)
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
        int32_t stageFlag = shaderModule.shader_stage;
        std::string Name = descriptorBinding->name ? descriptorBinding->name : "Unknown";
        std::cout << "Descriptor binding: " << Name << " set: " << set << " binding: " << binding << " type: " << type << " count: " << descriptorCount << std::endl;
    }

    //push constants
    uint32_t pushConstantBlocksCount = 0;
    std::vector<SpvReflectBlockVariable*> pushConstantBlocks;
    result = spvReflectEnumeratePushConstantBlocks(&shaderModule, &pushConstantBlocksCount, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to enumerate push constant blocks");
    }
    result = spvReflectEnumeratePushConstantBlocks(&shaderModule, &pushConstantBlocksCount, pushConstantBlocks.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to enumerate push constant blocks");
    }
    for(auto& pushConstantBlock : pushConstantBlocks)
    {
        int32_t offset = pushConstantBlock->offset;
        int32_t size = pushConstantBlock->size;
        int32_t stageFlag = shaderModule.shader_stage;
        std::string Name = pushConstantBlock->name ? pushConstantBlock->name : "Unknown";
        std::cout << "Push constant block: " << Name << " offset: " << offset << " size: " << size << std::endl;
    }
    
    // vertex attributes
    uint32_t vertexInputCount = 0;
    std::vector<SpvReflectInterfaceVariable*> vertexInputs;
    result = spvReflectEnumerateInputVariables(&shaderModule, &vertexInputCount, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to enumerate vertex inputs");
    }
    vertexInputs.resize(vertexInputCount);
    result = spvReflectEnumerateInputVariables(&shaderModule, &vertexInputCount, vertexInputs.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        throw std::runtime_error("Failed to enumerate vertex inputs");
    }
    for(auto& vertexInput : vertexInputs)
    {
        int32_t location = vertexInput->location;
        SpvReflectFormat format = vertexInput->format;
        int32_t count = vertexInput->array.dims_count ? vertexInput->array.dims[0] : 1;
        std::string Name = vertexInput->name ? vertexInput->name : "Unknown";
        std::cout << "Vertex input: " << Name << " location: " << location << " format: " << format << " count: " << count << std::endl;
    }
}

ShaderReflect::~ShaderReflect()
{
    spvReflectDestroyShaderModule(&shaderModule);
}

std::vector<ShaderBinding> ShaderReflect::GetShaderBindings()
{
    uint32_t descriptorBindingsCount = 0;
    std::vector<SpvReflectDescriptorBinding*> descriptorBindings;
    SpvReflectResult result = spvReflectEnumerateDescriptorBindings(&shaderModule, &descriptorBindingsCount, nullptr);
    // bindings
    descriptorBindings.resize(descriptorBindingsCount);
    result = spvReflectEnumerateDescriptorBindings(&shaderModule, &descriptorBindingsCount, descriptorBindings.data());
    std::vector<ShaderBinding> shaderBindings;
    for(auto& descriptorBinding : descriptorBindings)
    {
        ShaderBinding binding;
        binding.set = descriptorBinding->set;
        binding.binding = descriptorBinding->binding;
        binding.type = GetVulkanDescriptorType(descriptorBinding->descriptor_type);
        binding.stageFlags = GetVulkanShaderStage(shaderModule.shader_stage);
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

vk::ShaderStageFlagBits ShaderReflect::GetVulkanShaderStage(SpvReflectShaderStageFlagBits stage)
{
    switch (stage)
    {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
            return vk::ShaderStageFlagBits::eVertex;
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            return vk::ShaderStageFlagBits::eTessellationControl;
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            return vk::ShaderStageFlagBits::eTessellationEvaluation;
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:
            return vk::ShaderStageFlagBits::eGeometry;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
            return vk::ShaderStageFlagBits::eFragment;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
            return vk::ShaderStageFlagBits::eCompute;
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_NV:
            return vk::ShaderStageFlagBits::eTaskNV;
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_NV:
            return vk::ShaderStageFlagBits::eMeshNV;
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:
            return vk::ShaderStageFlagBits::eRaygenKHR;
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return vk::ShaderStageFlagBits::eAnyHitKHR;
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return vk::ShaderStageFlagBits::eClosestHitKHR;
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:
            return vk::ShaderStageFlagBits::eMissKHR;
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return vk::ShaderStageFlagBits::eIntersectionKHR;
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR:
            return vk::ShaderStageFlagBits::eCallableKHR;
        default:
            throw std::runtime_error("Unknown shader stage");
    }
}