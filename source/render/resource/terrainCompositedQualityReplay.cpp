#include "render/resource/terrainCompositedQualityReplay.h"

#include "pipeline/pipelineFactory.h"
#include <algorithm>
#include <stdexcept>

namespace VL
{
ShaderVariantKey TerrainQualityCaptureVariant(ShaderVariantKey regularVariant)
{
    if (regularVariant.shaderName != "pass/terrainFeedback")
        throw std::runtime_error("Quality replay requires the actual terrainFeedback shader pair");
    auto& macros = regularVariant.macros;
    macros.erase(std::remove_if(macros.begin(), macros.end(), [](const std::string& macro)
    {
        return HasMaterialMacroName({macro}, "VL_TERRAIN_QUALITY_CAPTURE");
    }), macros.end());
    macros.push_back("VL_TERRAIN_QUALITY_CAPTURE=1");
    macros = NormalizeMaterialMacros(std::move(macros));
    return regularVariant;
}

GraphicsPipelineLayoutDesc TerrainQualityReplayLayout(
    const std::vector<ShaderBinding>& regularLayout,
    const GraphicsShaderVariantArtifact& captureArtifact)
{
    GraphicsPipelineLayoutDesc layout;
    for (uint32_t set = 0; set < MAX_DESCRIPTOR_SETS; ++set)
        layout.overrideSets[set] = true;
    for (const auto& binding : regularLayout)
    {
        if (binding.set >= MAX_DESCRIPTOR_SETS || (binding.set == 3 && binding.binding >= 5))
            throw std::runtime_error("Quality replay received an incompatible regular feedback layout");
        layout.setBindings[binding.set].push_back(binding);
    }
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        const auto& pass = layout.setBindings[3];
        const auto found = std::find_if(pass.begin(), pass.end(),
            [slot](const ShaderBinding& binding) { return binding.binding == slot; });
        if (found == pass.end() || found->type != vk::DescriptorType::eCombinedImageSampler ||
            found->descriptorCount != 1)
            throw std::runtime_error("Quality replay must retain all five graph-owned feedback inputs");
    }
    for (const auto& binding : captureArtifact.shaderBindings)
    {
        if (binding.set >= MAX_DESCRIPTOR_SETS)
            throw std::runtime_error("Quality replay reflected set is outside the engine contract");
        if (binding.set == 3 && (binding.binding == 5 || binding.binding == 6))
        {
            const auto type = binding.binding == 5 ? vk::DescriptorType::eStorageBuffer :
                vk::DescriptorType::eCombinedImageSampler;
            if (binding.type != type || binding.descriptorCount != 1 ||
                binding.stageFlags != vk::ShaderStageFlagBits::eFragment)
                throw std::runtime_error("Quality replay reflected capture binding differs from TVQ1");
            layout.setBindings[3].push_back(binding);
            continue;
        }
        const auto& regular = layout.setBindings[binding.set];
        const auto found = std::find_if(regular.begin(), regular.end(),
            [&](const ShaderBinding& existing) { return existing.binding == binding.binding; });
        if (found == regular.end() || !HasSameShaderBindingLayout(*found, binding) ||
            (binding.stageFlags & ~found->stageFlags) != vk::ShaderStageFlags{})
            throw std::runtime_error("Quality replay cannot reuse the regular draw descriptor sets");
    }
    if (layout.setBindings[3].size() != 7)
        throw std::runtime_error("Quality replay requires actual reflected storage5 and depth6");
    for (auto& bindings : layout.setBindings)
        std::sort(bindings.begin(), bindings.end(),
            [](const ShaderBinding& left, const ShaderBinding& right)
            { return left.binding < right.binding; });
    return layout;
}

std::shared_ptr<PipelineBase> CreateTerrainQualityReplayPipeline(
    PipelineFactory& factory, vk::RenderPass& renderPass,
    PassPipelineContractKey replayContract, const ShaderVariantKey& regularVariant,
    const std::vector<ShaderBinding>& regularLayout)
{
    PipelineFactory::GraphicsCandidateState candidate;
    const auto& artifact = factory.PrepareGraphicsShaderVariantCandidate(
        candidate, TerrainQualityCaptureVariant(regularVariant));
    const auto layout = TerrainQualityReplayLayout(regularLayout, artifact);
    replayContract.depthTestEnable = false;
    replayContract.depthWriteEnable = false;
    replayContract.stencilTestEnable = false;
    replayContract.useVertexInput = true;
    return factory.CreateGraphicsPipelineCandidate(candidate, &renderPass, replayContract, artifact,
        vk::CullModeFlagBits::eBack, GraphicsPipelineBlendMode::Opaque, layout);
}
}
