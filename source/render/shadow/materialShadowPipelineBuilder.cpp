#include "render/shadow/materialShadowPipelineBuilder.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "commonFunction.h"
#include "material.h"
#include "materialInstanceValidator.h"
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "renderGraph.h"

namespace VL
{
namespace
{

enum class ShadowShaderPairState
{
    Missing,
    Incomplete,
    Complete
};

// 檢查材質是否提供完整 `.shadow.vert/.shadow.frag` override。兩個檔案都缺失
// 表示沒有 override；只存在一個 stage 時警告，之後仍繼續自動/公共路由。
ShadowShaderPairState ResolveShadowShaderPair(const std::string& surfaceShaderName)
{
    const std::filesystem::path shaderRoot =
        std::filesystem::path(CommonFunction::GetProjectPath()) / "shader" / "glsl";
    const std::filesystem::path shadowBase = shaderRoot / (surfaceShaderName + ".shadow");
    const bool hasVertex = std::filesystem::is_regular_file(shadowBase.string() + ".vert");
    const bool hasFragment = std::filesystem::is_regular_file(shadowBase.string() + ".frag");

    if (hasVertex != hasFragment)
    {
        return ShadowShaderPairState::Incomplete;
    }
    return hasVertex ? ShadowShaderPairState::Complete : ShadowShaderPairState::Missing;
}

// 按 set/binding 定位一条反射绑定，用于比较 Surface 与 Shadow shader
// 对同一 descriptor 槽位的声明。
const ShaderBinding* FindBinding(
    const std::vector<ShaderBinding>& bindings,
    uint32_t setIndex,
    uint32_t bindingIndex)
{
    for (const ShaderBinding& binding : bindings)
    {
        if (binding.set == setIndex && binding.binding == bindingIndex)
        {
            return &binding;
        }
    }
    return nullptr;
}

// Set 0/2 仍使用 Surface reflection 合同；Set 1 由 M_ schema 單獨校驗。
// Shadow shader 禁止聲明 pass-owned Set 3，避免材質自行侵入 RenderGraph 資源。
void ValidateInheritedEngineBindings(
    const std::string& materialName,
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>& shadowBindings)
{
    for (const ShaderBinding& shadowBinding : shadowBindings)
    {
        if (shadowBinding.set >= PassSetIndex)
        {
            throw std::runtime_error(
                "Material ShadowCaster cannot use Set 3 or higher: " + materialName);
        }

        if (shadowBinding.set == MaterialSetIndex)
        {
            continue;
        }

        const ShaderBinding* surfaceBinding = FindBinding(
            surfaceBindings,
            shadowBinding.set,
            shadowBinding.binding);
        const bool stageIsSubset = surfaceBinding != nullptr &&
            (shadowBinding.stageFlags & ~surfaceBinding->stageFlags) == vk::ShaderStageFlags{};
        const bool bindingMatches = surfaceBinding != nullptr &&
            HasSameShaderBindingLayout(*surfaceBinding, shadowBinding);

        if (!bindingMatches || !stageIsSubset)
        {
            throw std::runtime_error(
                "Material ShadowCaster binding is incompatible with the Surface descriptor contract: " +
                materialName + " Set " + std::to_string(shadowBinding.set) +
                " Binding " + std::to_string(shadowBinding.binding));
        }
    }
}

// Shadow pipeline 的 Set 0/2 繼承 Base engine contract，Set 1 始終使用
// 完整材質 schema，從而可直接綁定 MI 已建立的 descriptor set。
GraphicsPipelineLayoutDesc BuildInheritedMaterialLayout(
    const Material& material,
    const std::vector<ShaderBinding>& surfaceBindings)
{
    GraphicsPipelineLayoutDesc layoutDesc;
    for (uint32_t setIndex = GlobalSetIndex; setIndex <= ObjectSetIndex; ++setIndex)
    {
        layoutDesc.overrideSets[setIndex] = true;
    }
    for (const ShaderBinding& binding : surfaceBindings)
    {
        if (binding.set == GlobalSetIndex || binding.set == ObjectSetIndex)
        {
            layoutDesc.setBindings[binding.set].push_back(binding);
        }
    }
    layoutDesc.setBindings[MaterialSetIndex] =
        material.GetMaterialDescriptorSchema().GetSetBindings();
    return layoutDesc;
}

bool RequiresMaterialShadowPass(const VL::MaterialFeatureKey& features)
{
    return features.usesOpacityMask ||
        features.modifiesMeshPosition ||
        features.twoSided;
}

} // namespace

MaterialShadowPipelineBuildResult BuildMaterialShadowPipeline(
    PipelineFactory& pipelineFactory,
    Renderpass* canonicalShadowPass,
    const MaterialInstanceBuildPlan& loadPlan,
    const Material& material,
    PipelineFactory::GraphicsCandidateState* candidateState)
{
    // 顯式 override 只在完整配對時啟用。不完整配對只忽略 override，後續仍可
    // 走自動生成；真正的編譯或合同錯誤則必須中止，不能靜默回退。
    const ShadowShaderPairState pairState =
        ResolveShadowShaderPair(loadPlan.shaderVariantKey.shaderName);
    if (pairState == ShadowShaderPairState::Incomplete)
    {
        std::cout << "Warning: Material variant '" << loadPlan.materialKey
                  << "' has an incomplete .shadow shader pair; its dedicated ShadowCaster is disabled."
                  << std::endl;
    }

    const bool hasExplicitOverride = pairState == ShadowShaderPairState::Complete;
    const RenderMode renderMode = loadPlan.shaderVariantKey.renderMode;
    // ThinTranslucent 与其它透明模式一样不自动生成普通 Shadow Map；只有显式 shadow
    // override 才能表达作者确实需要的投影行为。
    const bool isTransparent = IsTransparentRenderMode(renderMode);
    // 透明材質只有顯式 override 才進普通 Shadow Map；WPO 本身不等於投影意圖。
    const bool requiresGeneratedPass = !isTransparent &&
        RequiresMaterialShadowPass(loadPlan.materialFeatureKey);
    if (!hasExplicitOverride && !requiresGeneratedPass)
    {
        return {};
    }
    if (!hasExplicitOverride && !loadPlan.baseShaderCompileRequest)
    {
        throw std::runtime_error(
            "Material requires ShadowDepth composition but its M_ asset has no shaderEvaluation: " +
            loadPlan.materialKey);
    }
    if (canonicalShadowPass == nullptr)
    {
        throw std::runtime_error("Material ShadowCaster requires a shadow RenderGraph pass");
    }

    const GraphicsShaderVariantArtifact* shadowShaderArtifact = nullptr;
    if (hasExplicitOverride)
    {
        // override 是完整 stage shader，只替換 shader pair，靜態 MI variant 保持一致。
        ShaderVariantKey shadowVariant = loadPlan.shaderVariantKey;
        shadowVariant.shaderName += ".shadow";
        shadowShaderArtifact = candidateState != nullptr
            ? &pipelineFactory.PrepareGraphicsShaderVariantCandidate(
                *candidateState,
                shadowVariant)
            : &pipelineFactory.PrepareGraphicsShaderVariant(
                shadowVariant);
    }
    else
    {
        VL::MaterialShaderCompileRequest shadowRequest =
            *loadPlan.baseShaderCompileRequest;
        shadowRequest.pass = VL::MaterialPass::ShadowDepth;
        shadowShaderArtifact = candidateState != nullptr
            ? &pipelineFactory.PrepareMaterialShaderVariantCandidate(
                *candidateState,
                shadowRequest)
            : &pipelineFactory.PrepareMaterialShaderVariant(
                shadowRequest);
    }

    const std::vector<ShaderBinding>& surfaceBindings =
        material.GetRenderPipeline()->GetShaderBindings();
    ValidateMaterialShadowShaderBindings(
        loadPlan.materialKey,
        material.GetMaterialDescriptorSchema(),
        surfaceBindings,
        shadowShaderArtifact->shaderBindings);

    // 使用 canonical Shadow pass 的深度合同建立管線，並繼承 Base Set 0/2
    // 與完整材質 Set 1，使繪製時直接綁定 MI 已有 descriptor set。
    MaterialShadowPipelineBuildResult result;
    const GraphicsPipelineLayoutDesc shadowLayout =
        BuildInheritedMaterialLayout(material, surfaceBindings);
    result.pipeline = candidateState != nullptr
        ? pipelineFactory.CreateGraphicsPipelineCandidate(
            *candidateState,
            &canonicalShadowPass->renderPass,
            canonicalShadowPass->pipelineContractKey,
            *shadowShaderArtifact,
            loadPlan.cullMode,
            GraphicsPipelineBlendMode::Opaque,
            shadowLayout)
        : pipelineFactory.CreateGraphicsPipeline(
            &canonicalShadowPass->renderPass,
            canonicalShadowPass->pipelineContractKey,
            *shadowShaderArtifact,
            loadPlan.cullMode,
            GraphicsPipelineBlendMode::Opaque,
            shadowLayout);
    result.shaderArtifact = *shadowShaderArtifact;
    result.reloadRecipe.passName = canonicalShadowPass->name;
    result.reloadRecipe.passPipelineContractKey =
        canonicalShadowPass->pipelineContractKey;
    result.reloadRecipe.shader.shaderVariantKey =
        loadPlan.shaderVariantKey;
    if (hasExplicitOverride)
    {
        result.reloadRecipe.shader.shaderVariantKey.shaderName += ".shadow";
    }
    result.reloadRecipe.shader.kind = hasExplicitOverride
        ? GraphicsShaderReloadRecipeKind::StandaloneVariant
        : GraphicsShaderReloadRecipeKind::MaterialComposed;
    if (!hasExplicitOverride)
    {
        MaterialShaderCompileRequest shadowRequest =
            *loadPlan.baseShaderCompileRequest;
        shadowRequest.pass = MaterialPass::ShadowDepth;
        result.reloadRecipe.shader.materialCompileRequest =
            std::move(shadowRequest);
    }
    result.reloadRecipe.cullMode = loadPlan.cullMode;
    result.reloadRecipe.blendMode = GraphicsPipelineBlendMode::Opaque;
    result.reloadRecipe.layoutKind =
        MaterialPipelineLayoutRecipeKind::ShadowInheritedSurface;
    return result;
}

void ValidateMaterialShadowShaderBindings(
    const std::string& materialName,
    const MaterialDescriptorSchema& materialDescriptorSchema,
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>& shadowBindings)
{
    materialDescriptorSchema.ValidateShaderBindings(
        shadowBindings,
        materialName);
    ValidateInheritedEngineBindings(
        materialName,
        surfaceBindings,
        shadowBindings);
}

GraphicsPipelineLayoutDesc BuildMaterialShadowPipelineLayout(
    const MaterialDescriptorSchema& materialDescriptorSchema,
    const std::vector<ShaderBinding>& surfaceBindings)
{
    GraphicsPipelineLayoutDesc layoutDesc;
    for (uint32_t setIndex = GlobalSetIndex; setIndex <= ObjectSetIndex; ++setIndex)
    {
        layoutDesc.overrideSets[setIndex] = true;
    }
    for (const ShaderBinding& binding : surfaceBindings)
    {
        if (binding.set == GlobalSetIndex || binding.set == ObjectSetIndex)
        {
            layoutDesc.setBindings[binding.set].push_back(binding);
        }
    }
    layoutDesc.setBindings[MaterialSetIndex] =
        materialDescriptorSchema.GetSetBindings();
    return layoutDesc;
}

} // namespace VL
