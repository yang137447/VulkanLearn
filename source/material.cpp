#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineFactory.h"
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/graphicsShaderVariantArtifact.h"
#include "pipeline/pipelineBase.h"
#include "renderGraph.h"
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace
{

bool CompareShaderBindings(const ShaderBinding& lhs, const ShaderBinding& rhs)
{
    return lhs.set != rhs.set ? lhs.set < rhs.set : lhs.binding < rhs.binding;
}

void MergeActiveShaderBinding(
    std::vector<ShaderBinding>& activeBindings,
    const ShaderBinding& newBinding)
{
    for (ShaderBinding& activeBinding : activeBindings)
    {
        if (activeBinding.set != newBinding.set || activeBinding.binding != newBinding.binding)
        {
            continue;
        }

        if (!HasSameShaderBindingLayout(activeBinding, newBinding))
        {
            throw std::runtime_error(
                "Material passes use incompatible descriptor bindings at Set " +
                std::to_string(newBinding.set) + " Binding " +
                std::to_string(newBinding.binding));
        }
        activeBinding.stageFlags |= newBinding.stageFlags;
        return;
    }
    activeBindings.push_back(newBinding);
}

} // namespace

Material::Material(
    PipelineFactory& pipelineFactory,
    Renderpass& renderPass,
    const ShaderVariantKey& shaderVariantKey,
    const VL::MaterialFeatureKey& materialFeatureKey,
    const VL::MaterialDescriptorSchema& materialDescriptorSchema,
    const std::optional<VL::MaterialShaderCompileRequest>& baseShaderCompileRequest,
    const std::string& materialKey,
    vk::CullModeFlags cullMode,
    GraphicsPipelineBlendMode blendMode,
    PipelineFactory::GraphicsCandidateState* candidateState)
{
    this->shaderVariantKey = shaderVariantKey;
    this->materialFeatureKey = materialFeatureKey;
    this->materialDescriptorSchema = materialDescriptorSchema;
    this->materialKey = materialKey;
    this->passPipelineContractKey = renderPass.pipelineContractKey;

    // M_ 声明 shaderEvaluation 时走材质装配路径；否则使用 shaderName 指向的完整 Shader 变体。
    // 两者是由资产格式明确选择的合法路径，不是材质装配失败后的回退。
    // An M_ asset with shaderEvaluation uses material composition; otherwise shaderName selects a complete
    // shader variant. Both are explicit asset paths, not a fallback after composition failure.
    const GraphicsShaderVariantArtifact& shaderArtifact =
        candidateState != nullptr
        ? (baseShaderCompileRequest
            ? pipelineFactory.PrepareMaterialShaderVariantCandidate(
                *candidateState,
                *baseShaderCompileRequest)
            : pipelineFactory.PrepareGraphicsShaderVariantCandidate(
                *candidateState,
                shaderVariantKey))
        : (baseShaderCompileRequest
            ? pipelineFactory.PrepareMaterialShaderVariant(
                *baseShaderCompileRequest)
            : pipelineFactory.PrepareGraphicsShaderVariant(
                shaderVariantKey));
    this->materialDescriptorSchema.ValidateShaderBindings(
        shaderArtifact.shaderBindings,
        shaderArtifact.displayName);

    // Set 1 layout始終來自 M_ schema；單個 pass 的 reflection 只描述它實際使用的子集。
    GraphicsPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.overrideSets[MaterialSetIndex] = true;
    pipelineLayoutDesc.setBindings[MaterialSetIndex] =
        this->materialDescriptorSchema.GetSetBindings();

    renderPipeline = candidateState != nullptr
        ? pipelineFactory.CreateGraphicsPipelineCandidate(
            *candidateState,
            &renderPass.renderPass,
            renderPass.pipelineContractKey,
            shaderArtifact,
            cullMode,
            blendMode,
            pipelineLayoutDesc)
        : pipelineFactory.CreateGraphicsPipeline(
            &renderPass.renderPass,
            renderPass.pipelineContractKey,
            shaderArtifact,
            cullMode,
            blendMode,
            pipelineLayoutDesc);
    activeShaderBindings = shaderArtifact.shaderBindings;
    surfaceShaderArtifact = shaderArtifact;

    pipelineReloadRecipe.surface.passName = renderPass.name;
    pipelineReloadRecipe.surface.passPipelineContractKey =
        renderPass.pipelineContractKey;
    pipelineReloadRecipe.surface.shader.shaderVariantKey = shaderVariantKey;
    pipelineReloadRecipe.surface.shader.materialCompileRequest =
        baseShaderCompileRequest;
    pipelineReloadRecipe.surface.shader.kind = baseShaderCompileRequest
        ? VL::GraphicsShaderReloadRecipeKind::MaterialComposed
        : VL::GraphicsShaderReloadRecipeKind::StandaloneVariant;
    pipelineReloadRecipe.surface.cullMode = cullMode;
    pipelineReloadRecipe.surface.blendMode = blendMode;
    pipelineReloadRecipe.surface.layoutKind =
        VL::MaterialPipelineLayoutRecipeKind::SurfaceMaterialSchema;
}

void Material::SetShadowPipeline(
    std::shared_ptr<PipelineBase> pipeline,
    const GraphicsShaderVariantArtifact& shaderArtifact,
    const VL::MaterialGraphicsPassReloadRecipe& reloadRecipe)
{
    shadowPipeline = std::move(pipeline);
    if (!shadowPipeline)
    {
        return;
    }

    // MI 只建立一套 descriptor 資源，因此實際寫入集合必須覆蓋 Base 與
    // 選中的 ShadowDepth shader 使用資源，但不強迫寫入 schema 中未使用的貼圖。
    activeShaderBindings = BuildActiveShaderBindings(
        surfaceShaderArtifact.shaderBindings,
        &shaderArtifact.shaderBindings);
    shadowShaderArtifact = shaderArtifact;
    pipelineReloadRecipe.shadow = reloadRecipe;
}

std::vector<ShaderBinding> Material::BuildActiveShaderBindings(
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>* shadowBindings)
{
    std::vector<ShaderBinding> activeBindings = surfaceBindings;
    if (shadowBindings != nullptr)
    {
        for (const ShaderBinding& binding : *shadowBindings)
        {
            MergeActiveShaderBinding(activeBindings, binding);
        }
    }
    std::sort(
        activeBindings.begin(),
        activeBindings.end(),
        CompareShaderBindings);
    return activeBindings;
}

void Material::ValidatePipelineReloadCommit(
    const VL::MaterialPipelineReloadCommit& commit) const
{
    if (commit.replaceSurface && !commit.surfacePipeline)
    {
        throw std::runtime_error(
            "Material pipeline reload commit is missing its Surface pipeline");
    }
    if (pipelineReloadRecipe.shadow.has_value() !=
        commit.shadowArtifact.has_value())
    {
        throw std::runtime_error(
            "Material pipeline reload commit does not match its Shadow recipe");
    }
    if (commit.replaceShadow && !commit.shadowPipeline)
    {
        throw std::runtime_error(
            "Material pipeline reload commit is missing its Shadow pipeline");
    }
}

VL::RetiredMaterialPipelines Material::CommitPipelineReload(
    VL::MaterialPipelineReloadCommit commit) noexcept
{
    VL::RetiredMaterialPipelines retired;
    if (commit.replaceSurface)
    {
        retired.surfacePipeline = std::move(renderPipeline);
        renderPipeline = std::move(commit.surfacePipeline);
    }
    if (commit.replaceShadow)
    {
        retired.shadowPipeline = std::move(shadowPipeline);
        shadowPipeline = std::move(commit.shadowPipeline);
    }
    surfaceShaderArtifact = std::move(commit.surfaceArtifact);
    shadowShaderArtifact = std::move(commit.shadowArtifact);
    activeShaderBindings = std::move(commit.activeShaderBindings);
    return retired;
}
std::shared_ptr<MaterialInstance> Material::CreateInstance()
{
    auto instance = std::make_shared<MaterialInstance>();
    instance->SetBaseMaterial(shared_from_this());
    
    return instance;
}
