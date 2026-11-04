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

// LocalShadow 的 Set 0/2 只校验 Vulkan 管线兼容性（set/binding/type/count/
// stage），不比较 UBO 反射成员表。原因：片段新增 localShadowLightPos/Params
// 的使用后，SPIR-V 反射会保留这两个成员；Surface shader 未使用它们时反射
// 成员表更短，但两者绑定的是同一份引擎 UBO，管线布局完全兼容。
void ValidateLocalShadowEngineBindings(
    const std::string& materialName,
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>& localShadowBindings)
{
    for (const ShaderBinding& localShadowBinding : localShadowBindings)
    {
        if (localShadowBinding.set >= PassSetIndex)
        {
            throw std::runtime_error(
                "Material LocalShadow cannot use Set 3 or higher: " +
                materialName);
        }
        if (localShadowBinding.set == MaterialSetIndex)
        {
            continue;
        }

        const ShaderBinding* surfaceBinding = FindBinding(
            surfaceBindings,
            localShadowBinding.set,
            localShadowBinding.binding);
        const bool pipelineLayoutMatches = surfaceBinding != nullptr &&
            surfaceBinding->type == localShadowBinding.type &&
            surfaceBinding->descriptorCount ==
                localShadowBinding.descriptorCount &&
            surfaceBinding->name == localShadowBinding.name;
        if (!pipelineLayoutMatches)
        {
            throw std::runtime_error(
                "Material LocalShadow binding is incompatible with the "
                "Surface descriptor contract: " + materialName +
                " Set " + std::to_string(localShadowBinding.set) +
                " Binding " + std::to_string(localShadowBinding.binding));
        }
    }
}

// LocalShadow fragment 需要读 UBOGlobal 的 localShadow 字段，而 Surface
// fragment 可能完全没用到 Set 0；这里把 LocalShadow 反射到的 Set 0/2 绑定
// 并入 layout，并合并 stageFlags。描述符集本身仍由 Surface layout 分配，
// 两者 binding/type/count/name 相同，因此可以直接复用同一批 MI descriptor。
void MergeLocalShadowEngineBindings(
    GraphicsPipelineLayoutDesc& layout,
    const std::vector<ShaderBinding>& localShadowBindings,
    const std::string& materialName)
{
    for (const ShaderBinding& localShadowBinding : localShadowBindings)
    {
        if (localShadowBinding.set == MaterialSetIndex)
        {
            continue;
        }
        std::vector<ShaderBinding>& engineBindings =
            layout.setBindings[localShadowBinding.set];
        bool merged = false;
        for (ShaderBinding& binding : engineBindings)
        {
            if (binding.binding != localShadowBinding.binding)
            {
                continue;
            }
            if (binding.type != localShadowBinding.type ||
                binding.descriptorCount !=
                    localShadowBinding.descriptorCount ||
                binding.name != localShadowBinding.name)
            {
                throw std::runtime_error(
                    "Material LocalShadow binding layout conflict: " +
                    materialName + " Set " +
                    std::to_string(localShadowBinding.set) +
                    " Binding " +
                    std::to_string(localShadowBinding.binding));
            }
            binding.stageFlags |= localShadowBinding.stageFlags;
            merged = true;
            break;
        }
        if (!merged)
        {
            engineBindings.push_back(localShadowBinding);
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
            ShaderBinding engineBinding = binding;
            engineBinding.stageFlags |=
                vk::ShaderStageFlagBits::eVertex |
                vk::ShaderStageFlagBits::eFragment;
            layoutDesc.setBindings[binding.set].push_back(
                std::move(engineBinding));
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
    if (hasExplicitOverride && loadPlan.baseShaderCompileRequest &&
        loadPlan.baseShaderCompileRequest->vertexFactoryKey == "SkinnedMesh")
    {
        // 旧式完整 shader 未声明蒙皮输入语义，不能只凭 descriptor 兼容就接受静态阴影。
        throw std::runtime_error("SkinnedMesh requires composed ShadowDepth without a standalone override: " +
            loadPlan.materialKey);
    }
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

MaterialShadowPipelineBuildResult BuildMaterialLocalShadowPipeline(
    PipelineFactory& pipelineFactory,
    Renderpass* canonicalLocalShadowPass,
    const MaterialInstanceBuildPlan& loadPlan,
    const Material& material,
    PipelineFactory::GraphicsCandidateState* candidateState)
{
    if (canonicalLocalShadowPass == nullptr)
    {
        return {};
    }

    // 只有两类材质需要专属 LocalShadow 管线：
    // 1) 普通世界材质已经有 ShadowCaster 管线（alpha/WPO/two-sided）；
    // 2) LocalShadow pass 自己的公共材质（M_shadow → shaderName "shadow"）。
    const bool isShadowPassMaterial =
        loadPlan.shaderVariantKey.shaderName == "shadow";
    if (!isShadowPassMaterial && material.GetShadowPipeline() == nullptr)
    {
        return {};
    }

    const GraphicsShaderVariantArtifact* localShadowShaderArtifact = nullptr;
    std::optional<MaterialShaderCompileRequest> composedRequest;
    if (loadPlan.baseShaderCompileRequest)
    {
        composedRequest = *loadPlan.baseShaderCompileRequest;
        composedRequest->pass = MaterialPass::LocalShadow;
        localShadowShaderArtifact = candidateState != nullptr
            ? &pipelineFactory.PrepareMaterialShaderVariantCandidate(
                *candidateState,
                *composedRequest)
            : &pipelineFactory.PrepareMaterialShaderVariant(
                *composedRequest);
    }
    else if (isShadowPassMaterial)
    {
        // 公共 pass 材质没有 shaderEvaluation，直接编译完整 shadowLocal 变体。
        // 它只声明 Set 0/2，不依赖材质参数。
        ShaderVariantKey localShadowVariant = loadPlan.shaderVariantKey;
        localShadowVariant.shaderName = "shadowLocal";
        localShadowShaderArtifact = candidateState != nullptr
            ? &pipelineFactory.PrepareGraphicsShaderVariantCandidate(
                *candidateState,
                localShadowVariant)
            : &pipelineFactory.PrepareGraphicsShaderVariant(
                localShadowVariant);
    }
    else
    {
        // 显式 `.shadow` override 是作者自定义投影 shader，不能假设它也写
        // LocalShadow 深度；退回公共管线并由 draw executor 保持几何投影。
        return {};
    }

    const std::vector<ShaderBinding>& surfaceBindings =
        material.GetRenderPipeline()->GetShaderBindings();
    material.GetMaterialDescriptorSchema().ValidateShaderBindings(
        localShadowShaderArtifact->shaderBindings,
        loadPlan.materialKey);
    ValidateLocalShadowEngineBindings(
        loadPlan.materialKey,
        surfaceBindings,
        localShadowShaderArtifact->shaderBindings);

    GraphicsPipelineLayoutDesc localShadowLayout =
        BuildInheritedMaterialLayout(material, surfaceBindings);
    MergeLocalShadowEngineBindings(
        localShadowLayout,
        localShadowShaderArtifact->shaderBindings,
        loadPlan.materialKey);
    MaterialShadowPipelineBuildResult result;
    result.pipeline = candidateState != nullptr
        ? pipelineFactory.CreateGraphicsPipelineCandidate(
            *candidateState,
            &canonicalLocalShadowPass->renderPass,
            canonicalLocalShadowPass->pipelineContractKey,
            *localShadowShaderArtifact,
            loadPlan.cullMode,
            GraphicsPipelineBlendMode::Opaque,
            localShadowLayout)
        : pipelineFactory.CreateGraphicsPipeline(
            &canonicalLocalShadowPass->renderPass,
            canonicalLocalShadowPass->pipelineContractKey,
            *localShadowShaderArtifact,
            loadPlan.cullMode,
            GraphicsPipelineBlendMode::Opaque,
            localShadowLayout);
    result.shaderArtifact = *localShadowShaderArtifact;
    result.reloadRecipe.passName = canonicalLocalShadowPass->name;
    result.reloadRecipe.passPipelineContractKey =
        canonicalLocalShadowPass->pipelineContractKey;
    result.reloadRecipe.shader.shaderVariantKey =
        loadPlan.shaderVariantKey;
    result.reloadRecipe.shader.kind =
        composedRequest.has_value()
        ? GraphicsShaderReloadRecipeKind::MaterialComposed
        : GraphicsShaderReloadRecipeKind::StandaloneVariant;
    if (composedRequest.has_value())
    {
        result.reloadRecipe.shader.materialCompileRequest =
            std::move(composedRequest);
    }
    else
    {
        result.reloadRecipe.shader.shaderVariantKey.shaderName =
            "shadowLocal";
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
            ShaderBinding engineBinding = binding;
            engineBinding.stageFlags |=
                vk::ShaderStageFlagBits::eVertex |
                vk::ShaderStageFlagBits::eFragment;
            layoutDesc.setBindings[binding.set].push_back(
                std::move(engineBinding));
        }
    }
    layoutDesc.setBindings[MaterialSetIndex] =
        materialDescriptorSchema.GetSetBindings();
    return layoutDesc;
}

} // namespace VL
