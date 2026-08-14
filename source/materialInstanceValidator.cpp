#include "materialInstanceValidator.h"

#include <sstream>
#include <stdexcept>
#include <filesystem>
#include "commonFunction.h"
#include "material/materialAssetUtils.h"

namespace
{
    // Render mode is a static material property: it participates in shader
    // variant selection and selects the default pipeline state for the instance.
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

    bool IsNumericMacroValue(const nlohmann::json& value)
    {
        return value.is_number_integer() || value.is_number_unsigned() || value.is_number_float();
    }

    std::string MacroValueToString(const nlohmann::json& value)
    {
        if (!IsNumericMacroValue(value))
        {
            throw std::runtime_error("Material macro values must be numeric");
        }
        if (value.is_number_float())
        {
            std::ostringstream stream;
            stream << value.get<double>();
            return stream.str();
        }
        return std::to_string(value.get<int64_t>());
    }

    // 统一整理材质宏，保证 variant key 稳定可缓存。
    std::vector<std::string> ParseMaterialMacros(const nlohmann::json& materialInstanceJson)
    {
        if (!materialInstanceJson.contains("macros"))
        {
            return {};
        }

        std::vector<std::string> macros;
        for (const auto& [name, value] : materialInstanceJson["macros"].items())
        {
            macros.push_back(name + "=" + MacroValueToString(value));
        }
        return NormalizeMaterialMacros(std::move(macros));
    }

    GraphicsPipelineBlendMode ResolveRenderModeBlendMode(RenderMode renderMode)
    {
        switch (renderMode)
        {
        case RenderMode::Opaque:
        case RenderMode::OpaqueClip:
            return GraphicsPipelineBlendMode::Opaque;
        case RenderMode::TransparentAlphaBlend:
            return GraphicsPipelineBlendMode::AlphaBlend;
        case RenderMode::TransparentAdditive:
            return GraphicsPipelineBlendMode::Additive;
        default:
            throw std::runtime_error("Unsupported renderMode when resolving blend mode");
        }
    }

    std::string BuildMaterialCacheKey(
        const ShaderVariantKey& shaderVariantKey,
        const VL::MaterialFeatureKey& materialFeatureKey,
        const PassPipelineContractKey& passPipelineContractKey,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode)
    {
        return shaderVariantKey.GetNormalizedKey() + "|" +
            "features=" + materialFeatureKey.GetNormalizedKey() + "|" +
            "pass=" + passPipelineContractKey.GetNormalizedKey() + "|" +
            "cullMode=" + std::to_string(static_cast<uint32_t>(cullMode)) + "|" +
            "blendMode=" + std::to_string(static_cast<uint32_t>(blendMode));
    }

    std::string BuildMaterialInstanceCacheKey(std::string_view materialInstancePath)
    {
        return MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    }

    std::string BuildParameterIncludePath(
        std::string_view materialPath,
        const nlohmann::json& materialJson)
    {
        const std::filesystem::path normalizedPath =
            std::filesystem::path(materialPath).lexically_normal();
        std::filesystem::path relativeToGlsl;
        bool foundGlsl = false;
        for (const std::filesystem::path& part : normalizedPath)
        {
            if (foundGlsl)
            {
                relativeToGlsl /= part;
            }
            else if (part == "glsl")
            {
                foundGlsl = true;
            }
        }
        if (!foundGlsl)
        {
            throw std::runtime_error(
                "Material definition must be under shader/glsl: " + std::string(materialPath));
        }

        return (relativeToGlsl.parent_path() / "generate" /
            (materialJson.at("name").get<std::string>() + "Paramter.glsl")).generic_string();
    }

    VL::MaterialFeatureKey BuildMaterialFeatureKey(
        RenderMode renderMode,
        vk::CullModeFlags cullMode,
        const nlohmann::json& materialJson)
    {
        VL::MaterialFeatureKey features;
        features.writesEveryPixel = renderMode == RenderMode::Opaque;
        features.usesOpacityMask = renderMode == RenderMode::OpaqueClip;
        features.twoSided = cullMode == vk::CullModeFlagBits::eNone;
        features.modifiesMeshPosition = materialJson.value(
            "features",
            nlohmann::json::object()).value("modifiesMeshPosition", false);
        return features;
    }
}

MaterialInstanceBuildPlan MaterialInstanceValidator::BuildLoadPlan(
    std::string_view materialInstancePath,
    const PassPipelineContractKey& passPipelineContractKey,
    const nlohmann::json& materialInstanceJson,
    std::string_view materialPath,
    const nlohmann::json& materialJson)
{
    MaterialInstanceBuildPlan loadPlan;
    loadPlan.shaderVariantKey.shaderName =
        materialInstanceJson.at("shaderName").get<std::string>();
    loadPlan.shaderVariantKey.renderMode = ParseRenderMode(materialInstanceJson);
    loadPlan.shaderVariantKey.shadingModelMacro =
        MaterialAssetUtils::ShadingModelToShaderDefine(materialInstanceJson.at("shadingModel").get<std::string>());
    loadPlan.shaderVariantKey.macros = ParseMaterialMacros(materialInstanceJson);

    loadPlan.blendMode = ResolveRenderModeBlendMode(loadPlan.shaderVariantKey.renderMode);
    loadPlan.cullMode = ParseCullMode(materialInstanceJson);
    loadPlan.materialFeatureKey = BuildMaterialFeatureKey(
        loadPlan.shaderVariantKey.renderMode,
        loadPlan.cullMode,
        materialJson);
    loadPlan.materialDescriptorSchema =
        VL::MaterialDescriptorSchema::Build(materialJson, materialPath);

    if (materialJson.contains("shaderEvaluation"))
    {
        VL::MaterialShaderCompileRequest request;
        request.shaderVariantKey = loadPlan.shaderVariantKey;
        request.pass = VL::MaterialPass::Base;
        request.features = loadPlan.materialFeatureKey;
        request.source.materialSourcePath =
            MaterialAssetUtils::NormalizeShaderGlslRelativePath(
                materialPath);
        request.source.vertexEvaluationPath =
            materialJson.at("shaderEvaluation").at("vertex").get<std::string>();
        request.source.surfaceEvaluationPath =
            materialJson.at("shaderEvaluation").at("surface").get<std::string>();
        request.source.parameterIncludePath =
            BuildParameterIncludePath(materialPath, materialJson);
        loadPlan.baseShaderCompileRequest = std::move(request);
    }

    loadPlan.materialKey = BuildMaterialCacheKey(
        loadPlan.shaderVariantKey,
        loadPlan.materialFeatureKey,
        passPipelineContractKey,
        loadPlan.cullMode,
        loadPlan.blendMode);
    loadPlan.materialInstanceKey = BuildMaterialInstanceCacheKey(materialInstancePath);
    return loadPlan;
}

void MaterialInstanceValidator::Validate(
    std::string_view materialInstancePath,
    const nlohmann::json& materialInstanceJson,
    const VL::MaterialDescriptorSchema& descriptorSchema,
    const std::vector<ShaderBinding>& activeShaderBindings)
{
    const auto& shaderTextures = materialInstanceJson.contains("textures")
        ? materialInstanceJson["textures"]
        : nlohmann::json::object();
    descriptorSchema.ValidateInstanceValues(
        materialInstanceJson.at("parameters"),
        shaderTextures,
        activeShaderBindings,
        materialInstancePath);
}
