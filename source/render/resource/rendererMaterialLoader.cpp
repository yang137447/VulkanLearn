#include "render/resource/rendererMaterialLoader.h"

#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "material.h"
#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"
#include "materialInstance.h"
#include "materialInstanceValidator.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "render/shadow/materialShadowPipelineBuilder.h"
#include "renderGraph.h"
#include "texture.h"
#include "textureAssetLoader.h"

namespace VL
{

RendererMaterialLoader::RendererMaterialLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
{
}

void RendererMaterialLoader::LoadPassMaterials() const
{
    RenderGraph& renderGraph = RenderGraph::GetInstance();

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
        
        renderPass.materialInstance = materialInstance;
    }
}

std::shared_ptr<MaterialInstance> RendererMaterialLoader::LoadMaterialInstance(
    std::string_view materialInstancePath,
    Renderpass& renderPass) const
{
    RendererResourceCache& resourceCache = RendererResourceCache::GetInstance();
    // MI 以规范化资产路径作为唯一身份。同一份 MI 再次加载时必须返回同一对象，
    // 但它的 Material 管线合同仍需与目标 pass 一致，避免把旧图管线带入新图。
    const std::string materialInstanceKey =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    const std::shared_ptr<MaterialInstance>* cachedMaterialInstance =
        resourceCache.GetMaterialInstance(materialInstanceKey);
    if (cachedMaterialInstance != nullptr && *cachedMaterialInstance != nullptr)
    {
        std::shared_ptr<Material> cachedBaseMaterial =
            (*cachedMaterialInstance)->GetBaseMaterial().lock();
        if (!cachedBaseMaterial ||
            cachedBaseMaterial->GetPassPipelineContractKey() !=
                renderPass.pipelineContractKey)
        {
            throw std::runtime_error(
                "Material instance asset '" + materialInstanceKey +
                "' was requested with an incompatible pass pipeline contract");
        }
        return *cachedMaterialInstance;
    }

    const std::string materialInstancePathString(materialInstancePath);
    std::ifstream materialInstanceFile(CommonFunction::Path(materialInstancePathString));
    nlohmann::json materialInstanceJson;
    materialInstanceFile >> materialInstanceJson;
    MaterialInstanceResolveResult materialInstanceResolveResult =
        MaterialInstanceResolver::Resolve(materialInstancePath, materialInstanceJson);
    const nlohmann::json& effectiveMaterialInstanceJson = materialInstanceResolveResult.effectiveMaterialInstanceJson;
    // Resolve 先合并材质定义与 MI 覆写，BuildLoadPlan 再一次性生成 shader variant、
    // 固定管线状态和缓存 key；后续步骤只消费结果，不重复解释 JSON。
    MaterialInstanceBuildPlan loadPlan = MaterialInstanceValidator::BuildLoadPlan(
        materialInstancePath,
        renderPass.pipelineContractKey,
        effectiveMaterialInstanceJson,
        materialInstanceResolveResult.materialPath,
        materialInstanceResolveResult.materialJson);
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
            &renderPass.renderPass,
            renderPass.pipelineContractKey,
            loadPlan.shaderVariantKey,
            loadPlan.materialFeatureKey,
            loadPlan.materialDescriptorSchema,
            loadPlan.baseShaderCompileRequest,
            loadPlan.materialKey,
            loadPlan.cullMode,
            loadPlan.blendMode
        );
        if (renderPass.type != "shadow")
        {
            // 专用 ShadowCaster pipeline 同样由 Material 持有且只构建一次。
            // Shadow pass 自己的公共材质不再递归寻找 `.shadow` shader。
            material->SetShadowPipeline(BuildMaterialShadowPipeline(
                pipelineFactory,
                RenderGraph::GetInstance().FindCanonicalShadowPass(),
                loadPlan,
                *material));
        }
    }
    const auto& shaderParameters = effectiveMaterialInstanceJson["parameters"];
    const auto& shaderTextures = effectiveMaterialInstanceJson.contains("textures")
        ? effectiveMaterialInstanceJson["textures"]
        : nlohmann::json::object();
    MaterialInstanceValidator::Validate(
        materialInstancePath,
        effectiveMaterialInstanceJson,
        material->GetMaterialDescriptorSchema(),
        material->GetActiveShaderBindings());

    std::shared_ptr<MaterialInstance> materialInstance = material->CreateInstance();
    materialInstance->SetName(loadPlan.materialInstanceKey);

    // Parameter JSON was validated against shader reflection above; this block
    // only copies typed values into MaterialInstance storage.
    for(const auto& [name, value]  : shaderParameters.items())
    {
        const std::string& paramName = name;
        uint32_t paramSize = JsonParser::ParseValueSize(value);
        if(paramSize == sizeof(float))
        {
            auto paramValue = JsonParser::ParseValue<float>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector2f))
        {
            auto paramValue = JsonParser::ParseValue<Eigen::Vector2f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector3f))
        {
            auto paramValue = JsonParser::ParseValue<Eigen::Vector3f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else if(paramSize == sizeof(Eigen::Vector4f))
        {
            auto paramValue = JsonParser::ParseValue<Eigen::Vector4f>(value);
            materialInstance->SetParameter(paramName, paramValue);
        }
        else 
        {
            throw std::runtime_error(std::string("Unsupported parameter type or size in material instance: ") + materialInstancePath.data());
        }
    }

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
        materialInstance->SetTexture(textureName, texture);
    }

    resourceCache.BindMaterial(loadPlan.materialKey, material);
    resourceCache.BindMaterialInstance(loadPlan.materialInstanceKey, materialInstance);

    return materialInstance;
}

} // namespace VL
