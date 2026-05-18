#include "materialInstanceValidator.h"

#include <stdexcept>
#include "commonFunction.h"

namespace
{
    // 从材质实例配置中读取渲染模式，决定后续 shader variant 与默认管线状态。
    RenderMode ParseRenderMode(const nlohmann::json& materialInstanceJson)
    {
        if (!materialInstanceJson.contains("renderStates"))
        {
            return RenderMode::Opaque;
        }
        const auto& renderStatesJson = materialInstanceJson["renderStates"];
        const std::string renderMode = renderStatesJson.value("renderMode", std::string("Opaque"));
        if (renderMode == "Opaque")
        {
            return RenderMode::Opaque;
        }
        if (renderMode == "OpaqueClip")
        {
            return RenderMode::OpaqueClip;
        }
        if (renderMode == "TransparentAlphaBlend")
        {
            return RenderMode::TransparentAlphaBlend;
        }
        if (renderMode == "TransparentAdditive")
        {
            return RenderMode::TransparentAdditive;
        }
        throw std::runtime_error("Unsupported renderMode: " + renderMode);
    }

    vk::CullModeFlags ParseCullMode(const nlohmann::json& materialInstanceJson)
    {
        if (!materialInstanceJson.contains("renderStates"))
        {
            return vk::CullModeFlagBits::eBack;
        }
        const auto& renderStatesJson = materialInstanceJson["renderStates"];
        const std::string cullMode = renderStatesJson.value("cullMode", std::string("Back"));
        if (cullMode == "Back")
        {
            return vk::CullModeFlagBits::eBack;
        }
        if (cullMode == "None")
        {
            return vk::CullModeFlagBits::eNone;
        }
        throw std::runtime_error("Unsupported cullMode: " + cullMode);
    }

    // 统一整理 art macro，保证 variant key 稳定可缓存。
    std::vector<std::string> ParseArtMacros(const nlohmann::json& materialInstanceJson)
    {
        if (!materialInstanceJson.contains("artMacros"))
        {
            return {};
        }
        return NormalizeArtMacros(materialInstanceJson["artMacros"].get<std::vector<std::string>>());
    }

    // 先按 render mode 写入一套默认状态，再由 pass 级配置进行覆盖。
    void ApplyRenderModeDefaults(RenderMode renderMode, GraphicsPipelineStateDesc& pipelineStateDesc)
    {
        switch (renderMode)
        {
        case RenderMode::Opaque:
        case RenderMode::OpaqueClip:
            pipelineStateDesc.blendMode = GraphicsPipelineBlendMode::Opaque;
            pipelineStateDesc.bDepthTestEnable = true;
            pipelineStateDesc.bDepthWriteEnable = true;
            break;
        case RenderMode::TransparentAlphaBlend:
            pipelineStateDesc.blendMode = GraphicsPipelineBlendMode::AlphaBlend;
            pipelineStateDesc.bDepthTestEnable = true;
            pipelineStateDesc.bDepthWriteEnable = false;
            break;
        case RenderMode::TransparentAdditive:
            pipelineStateDesc.blendMode = GraphicsPipelineBlendMode::Additive;
            pipelineStateDesc.bDepthTestEnable = true;
            pipelineStateDesc.bDepthWriteEnable = false;
            break;
        default:
            throw std::runtime_error("Unsupported renderMode when applying pipeline defaults");
        }
    }

    std::string BuildMaterialCacheKey(
        std::string_view passName,
        const ShaderVariantKey& shaderVariantKey,
        vk::SampleCountFlagBits sampleCount,
        const GraphicsPipelineStateDesc& pipelineStateDesc,
        bool bIsShadowPass)
    {
        return std::string(passName) + "|" +
            shaderVariantKey.GetNormalizedKey() + "|" +
            "sampleCount=" + std::to_string(static_cast<uint32_t>(sampleCount)) + "|" +
            "vertexInput=" + std::to_string(pipelineStateDesc.bUseVertexInput) + "|" +
            "depthTest=" + std::to_string(pipelineStateDesc.bDepthTestEnable) + "|" +
            "depthWrite=" + std::to_string(pipelineStateDesc.bDepthWriteEnable) + "|" +
            "depthCompare=" + std::to_string(static_cast<uint32_t>(pipelineStateDesc.depthCompareOp)) + "|" +
            "cullMode=" + std::to_string(static_cast<uint32_t>(pipelineStateDesc.cullMode)) + "|" +
            "blendMode=" + std::to_string(static_cast<uint32_t>(pipelineStateDesc.blendMode)) + "|" +
            "shadowPass=" + std::to_string(bIsShadowPass);
    }

    std::string BuildMaterialInstanceCacheKey(std::string_view materialInstancePath, const std::string& materialKey)
    {
        return std::string(materialInstancePath) + "|" + materialKey;
    }

    // 只要求材质实例提供 shader 声明过的纹理名；纹理类型正确性由资源加载阶段保证。
    void ValidateTextures(
        std::string_view materialInstancePath,
        const nlohmann::json& shaderTextures,
        const std::vector<ShaderBinding>& shaderBindings)
    {
        std::vector<std::string> expectedTextureNames;
        for (const auto& binding : shaderBindings)
        {
            if (binding.set == MaterialSetIndex && binding.type == vk::DescriptorType::eCombinedImageSampler)
            {
                expectedTextureNames.push_back(binding.name);
            }
        }

        bool texturesMatch = shaderTextures.is_object();
        if (texturesMatch)
        {
            for (const auto& textureName : expectedTextureNames)
            {
                if (!shaderTextures.contains(textureName))
                {
                    texturesMatch = false;
                    break;
                }
            }
        }

        if (!texturesMatch)
        {
            throw std::runtime_error(std::string("Missing required material textures in material instance: ") + materialInstancePath.data());
        }
    }

    // 当前仍按 MaterialSet binding 0 的 UBO 布局校验参数名与大小。
    void ValidateParameters(
        std::string_view materialInstancePath,
        const nlohmann::json& shaderParameters,
        const std::vector<ShaderBinding>& shaderBindings)
    {
        const uint32_t parameterCount = shaderParameters.size();
        bool validParameters = false;
        bool hasMaterialUbo = false;
        for (const auto& binding : shaderBindings)
        {
            if (binding.set != MaterialSetIndex || binding.binding != 0)
            {
                continue;
            }
            if (binding.type != vk::DescriptorType::eUniformBuffer)
            {
                continue;
            }

            hasMaterialUbo = true;
            if (binding.memberCount != parameterCount)
            {
                break;
            }

            bool allMatch = true;
            for (uint32_t i = 0; i < binding.memberNames.size(); ++i)
            {
                const std::string& memberName = binding.memberNames[i];
                if (!shaderParameters.contains(memberName))
                {
                    allMatch = false;
                    break;
                }

                const auto& paramValue = shaderParameters[memberName];
                const size_t paramSize = JsonParser::ParseValueSize(paramValue);
                if (i >= binding.members.size() || paramSize != binding.members[i])
                {
                    allMatch = false;
                    break;
                }
            }

            if (allMatch)
            {
                validParameters = true;
                break;
            }
        }

        if (!validParameters && !(parameterCount == 0 && !hasMaterialUbo))
        {
            throw std::runtime_error(std::string("Parameter types or sizes mismatch in material instance: ") + materialInstancePath.data());
        }
    }
}

MaterialInstanceBuildPlan MaterialInstanceValidator::BuildLoadPlan(
    std::string_view materialInstancePath,
    std::string_view passName,
    vk::SampleCountFlagBits sampleCount,
    const GraphicsPipelineStateDesc& passPipelineStateDesc,
    const nlohmann::json& materialInstanceJson)
{
    MaterialInstanceBuildPlan loadPlan;
    loadPlan.shaderName = materialInstanceJson.at("shader").get<std::string>();
    loadPlan.shaderVariantKey.shaderName = loadPlan.shaderName;
    loadPlan.shaderVariantKey.renderMode = ParseRenderMode(materialInstanceJson);
    loadPlan.shaderVariantKey.artMacros = ParseArtMacros(materialInstanceJson);

    ApplyRenderModeDefaults(loadPlan.shaderVariantKey.renderMode, loadPlan.pipelineStateDesc);
    loadPlan.pipelineStateDesc.bUseVertexInput = passPipelineStateDesc.bUseVertexInput;
    loadPlan.pipelineStateDesc.bDepthTestEnable = passPipelineStateDesc.bDepthTestEnable;
    loadPlan.pipelineStateDesc.bDepthWriteEnable = passPipelineStateDesc.bDepthWriteEnable;
    loadPlan.pipelineStateDesc.depthCompareOp = passPipelineStateDesc.depthCompareOp;
    loadPlan.pipelineStateDesc.cullMode = ParseCullMode(materialInstanceJson);

    loadPlan.bIsShadowPass = passName == "shadow";
    loadPlan.materialKey = BuildMaterialCacheKey(
        passName,
        loadPlan.shaderVariantKey,
        sampleCount,
        loadPlan.pipelineStateDesc,
        loadPlan.bIsShadowPass);
    loadPlan.materialInstanceKey = BuildMaterialInstanceCacheKey(materialInstancePath, loadPlan.materialKey);
    return loadPlan;
}

void MaterialInstanceValidator::Validate(
    std::string_view materialInstancePath,
    const nlohmann::json& materialInstanceJson,
    const std::vector<ShaderBinding>& shaderBindings)
{
    const auto& shaderTextures = materialInstanceJson.contains("textures")
        ? materialInstanceJson["textures"]
        : nlohmann::json::object();
    const auto& shaderParameters = materialInstanceJson["parameters"];
    ValidateTextures(materialInstancePath, shaderTextures, shaderBindings);
    ValidateParameters(materialInstancePath, shaderParameters, shaderBindings);
}
