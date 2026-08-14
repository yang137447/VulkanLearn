#include "shaderReflect.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace
{

std::string SafeName(const char* name)
{
    return name != nullptr ? name : "";
}

std::string DescribeScalarType(
    SpvReflectTypeFlags typeFlags,
    const SpvReflectNumericTraits& numeric)
{
    if ((typeFlags & SPV_REFLECT_TYPE_FLAG_BOOL) != 0)
    {
        return "bool";
    }

    if ((typeFlags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0)
    {
        if (numeric.scalar.width == 16)
        {
            return "float16_t";
        }
        if (numeric.scalar.width == 64)
        {
            return "double";
        }
        return "float";
    }

    if ((typeFlags & SPV_REFLECT_TYPE_FLAG_INT) != 0)
    {
        const bool isSigned = numeric.scalar.signedness != 0;
        if (numeric.scalar.width == 8)
        {
            return isSigned ? "int8_t" : "uint8_t";
        }
        if (numeric.scalar.width == 16)
        {
            return isSigned ? "int16_t" : "uint16_t";
        }
        if (numeric.scalar.width == 64)
        {
            return isSigned ? "int64_t" : "uint64_t";
        }
        return isSigned ? "int" : "uint";
    }

    return "unknown";
}

std::string DescribeType(const SpvReflectTypeDescription* typeDescription)
{
    if (typeDescription == nullptr)
    {
        return "unknown";
    }

    const SpvReflectTypeFlags typeFlags = typeDescription->type_flags;
    std::string type;
    if ((typeFlags & SPV_REFLECT_TYPE_FLAG_STRUCT) != 0)
    {
        type = "struct";
        if (typeDescription->type_name != nullptr)
        {
            type += ":" + std::string(typeDescription->type_name);
        }
    }
    else
    {
        const SpvReflectNumericTraits& numeric =
            typeDescription->traits.numeric;
        const std::string scalarType = DescribeScalarType(typeFlags, numeric);
        if ((typeFlags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0)
        {
            std::string matrixPrefix = "mat";
            if (scalarType == "double")
            {
                matrixPrefix = "dmat";
            }
            else if (scalarType == "float16_t")
            {
                matrixPrefix = "f16mat";
            }
            type = matrixPrefix +
                std::to_string(numeric.matrix.column_count) + "x" +
                std::to_string(numeric.matrix.row_count);
        }
        else if ((typeFlags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0)
        {
            std::string vectorPrefix;
            if (scalarType == "float")
            {
                vectorPrefix = "vec";
            }
            else if (scalarType == "double")
            {
                vectorPrefix = "dvec";
            }
            else if (scalarType == "int")
            {
                vectorPrefix = "ivec";
            }
            else if (scalarType == "uint")
            {
                vectorPrefix = "uvec";
            }
            else if (scalarType == "bool")
            {
                vectorPrefix = "bvec";
            }
            else
            {
                vectorPrefix = scalarType + "vec";
            }
            type = vectorPrefix +
                std::to_string(numeric.vector.component_count);
        }
        else
        {
            type = scalarType;
        }
    }

    const SpvReflectArrayTraits& array = typeDescription->traits.array;
    for (uint32_t dimensionIndex = 0;
         dimensionIndex < array.dims_count;
         ++dimensionIndex)
    {
        type += "[";
        if (array.dims[dimensionIndex] != SPV_REFLECT_ARRAY_DIM_RUNTIME)
        {
            type += std::to_string(array.dims[dimensionIndex]);
        }
        type += "]";
    }
    return type;
}

bool DescriptorDeclarationsMatch(
    const SpvReflectDescriptorBinding& lhs,
    const SpvReflectDescriptorBinding& rhs)
{
    if (lhs.descriptor_type != rhs.descriptor_type ||
        lhs.count != rhs.count ||
        lhs.block.size != rhs.block.size ||
        lhs.block.member_count != rhs.block.member_count ||
        SafeName(lhs.name) != SafeName(rhs.name))
    {
        return false;
    }

    for (uint32_t memberIndex = 0;
         memberIndex < lhs.block.member_count;
         ++memberIndex)
    {
        const SpvReflectBlockVariable& lhsMember = lhs.block.members[memberIndex];
        const SpvReflectBlockVariable& rhsMember = rhs.block.members[memberIndex];
        if (lhsMember.offset != rhsMember.offset ||
            lhsMember.size != rhsMember.size ||
            SafeName(lhsMember.name) != SafeName(rhsMember.name) ||
            DescribeType(lhsMember.type_description) !=
                DescribeType(rhsMember.type_description))
        {
            return false;
        }
    }
    return true;
}

void AppendAbiMembers(
    const SpvReflectBlockVariable& block,
    const std::string& parentName,
    std::vector<VL::ShaderAbiMember>& members)
{
    for (uint32_t memberIndex = 0;
         memberIndex < block.member_count;
         ++memberIndex)
    {
        const SpvReflectBlockVariable& reflectedMember =
            block.members[memberIndex];
        const std::string memberName = parentName.empty()
            ? SafeName(reflectedMember.name)
            : parentName + "." + SafeName(reflectedMember.name);

        VL::ShaderAbiMember member;
        member.name = memberName;
        member.offset = reflectedMember.absolute_offset;
        member.size = reflectedMember.size;
        member.type = DescribeType(reflectedMember.type_description);
        members.push_back(std::move(member));

        AppendAbiMembers(reflectedMember, memberName, members);
    }
}

void AppendInterfaceVariable(
    const SpvReflectInterfaceVariable& variable,
    const std::string& parentName,
    std::vector<VL::ShaderAbiInterfaceVariable>& output)
{
    if ((variable.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0 ||
        variable.built_in >= 0)
    {
        return;
    }

    const std::string variableName = parentName.empty()
        ? SafeName(variable.name)
        : parentName + "." + SafeName(variable.name);
    if (variable.member_count == 0)
    {
        VL::ShaderAbiInterfaceVariable abiVariable;
        abiVariable.location = variable.location;
        abiVariable.component = variable.component;
        abiVariable.format = static_cast<vk::Format>(variable.format);
        abiVariable.type = DescribeType(variable.type_description);
        abiVariable.name = variableName;
        output.push_back(std::move(abiVariable));
        return;
    }

    for (uint32_t memberIndex = 0;
         memberIndex < variable.member_count;
         ++memberIndex)
    {
        AppendInterfaceVariable(
            variable.members[memberIndex],
            variableName,
            output);
    }
}

} // namespace

ShaderReflect::ShaderReflect(const std::vector<std::vector<uint32_t>>& spirvs)
{
    for (const auto& spirv : spirvs)
    {
        SpvReflectShaderModule shaderModule;
        SpvReflectResult result = spvReflectCreateShaderModule(
            spirv.size() * sizeof(uint32_t),
            spirv.data(),
            &shaderModule);
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to create shader module");
        }
        shaderModules.emplace_back(shaderModule);
    }

    for (auto& shaderModule : shaderModules)
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

        for (auto& descriptorBinding : shaderModuleDescriptorBindings)
        {
            bool found = false;
            for (auto& [existingBinding, existingStageFlags] : allDescriptorBindings)
            {
                if (existingBinding->set == descriptorBinding->set &&
                    existingBinding->binding == descriptorBinding->binding)
                {
                    if (!DescriptorDeclarationsMatch(
                            *existingBinding,
                            *descriptorBinding))
                    {
                        throw std::runtime_error(
                            "Shader stages declare incompatible descriptor layouts at Set " +
                            std::to_string(descriptorBinding->set) +
                            " Binding " +
                            std::to_string(descriptorBinding->binding));
                    }
                    found = true;
                    existingStageFlags = static_cast<SpvReflectShaderStageFlagBits>(
                        existingStageFlags | shaderModule.shader_stage);
                }
            }
            if (!found)
            {
                allDescriptorBindings.emplace_back(descriptorBinding, shaderModule.shader_stage);
            }
        }
    }
}

ShaderReflect::~ShaderReflect()
{
    for (auto& shaderModule : shaderModules)
    {
        spvReflectDestroyShaderModule(&shaderModule);
    }
}

std::vector<ShaderBinding> ShaderReflect::GetShaderBindings()
{
    std::vector<ShaderBinding> shaderBindings;
    for (auto& [descriptorBinding, shaderStageFlags] : allDescriptorBindings)
    {
        ShaderBinding binding;
        binding.set = descriptorBinding->set;
        binding.binding = descriptorBinding->binding;
        binding.type = static_cast<vk::DescriptorType>(descriptorBinding->descriptor_type);
        binding.descriptorCount = descriptorBinding->count;
        binding.stageFlags = GetVulkanShaderStage(shaderStageFlags);
        binding.memberCount = descriptorBinding->block.member_count;
        binding.size = descriptorBinding->block.size;
        for (uint32_t i = 0; i < descriptorBinding->block.member_count; ++i)
        {
            const SpvReflectBlockVariable& member = descriptorBinding->block.members[i];
            binding.members.push_back(member.size);
            binding.memberOffsets.push_back(member.offset);
            binding.memberNames.push_back(SafeName(member.name));
            binding.memberTypes.push_back(DescribeType(member.type_description));
        }
        binding.name = SafeName(descriptorBinding->name);
        shaderBindings.push_back(binding);
    }
    std::sort(
        shaderBindings.begin(),
        shaderBindings.end(),
        [](const ShaderBinding& lhs, const ShaderBinding& rhs)
        {
            if (lhs.set != rhs.set)
            {
                return lhs.set < rhs.set;
            }
            return lhs.binding < rhs.binding;
        });
    return shaderBindings;
}

VL::ShaderAbiSignature ShaderReflect::GetShaderAbiSignature()
{
    VL::ShaderAbiSignature signature;
    for (auto& [descriptorBinding, shaderStageFlags] : allDescriptorBindings)
    {
        VL::ShaderAbiDescriptor descriptor;
        descriptor.set = descriptorBinding->set;
        descriptor.binding = descriptorBinding->binding;
        descriptor.type =
            static_cast<vk::DescriptorType>(descriptorBinding->descriptor_type);
        descriptor.count = descriptorBinding->count;
        descriptor.stageFlags = GetVulkanShaderStage(shaderStageFlags);
        descriptor.name = SafeName(descriptorBinding->name);
        descriptor.blockSize = descriptorBinding->block.size;
        AppendAbiMembers(descriptorBinding->block, "", descriptor.members);
        signature.descriptors.push_back(std::move(descriptor));
    }

    for (SpvReflectShaderModule& shaderModule : shaderModules)
    {
        const vk::ShaderStageFlags stageFlags =
            GetVulkanShaderStage(shaderModule.shader_stage);

        uint32_t pushConstantCount = 0;
        SpvReflectResult result = spvReflectEnumeratePushConstantBlocks(
            &shaderModule,
            &pushConstantCount,
            nullptr);
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to enumerate push constant blocks");
        }
        std::vector<SpvReflectBlockVariable*> pushConstants(pushConstantCount);
        result = spvReflectEnumeratePushConstantBlocks(
            &shaderModule,
            &pushConstantCount,
            pushConstants.data());
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to enumerate push constant blocks");
        }
        for (SpvReflectBlockVariable* reflectedBlock : pushConstants)
        {
            VL::ShaderAbiPushConstant candidate;
            candidate.offset = reflectedBlock->offset;
            candidate.size = reflectedBlock->size;
            candidate.stageFlags = stageFlags;
            AppendAbiMembers(*reflectedBlock, "", candidate.members);

            bool merged = false;
            for (VL::ShaderAbiPushConstant& existing : signature.pushConstants)
            {
                if (existing.offset != candidate.offset ||
                    existing.size != candidate.size)
                {
                    continue;
                }
                VL::ShaderAbiPushConstant existingWithoutStages = existing;
                VL::ShaderAbiPushConstant candidateWithoutStages = candidate;
                existingWithoutStages.stageFlags = {};
                candidateWithoutStages.stageFlags = {};
                if (!(existingWithoutStages == candidateWithoutStages))
                {
                    throw std::runtime_error(
                        "Shader stages declare incompatible push constant layouts");
                }
                existing.stageFlags |= candidate.stageFlags;
                merged = true;
                break;
            }
            if (!merged)
            {
                signature.pushConstants.push_back(std::move(candidate));
            }
        }

        if (shaderModule.shader_stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT)
        {
            uint32_t inputCount = 0;
            result = spvReflectEnumerateInputVariables(
                &shaderModule,
                &inputCount,
                nullptr);
            if (result != SPV_REFLECT_RESULT_SUCCESS)
            {
                throw std::runtime_error("Failed to enumerate vertex inputs");
            }
            std::vector<SpvReflectInterfaceVariable*> inputs(inputCount);
            result = spvReflectEnumerateInputVariables(
                &shaderModule,
                &inputCount,
                inputs.data());
            if (result != SPV_REFLECT_RESULT_SUCCESS)
            {
                throw std::runtime_error("Failed to enumerate vertex inputs");
            }
            for (SpvReflectInterfaceVariable* input : inputs)
            {
                AppendInterfaceVariable(*input, "", signature.vertexInputs);
            }
        }

        if (shaderModule.shader_stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT)
        {
            uint32_t outputCount = 0;
            result = spvReflectEnumerateOutputVariables(
                &shaderModule,
                &outputCount,
                nullptr);
            if (result != SPV_REFLECT_RESULT_SUCCESS)
            {
                throw std::runtime_error("Failed to enumerate fragment outputs");
            }
            std::vector<SpvReflectInterfaceVariable*> outputs(outputCount);
            result = spvReflectEnumerateOutputVariables(
                &shaderModule,
                &outputCount,
                outputs.data());
            if (result != SPV_REFLECT_RESULT_SUCCESS)
            {
                throw std::runtime_error("Failed to enumerate fragment outputs");
            }
            for (SpvReflectInterfaceVariable* output : outputs)
            {
                AppendInterfaceVariable(
                    *output,
                    "",
                    signature.fragmentOutputs);
            }
        }

        uint32_t specializationConstantCount = 0;
        result = spvReflectEnumerateSpecializationConstants(
            &shaderModule,
            &specializationConstantCount,
            nullptr);
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error(
                "Failed to enumerate specialization constants");
        }
        std::vector<SpvReflectSpecializationConstant*> specializationConstants(
            specializationConstantCount);
        result = spvReflectEnumerateSpecializationConstants(
            &shaderModule,
            &specializationConstantCount,
            specializationConstants.data());
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            throw std::runtime_error(
                "Failed to enumerate specialization constants");
        }
        for (SpvReflectSpecializationConstant* reflectedConstant :
             specializationConstants)
        {
            VL::ShaderAbiSpecializationConstant candidate;
            candidate.constantId = reflectedConstant->constant_id;
            candidate.type = DescribeType(reflectedConstant->type_description);
            candidate.name = SafeName(reflectedConstant->name);

            bool found = false;
            for (const VL::ShaderAbiSpecializationConstant& existing :
                 signature.specializationConstants)
            {
                if (existing.constantId != candidate.constantId)
                {
                    continue;
                }
                if (!(existing == candidate))
                {
                    throw std::runtime_error(
                        "Shader stages declare incompatible specialization constant ID " +
                        std::to_string(candidate.constantId));
                }
                found = true;
                break;
            }
            if (!found)
            {
                signature.specializationConstants.push_back(
                    std::move(candidate));
            }
        }

        if (shaderModule.shader_stage ==
                SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT &&
            shaderModule.entry_point_count > 0)
        {
            const SpvReflectEntryPoint& entryPoint =
                shaderModule.entry_points[0];
            signature.workgroupSize.x = entryPoint.local_size.x;
            signature.workgroupSize.y = entryPoint.local_size.y;
            signature.workgroupSize.z = entryPoint.local_size.z;
            signature.workgroupSize.present = true;
        }
    }

    signature.Normalize();
    return signature;
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
