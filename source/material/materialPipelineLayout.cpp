#include "material/materialPipelineLayout.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "material/materialDescriptorSchema.h"
#include "renderGraph.h"

namespace VL
{
namespace
{

const ShaderBinding* FindReflectedBinding(
    const std::vector<ShaderBinding>& shaderBindings,
    uint32_t set,
    uint32_t binding)
{
    for (const ShaderBinding& shaderBinding : shaderBindings)
    {
        if (shaderBinding.set == set && shaderBinding.binding == binding)
        {
            return &shaderBinding;
        }
    }
    return nullptr;
}

void ValidatePassInputDescriptor(
    const CompiledRenderGraphPassInputDescriptor& inputDescriptor,
    const ShaderBinding* reflectedBinding,
    const std::string& passName)
{
    if (reflectedBinding == nullptr)
    {
        return;
    }
    if (reflectedBinding->type != vk::DescriptorType::eCombinedImageSampler ||
        reflectedBinding->descriptorCount != 1)
    {
        throw std::runtime_error(
            "Material pass input must be a single combined image sampler: " +
            passName + " binding " + std::to_string(inputDescriptor.binding));
    }
}

} // namespace

GraphicsPipelineLayoutDesc BuildMaterialSurfacePipelineLayout(
    const Renderpass& renderPass,
    const MaterialDescriptorSchema& descriptorSchema,
    const std::vector<ShaderBinding>& reflectedShaderBindings)
{
    GraphicsPipelineLayoutDesc layoutDesc;
    layoutDesc.overrideSets[MaterialSetIndex] = true;
    layoutDesc.setBindings[MaterialSetIndex] =
        descriptorSchema.GetSetBindings();

    // Set 3 的物理布局由 RenderGraph 输入集合决定，而不是由当前 shader
    // 反射出的“实际使用子集”决定。这样 ThinTranslucent 等非 Hair variant
    // 也能与同一个 forwardTransparent descriptor set 兼容。
    layoutDesc.overrideSets[PassSetIndex] = true;
    for (const CompiledRenderGraphPassInputDescriptor& inputDescriptor :
         renderPass.inputDescriptorPlan)
    {
        for (const ShaderBinding& existingBinding :
             layoutDesc.setBindings[PassSetIndex])
        {
            if (existingBinding.binding == inputDescriptor.binding)
            {
                throw std::runtime_error(
                    "RenderGraph pass has conflicting descriptor inputs at binding " +
                    std::to_string(inputDescriptor.binding) + ": " + renderPass.name);
            }
        }

        const ShaderBinding* reflectedBinding = FindReflectedBinding(
            reflectedShaderBindings,
            PassSetIndex,
            inputDescriptor.binding);
        ValidatePassInputDescriptor(
            inputDescriptor,
            reflectedBinding,
            renderPass.name);

        ShaderBinding layoutBinding;
        layoutBinding.set = PassSetIndex;
        layoutBinding.binding = inputDescriptor.binding;
        layoutBinding.type = vk::DescriptorType::eCombinedImageSampler;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = reflectedBinding != nullptr &&
            reflectedBinding->stageFlags != vk::ShaderStageFlags{}
            ? reflectedBinding->stageFlags
            : vk::ShaderStageFlagBits::eFragment;
        layoutBinding.name = reflectedBinding != nullptr &&
            !reflectedBinding->name.empty()
            ? reflectedBinding->name
            : inputDescriptor.resource;
        layoutDesc.setBindings[PassSetIndex].push_back(
            std::move(layoutBinding));
    }

    for (const ShaderBinding& reflectedBinding : reflectedShaderBindings)
    {
        if (reflectedBinding.set != PassSetIndex)
        {
            continue;
        }
        const CompiledRenderGraphPassInputDescriptor* inputDescriptor = nullptr;
        for (const CompiledRenderGraphPassInputDescriptor& candidateInput :
             renderPass.inputDescriptorPlan)
        {
            if (candidateInput.binding == reflectedBinding.binding)
            {
                inputDescriptor = &candidateInput;
                break;
            }
        }
        if (inputDescriptor == nullptr)
        {
            throw std::runtime_error(
                "Material pass shader declares an undeclared RenderGraph input: " +
                renderPass.name + " binding " +
                std::to_string(reflectedBinding.binding));
        }
        ValidatePassInputDescriptor(
            *inputDescriptor,
            &reflectedBinding,
            renderPass.name);
    }

    return layoutDesc;
}

} // namespace VL
