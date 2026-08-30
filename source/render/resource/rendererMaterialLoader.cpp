#include "render/resource/rendererMaterialLoader.h"

#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "material.h"
#include "material/materialAssetUtils.h"
#include "material/generator/materialDefinitionReloadBatch.h"
#include "material/loader/materialInstanceResolver.h"
#include "materialInstance.h"
#include "materialInstanceValidator.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/eye/eyeMaterialContract.h"
#include "render/eye/eyeResourceSet.h"
#include "render/hair/hairMaterialContract.h"
#include "render/subsurface/subsurfaceMaterialContract.h"
#include "render/subsurface/subsurfaceResourceSet.h"
#include "render/shadow/materialShadowPipelineBuilder.h"
#include "renderGraph.h"
#include "texture.h"
#include "textureAssetLoader.h"

namespace VL
{
namespace
{

nlohmann::json LoadMaterialInstanceJson(
    const RendererResourceLoadContext& loadContext,
    std::string_view materialInstancePath)
{
    const std::string normalizedPath =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    if (loadContext.materialInstanceOverrideJson.has_value() &&
        loadContext.materialInstanceOverridePath == normalizedPath)
    {
        return *loadContext.materialInstanceOverrideJson;
    }

    std::ifstream materialInstanceFile(
        CommonFunction::Path(std::string(materialInstancePath)));
    if (!materialInstanceFile.is_open())
    {
        throw std::runtime_error(
            "Failed to open material instance: " +
            std::string(materialInstancePath));
    }

    nlohmann::json materialInstanceJson;
    materialInstanceFile >> materialInstanceJson;
    return materialInstanceJson;
}

const MaterialParameterSchemaEntry* FindParameterSchema(
    const MaterialDescriptorSchema& schema,
    const std::string& name)
{
    for (const MaterialParameterSchemaEntry& entry :
         schema.GetParameters())
    {
        if (entry.name == name)
        {
            return &entry;
        }
    }
    return nullptr;
}

const MaterialTextureSchemaEntry* FindTextureSchema(
    const MaterialDescriptorSchema& schema,
    const std::string& name)
{
    for (const MaterialTextureSchemaEntry& entry :
         schema.GetTextures())
    {
        if (entry.name == name)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool ParameterValueMatchesSchema(
    const MaterialInstanceParameterValue& value,
    const MaterialParameterSchemaEntry& schema)
{
    if (schema.glslType == "float")
    {
        return std::holds_alternative<float>(value);
    }
    if (schema.glslType == "vec2")
    {
        return std::holds_alternative<Eigen::Vector2f>(value);
    }
    if (schema.glslType == "vec3")
    {
        return std::holds_alternative<Eigen::Vector3f>(value);
    }
    return schema.glslType == "vec4" &&
        std::holds_alternative<Eigen::Vector4f>(value);
}

void SetParameterValue(
    MaterialInstance& materialInstance,
    const std::string& name,
    const MaterialInstanceParameterValue& value)
{
    std::visit(
        [&materialInstance, &name](const auto& typedValue)
        {
            materialInstance.SetParameter(name, typedValue);
        },
        value);
}

nlohmann::json ParameterValueToJson(
    const MaterialInstanceParameterValue& value)
{
    return std::visit(
        [](const auto& typedValue) -> nlohmann::json
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, float>)
            {
                return typedValue;
            }
            else
            {
                nlohmann::json array = nlohmann::json::array();
                for (Eigen::Index index = 0;
                     index < typedValue.size();
                     ++index)
                {
                    array.push_back(typedValue[index]);
                }
                return array;
            }
        },
        value);
}

nlohmann::json BuildCandidateValidationJson(
    const nlohmann::json& effectiveMaterialInstanceJson,
    const MaterialInstanceStateSnapshot& snapshot)
{
    nlohmann::json validationJson =
        effectiveMaterialInstanceJson;
    validationJson["parameters"] =
        nlohmann::json::object();
    for (const auto& [name, value] :
         snapshot.parameters)
    {
        validationJson["parameters"][name] =
            ParameterValueToJson(value);
    }

    validationJson["textures"] =
        nlohmann::json::object();
    for (const auto& [name, binding] :
         snapshot.textures)
    {
        if (binding.texture)
        {
            validationJson["textures"][name] =
                binding.textureAssetIdentity.value_or(
                    std::string("runtime://") + name);
        }
    }
    return validationJson;
}

void BindEngineSubsurfaceTextures(
    MaterialInstance& materialInstance,
    const Material& material,
    const SubsurfaceResourceSet& resources)
{
    // lookup texture 属于 World-local resource set，由引擎注入；MI 只作者化资产路径，
    // 不能自行绑定另一 generation 的 profile/LUT texture。
    if (FindTextureSchema(
            material.GetMaterialDescriptorSchema(),
            "subsurfaceProfileTable") != nullptr)
    {
        materialInstance.SetTexture(
            "subsurfaceProfileTable",
            resources.profileTableTexture);
    }
    if (FindTextureSchema(
            material.GetMaterialDescriptorSchema(),
            "preintegratedSkinLutTable") != nullptr)
    {
        materialInstance.SetTexture(
            "preintegratedSkinLutTable",
            resources.skinLutTableTexture);
    }
}

void ApplyParameterJson(
    MaterialInstance& materialInstance,
    const nlohmann::json& shaderParameters,
    std::string_view materialInstancePath)
{
    for (const auto& [name, value] :
         shaderParameters.items())
    {
        const uint32_t paramSize =
            JsonParser::ParseValueSize(value);
        if (paramSize == sizeof(float))
        {
            materialInstance.SetParameter(
                name,
                JsonParser::ParseValue<float>(value));
        }
        else if (paramSize == sizeof(Eigen::Vector2f))
        {
            materialInstance.SetParameter(
                name,
                JsonParser::ParseValue<Eigen::Vector2f>(value));
        }
        else if (paramSize == sizeof(Eigen::Vector3f))
        {
            materialInstance.SetParameter(
                name,
                JsonParser::ParseValue<Eigen::Vector3f>(value));
        }
        else if (paramSize == sizeof(Eigen::Vector4f))
        {
            materialInstance.SetParameter(
                name,
                JsonParser::ParseValue<Eigen::Vector4f>(value));
        }
        else
        {
            throw std::runtime_error(
                "Unsupported parameter type or size in material instance: " +
                std::string(materialInstancePath));
        }
    }
}

} // namespace

RendererMaterialLoader::RendererMaterialLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
{
}

void RendererMaterialLoader::LoadPassMaterials() const
{
    RenderGraph& renderGraph = loadContext.renderGraph;

    for (const CompiledRenderGraphPass& passDesc :
         renderGraph.GetCompiledRenderGraph().passes)
    {
        if (!passDesc.needCreateMaterial)
        {
            continue;
        }
        Renderpass& renderPass = renderGraph.GetRenderpasses().at(passDesc.name);
        std::shared_ptr<MaterialInstance> materialInstance =
            LoadMaterialInstance(passDesc.materialInstancePath, renderPass);

        if (loadContext.passMaterialBindings != nullptr)
        {
            loadContext.passMaterialBindings->insert_or_assign(
                passDesc.name,
                std::move(materialInstance));
        }
        else
        {
            renderPass.materialInstance =
                std::move(materialInstance);
        }
    }
}

std::shared_ptr<MaterialInstance>
RendererMaterialLoader::LoadSceneMaterialInstance(
    std::string_view materialInstancePath) const
{
    const nlohmann::json materialInstanceJson =
        LoadMaterialInstanceJson(loadContext, materialInstancePath);
    const MaterialInstanceResolveResult resolveResult =
        MaterialInstanceResolver::Resolve(
            materialInstancePath,
            materialInstanceJson);
    const RenderMode renderMode =
        MaterialInstanceValidator::ResolveRenderMode(
            resolveResult.effectiveMaterialInstanceJson);

    // Surface 材质按强类型 Pass 行为选择管线合同，不能依赖 config 中可变的
    // pass name。透明材质也不能复用 Geometry 的 8 MRT 管线，否则 fragment
    // output 和 Set 3 阴影描述符都不兼容。
    const RenderGraphPassType surfacePassType =
        renderMode == RenderMode::ForwardOpaque
        ? RenderGraphPassType::ForwardOpaque
        : renderMode == RenderMode::ForwardEyeInner
            ? RenderGraphPassType::ForwardEyeInner
            : renderMode == RenderMode::ForwardEyeCornea
                ? RenderGraphPassType::ForwardEyeCornea
                : IsTransparentRenderMode(renderMode)
                    ? RenderGraphPassType::ForwardTransparent
                    : RenderGraphPassType::Geometry;
    Renderpass& renderPass =
        loadContext.renderGraph.RequireUniquePass(
            surfacePassType);
    return LoadMaterialInstance(materialInstancePath, renderPass);
}

std::shared_ptr<MaterialInstance> RendererMaterialLoader::LoadMaterialInstance(
    std::string_view materialInstancePath,
    Renderpass& renderPass) const
{
    RendererResourceCache& resourceCache =
        loadContext.resourceCache;
    // MI 以规范化资产路径作为唯一身份。同一份 MI 再次加载时必须返回同一对象，
    // 但它的 Material 管线合同仍需与目标 pass 一致，避免把旧图管线带入新图。
    const std::string materialInstanceKey =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    const nlohmann::json materialInstanceJson =
        LoadMaterialInstanceJson(loadContext, materialInstancePath);
    MaterialInstanceResolveResult materialInstanceResolveResult =
        MaterialInstanceResolver::Resolve(materialInstancePath, materialInstanceJson);
    nlohmann::json& effectiveMaterialInstanceJson =
        materialInstanceResolveResult.effectiveMaterialInstanceJson;
    const RenderMode effectiveRenderMode =
        MaterialInstanceValidator::ResolveRenderMode(effectiveMaterialInstanceJson);
    const PassPipelineContractKey effectiveSurfacePipelineContractKey =
        MaterialInstanceValidator::ResolveSurfacePipelineContractKey(
            renderPass.pipelineContractKey,
            effectiveRenderMode);
    const std::shared_ptr<MaterialInstance>* cachedMaterialInstance =
        resourceCache.GetMaterialInstance(materialInstanceKey);
    const bool isOverriddenMaterialInstance =
        loadContext.materialInstanceOverrideJson.has_value() &&
        loadContext.materialInstanceOverridePath == materialInstanceKey;
    if (!isOverriddenMaterialInstance &&
        cachedMaterialInstance != nullptr && *cachedMaterialInstance != nullptr)
    {
        std::shared_ptr<Material> cachedBaseMaterial =
            (*cachedMaterialInstance)->GetBaseMaterial().lock();
        if (!cachedBaseMaterial ||
            cachedBaseMaterial->GetPassPipelineContractKey() !=
                effectiveSurfacePipelineContractKey)
        {
            throw std::runtime_error(
                "Material instance asset '" + materialInstanceKey +
                "' was requested with an incompatible pass pipeline contract");
        }
        return *cachedMaterialInstance;
    }
    const std::shared_ptr<const SubsurfaceResourceSet>& subsurfaceResources =
        resourceCache.GetSubsurfaceResources();
    if (!subsurfaceResources)
    {
        throw std::runtime_error(
            "Renderer material loading requires a prepared subsurface resource set");
    }
    const ResolvedSubsurfaceMaterialAssets resolvedSubsurfaceAssets =
        ResolveSubsurfaceMaterialContract(
            materialInstanceJson,
            effectiveMaterialInstanceJson,
            *subsurfaceResources,
            materialInstancePath);
    const ResolvedEyeMaterialAssets resolvedEyeAssets =
        ResolveEyeMaterialContract(
            materialInstanceJson,
            effectiveMaterialInstanceJson,
            resourceCache.GetEyeResources().get(),
            *subsurfaceResources,
            materialInstancePath);
    ValidateHairMaterialContract(
        effectiveMaterialInstanceJson,
        resourceCache.GetHairResources().get(),
        materialInstancePath);
    // Resolve 先合并材质定义与 MI 覆写，BuildLoadPlan 再一次性生成 shader variant、
    // 固定管线状态和缓存 key；后续步骤只消费结果，不重复解释 JSON。
    MaterialInstanceBuildPlan loadPlan = MaterialInstanceValidator::BuildLoadPlan(
        materialInstancePath,
        renderPass.pipelineContractKey,
        effectiveMaterialInstanceJson,
        materialInstanceResolveResult.materialPath,
        materialInstanceResolveResult.materialJson,
        rendererBackend.SupportsDualSourceBlend());
    // 所有连续透明输出都只能进入 forwardTransparent；写深度模式只改变该材质
    // pipeline 的 DepthWrite 状态，不允许借用 Geometry 的多 MRT 合同。
    if (IsTransparentRenderMode(loadPlan.shaderVariantKey.renderMode) &&
        renderPass.type != RenderGraphPassType::ForwardTransparent)
    {
        throw std::runtime_error(
            "Transparent material must use the forwardTransparent pass: " +
            std::string(materialInstancePath));
    }
    if (loadPlan.shaderVariantKey.renderMode == RenderMode::ForwardOpaque &&
        renderPass.type != RenderGraphPassType::ForwardOpaque)
    {
        throw std::runtime_error(
            "ForwardOpaque material must use the forwardOpaque pass: " +
            std::string(materialInstancePath));
    }
    if (loadPlan.shaderVariantKey.renderMode == RenderMode::ForwardEyeInner &&
        renderPass.type != RenderGraphPassType::ForwardEyeInner)
    {
        throw std::runtime_error(
            "ForwardEyeInner material must use the forwardEyeInner pass: " +
            std::string(materialInstancePath));
    }
    if (loadPlan.shaderVariantKey.renderMode == RenderMode::ForwardEyeCornea &&
        renderPass.type != RenderGraphPassType::ForwardEyeCornea)
    {
        throw std::runtime_error(
            "ForwardEyeCornea material must use the forwardEyeCornea pass: " +
            std::string(materialInstancePath));
    }
    if (loadPlan.baseShaderCompileRequest &&
        loadContext.materialDefinitionReload != nullptr)
    {
        const std::string materialSourceIdentity =
            MaterialAssetUtils::NormalizeShaderGlslRelativePath(
                materialInstanceResolveResult.materialPath);
        const auto overlayIt =
            loadContext.materialDefinitionReload
                ->includeOverlays.find(
                    loadPlan.baseShaderCompileRequest
                        ->source.parameterIncludePath);
        if (overlayIt !=
            loadContext.materialDefinitionReload
                ->includeOverlays.end())
        {
            loadPlan.baseShaderCompileRequest
                ->source.parameterIncludeBytes =
                overlayIt->second;
        }

        const bool sourceChanged =
            std::find(
                loadContext.materialDefinitionReload
                    ->changedSources.begin(),
                loadContext.materialDefinitionReload
                    ->changedSources.end(),
                materialSourceIdentity) !=
            loadContext.materialDefinitionReload
                ->changedSources.end();
        if (sourceChanged &&
            !loadPlan.baseShaderCompileRequest
                ->source.parameterIncludeBytes.has_value())
        {
            throw std::runtime_error(
                "Material definition reload batch is missing its generated include overlay: " +
                materialSourceIdentity);
        }
    }
    std::shared_ptr<Material> material;
    const std::shared_ptr<Material>* cachedMaterial = resourceCache.GetMaterial(loadPlan.materialKey);
    if(cachedMaterial != nullptr && *cachedMaterial != nullptr)
    {
        material = *cachedMaterial;
    }
    else
    {
        // Material 拥有 Surface pipeline；相同 materialKey 的 MI 共用这个 Material。
        material = std::make_shared<Material>(
            pipelineFactory, 
            renderPass,
            loadPlan.surfacePassPipelineContractKey,
            loadPlan.shaderVariantKey,
            loadPlan.materialFeatureKey,
            loadPlan.materialDescriptorSchema,
            loadPlan.baseShaderCompileRequest,
            loadPlan.materialKey,
            loadPlan.cullMode,
            loadPlan.blendMode,
            loadContext.graphicsCandidateState
        );
        if (renderPass.type !=
            RenderGraphPassType::Shadow)
        {
            // 专用 ShadowCaster pipeline 同样由 Material 持有且只构建一次。
            // Shadow pass 自己的公共材质不再递归寻找 `.shadow` shader。
            MaterialShadowPipelineBuildResult shadowBuildResult =
                BuildMaterialShadowPipeline(
                pipelineFactory,
                loadContext.renderGraph.FindCanonicalShadowPass(),
                loadPlan,
                *material,
                loadContext.graphicsCandidateState);
            if (shadowBuildResult.pipeline)
            {
                material->SetShadowPipeline(
                    std::move(shadowBuildResult.pipeline),
                    shadowBuildResult.shaderArtifact,
                    shadowBuildResult.reloadRecipe);
            }
        }
    }
    const auto& shaderParameters = effectiveMaterialInstanceJson["parameters"];
    const auto& shaderTextures = effectiveMaterialInstanceJson.contains("textures")
        ? effectiveMaterialInstanceJson["textures"]
        : nlohmann::json::object();
    std::shared_ptr<MaterialInstance> materialInstance = material->CreateInstance();
    materialInstance->SetName(loadPlan.materialInstanceKey);

    ApplyParameterJson(
        *materialInstance,
        shaderParameters,
        materialInstancePath);

    // Texture bindings are loaded through texture asset descriptors and cached
    // by their resolved source/semantic/create-desc tuple.
    for(const auto& [name, value] : shaderTextures.items())
    {
        const std::string& textureName = name;
        if (!value.is_string())
        {
            throw std::runtime_error(
                "Texture binding must be a string in material instance: " + std::string(materialInstancePath));
        }

        const std::string textureAssetPath = value.get<std::string>();
        ValidateTextureAssetReference(textureName, textureAssetPath, materialInstancePath);
        const TextureBindingLoadDesc textureLoadDesc = LoadTextureAssetDesc(textureAssetPath);
        const std::string textureCacheKey = BuildTextureCacheKey(textureLoadDesc);
        std::shared_ptr<Texture> texture;
        const std::shared_ptr<Texture>* cachedTexture = resourceCache.GetTexture(textureCacheKey);
        if(cachedTexture != nullptr && *cachedTexture != nullptr)
        {
            texture = *cachedTexture;
        }
        else
        {
            texture = std::make_shared<Texture>(
                rendererBackend,
                textureLoadDesc.source,
                ToTextureCreateDesc(textureLoadDesc));
            resourceCache.BindTexture(textureCacheKey, texture);
        }
        materialInstance->SetTexture(
            textureName,
            texture,
            textureAssetPath,
            textureCacheKey);
    }

    const std::shared_ptr<MaterialInstance>* previousInstance = nullptr;
    if (loadContext.previousWorldResources)
    {
        const auto previousIt =
            loadContext.previousWorldResources
                ->materialInstances.find(
                    loadPlan.materialInstanceKey);
        if (previousIt !=
            loadContext.previousWorldResources
                ->materialInstances.end() &&
            previousIt->second)
        {
            previousInstance = &previousIt->second;
        }
    }

    if (previousInstance != nullptr)
    {
        const MaterialInstanceStateSnapshot previousSnapshot =
            (*previousInstance)->CaptureStateSnapshot();
        const std::shared_ptr<Material> previousMaterial =
            (*previousInstance)->GetBaseMaterial().lock();
        if (!previousMaterial)
        {
            throw std::runtime_error(
                "Cannot migrate MaterialInstance without its previous Material: " +
                loadPlan.materialInstanceKey);
        }

        for (const auto& [name, value] :
             previousSnapshot.parameters)
        {
            const MaterialParameterSchemaEntry* newParameter =
                FindParameterSchema(
                    material->GetMaterialDescriptorSchema(),
                    name);
            if (newParameter == nullptr)
            {
                continue;
            }
            if (!ParameterValueMatchesSchema(
                    value,
                    *newParameter))
            {
                throw std::runtime_error(
                    "MaterialInstance live parameter type changed during state transfer: " +
                    loadPlan.materialInstanceKey + "." + name);
            }
            SetParameterValue(
                *materialInstance,
                name,
                value);
        }

        for (const auto& [name, binding] :
             previousSnapshot.textures)
        {
            const MaterialTextureSchemaEntry* newTexture =
                FindTextureSchema(
                    material->GetMaterialDescriptorSchema(),
                    name);
            if (newTexture == nullptr)
            {
                continue;
            }
            const MaterialTextureSchemaEntry* oldTexture =
                FindTextureSchema(
                    previousMaterial
                        ->GetMaterialDescriptorSchema(),
                    name);
            if (oldTexture == nullptr ||
                oldTexture->binding != newTexture->binding ||
                oldTexture->glslType != newTexture->glslType)
            {
                throw std::runtime_error(
                    "MaterialInstance live texture binding changed during state transfer: " +
                    loadPlan.materialInstanceKey + "." + name);
            }
            materialInstance->SetTexture(
                name,
                binding.texture,
                binding.textureAssetIdentity,
                binding.textureCacheIdentity);
            if (binding.textureCacheIdentity &&
                binding.texture)
            {
                resourceCache.BindTexture(
                    *binding.textureCacheIdentity,
                    binding.texture);
            }
        }
    }

    // 派生 ID 和引擎 lookup texture 必须在 snapshot 恢复后重新注入，
    // 然后才进行最终 descriptor/schema 校验。
    ReapplyResolvedSubsurfaceMaterialIds(
        resolvedSubsurfaceAssets,
        *materialInstance);
    ReapplyResolvedEyeMaterialIds(
        resolvedEyeAssets,
        *materialInstance);
    BindEngineSubsurfaceTextures(
        *materialInstance,
        *material,
        *subsurfaceResources);

    const MaterialInstanceStateSnapshot candidateSnapshot =
        materialInstance->CaptureStateSnapshot();
    MaterialInstanceValidator::Validate(
        materialInstancePath,
        BuildCandidateValidationJson(
            effectiveMaterialInstanceJson,
            candidateSnapshot),
        material->GetMaterialDescriptorSchema(),
        material->GetActiveShaderBindings());

    resourceCache.BindMaterial(loadPlan.materialKey, material);
    resourceCache.BindMaterialInstance(loadPlan.materialInstanceKey, materialInstance);

    return materialInstance;
}

} // namespace VL
