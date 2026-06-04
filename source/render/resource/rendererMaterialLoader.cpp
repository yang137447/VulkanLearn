#include "render/resource/rendererMaterialLoader.h"

#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "material.h"
#include "material/loader/materialInstanceResolver.h"
#include "materialInstance.h"
#include "materialInstanceValidator.h"
#include "pipeline/graphicsPipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "texture.h"
#include "textureAssetLoader.h"

namespace
{

vk::CompareOp ParseDepthCompareOp(const std::string& compareOp)
{
    if (compareOp == "lessOrEqual")
    {
        return vk::CompareOp::eLessOrEqual;
    }
    if (compareOp == "equal")
    {
        return vk::CompareOp::eEqual;
    }
    if (compareOp == "greater")
    {
        return vk::CompareOp::eGreater;
    }
    if (compareOp == "greaterOrEqual")
    {
        return vk::CompareOp::eGreaterOrEqual;
    }
    if (compareOp == "always")
    {
        return vk::CompareOp::eAlways;
    }
    return vk::CompareOp::eLess;
}

} // namespace

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

    auto& renderGraphJson = CommonFunction::InitRenderGraphJson();
    for(auto& passJson : renderGraphJson["passes"])
    {
        bool bNeedCreateMaterial = passJson.value("needCreateMaterial", false);
        if(!bNeedCreateMaterial)
        {
            continue;
        }
        std::string passName = passJson["name"];
        std::string materialInstancePath = passJson["materialInstancePath"];
        
        GraphicsPipelineStateDesc pipelineStateDesc;
        if (passJson.contains("pipelineState"))
        {
            const auto& pipelineStateJson = passJson["pipelineState"];
            pipelineStateDesc.bUseVertexInput = pipelineStateJson.value("useVertexInput", pipelineStateDesc.bUseVertexInput);
            pipelineStateDesc.bDepthTestEnable = pipelineStateJson.value("depthTestEnable", pipelineStateDesc.bDepthTestEnable);
            pipelineStateDesc.bDepthWriteEnable = pipelineStateJson.value("depthWriteEnable", pipelineStateDesc.bDepthWriteEnable);
            pipelineStateDesc.depthCompareOp = ParseDepthCompareOp(pipelineStateJson.value("depthCompareOp", std::string("less")));
        }

        Renderpass& renderPass = renderGraph.GetRenderpasses().at(passName);
        std::shared_ptr<MaterialInstance> materialInstance =
            LoadMaterialInstance(materialInstancePath, renderPass.sampleCount, passName, pipelineStateDesc);
        
        renderPass.materialInstance = materialInstance;
    }
}

std::shared_ptr<MaterialInstance> RendererMaterialLoader::LoadMaterialInstance(
    std::string_view materialInstancePath,
    vk::SampleCountFlagBits sampleCount,
    std::string_view passName,
    const GraphicsPipelineStateDesc& pipelineStateDesc) const
{
    RenderGraph& renderGraph = RenderGraph::GetInstance();
    RendererResourceCache& resourceCache = RendererResourceCache::GetInstance();

    std::ifstream materialInstanceFile(CommonFunction::Path(materialInstancePath.data()));
    nlohmann::json materialInstanceJson;
    materialInstanceFile >> materialInstanceJson;
    MaterialInstanceResolveResult materialInstanceResolveResult =
        MaterialInstanceResolver::Resolve(materialInstancePath, materialInstanceJson);
    const nlohmann::json& effectiveMaterialInstanceJson = materialInstanceResolveResult.effectiveMaterialInstanceJson;
    MaterialInstanceBuildPlan loadPlan = MaterialInstanceValidator::BuildLoadPlan(materialInstancePath, passName, sampleCount, pipelineStateDesc, effectiveMaterialInstanceJson);
    const std::string passNameKey(passName);
    Renderpass& renderPass = renderGraph.GetRenderpasses().at(passNameKey);
    std::shared_ptr<Material> material;
    const std::shared_ptr<Material>* cachedMaterial = resourceCache.GetMaterial(loadPlan.materialKey);
    if(cachedMaterial != nullptr && *cachedMaterial != nullptr)
    {
        material = *cachedMaterial;
    }
    else
    {
        material = std::make_shared<Material>(
            pipelineFactory, 
            &renderPass.renderPass,
            loadPlan.shaderVariantKey,
            loadPlan.materialKey,
            sampleCount,
            renderPass.colorAttachmentCount,
            loadPlan.pipelineStateDesc,
            loadPlan.bIsShadowPass
        );
    }
    const auto& shaderParameters = effectiveMaterialInstanceJson["parameters"];
    const auto& shaderTextures = effectiveMaterialInstanceJson.contains("textures")
        ? effectiveMaterialInstanceJson["textures"]
        : nlohmann::json::object();
    MaterialInstanceValidator::Validate(materialInstancePath, effectiveMaterialInstanceJson, material->GetRenderPipeline()->GetShaderBindings());

    std::shared_ptr<MaterialInstance> materialInstance;
    const std::shared_ptr<MaterialInstance>* cachedMaterialInstance =
        resourceCache.GetMaterialInstance(loadPlan.materialInstanceKey);
    if(cachedMaterialInstance != nullptr && *cachedMaterialInstance != nullptr)
    {
        materialInstance = *cachedMaterialInstance;
    }
    else
    {
        materialInstance = material->CreateInstance();
        materialInstance->SetName(loadPlan.materialInstanceKey);
    }

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
    if (passName != "geometry")
    {
        resourceCache.BindMaterial(loadPlan.shaderName, material);
    }
    resourceCache.BindMaterialInstance(loadPlan.materialInstanceKey, materialInstance);

    return materialInstance;
}

} // namespace VL
