#include "materialInstanceValidator.h"

#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <cmath>
#include "commonFunction.h"
#include "material/materialAssetUtils.h"

namespace
{
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
        if (cullMode == "Front")
        {
            return vk::CullModeFlagBits::eFront;
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

    GraphicsPipelineBlendMode ResolveRenderModeBlendMode(
        RenderMode renderMode,
        bool supportsDualSourceBlend)
    {
        switch (renderMode)
        {
        case RenderMode::Opaque:
        case RenderMode::OpaqueClip:
        case RenderMode::ForwardOpaque:
        case RenderMode::ForwardEyeInner:
            return GraphicsPipelineBlendMode::Opaque;
        case RenderMode::ForwardEyeCornea:
            return GraphicsPipelineBlendMode::Additive;
        case RenderMode::TransparentAlphaBlend:
            return GraphicsPipelineBlendMode::AlphaBlend;
        case RenderMode::TransparentAdditive:
            return GraphicsPipelineBlendMode::Additive;
        case RenderMode::ThinTranslucent:
            // 原生路径用第二颜色源提供逐通道目标乘数；不支持时退化为
            // premultiplied alpha，并由 shader 把彩色乘数压缩成单一透明度。
            return supportsDualSourceBlend
                ? GraphicsPipelineBlendMode::ThinTranslucentDualSource
                : GraphicsPipelineBlendMode::PremultipliedAlpha;
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

namespace
{
    bool IsFiniteClothValue(const nlohmann::json& value)
    {
        return value.is_number() && std::isfinite(value.get<double>());
    }

    void ValidateClothEffectiveParameters(
        const nlohmann::json& effectiveMaterialInstanceJson,
        std::string_view materialInstancePath)
    {
        if (effectiveMaterialInstanceJson.at("shadingModel").get<std::string>() !=
            "Cloth")
        {
            return;
        }

        const auto& parameters = effectiveMaterialInstanceJson.at("parameters");
        const auto& macros = effectiveMaterialInstanceJson.at("macros");
        if (!parameters.contains("u_clothSheenColor") ||
            !parameters.contains("u_clothSheenRoughness") ||
            !parameters.contains("u_pbrFactors"))
        {
            throw std::runtime_error(
                "Cloth effective parameters are incomplete: " +
                std::string(materialInstancePath));
        }
        if (macros.contains("USE_PBR_MAP") &&
            macros.at("USE_PBR_MAP").get<double>() != 0.0)
        {
            throw std::runtime_error(
                "Cloth does not support metallic/roughness values from USE_PBR_MAP: " +
                std::string(materialInstancePath));
        }

        const auto& sheenColor = parameters.at("u_clothSheenColor");
        if (!sheenColor.is_array() || sheenColor.size() != 4)
        {
            throw std::runtime_error(
                "Cloth sheen color must be a four-component linear RGBA value: " +
                std::string(materialInstancePath));
        }
        for (const auto& component : sheenColor)
        {
            if (!IsFiniteClothValue(component) || component.get<double>() < 0.0 ||
                component.get<double>() > 1.0)
            {
                throw std::runtime_error(
                    "Cloth sheen color must be finite and within [0, 1]: " +
                    std::string(materialInstancePath));
            }
        }
        if (sheenColor[3].get<double>() != 1.0)
        {
            throw std::runtime_error(
                "Cloth sheen color alpha is reserved and must remain 1: " +
                std::string(materialInstancePath));
        }

        const auto& sheenRoughness = parameters.at("u_clothSheenRoughness");
        if (!IsFiniteClothValue(sheenRoughness) ||
            sheenRoughness.get<double>() < 0.02 ||
            sheenRoughness.get<double>() > 1.0)
        {
            throw std::runtime_error(
                "Cloth sheen roughness must be within [0.02, 1]: " +
                std::string(materialInstancePath));
        }

        const auto& pbrFactors = parameters.at("u_pbrFactors");
        if (!pbrFactors.is_array() || pbrFactors.size() != 4)
        {
            throw std::runtime_error(
                "Cloth u_pbrFactors must be a four-component value: " +
                std::string(materialInstancePath));
        }
        for (const auto& component : pbrFactors)
        {
            if (!IsFiniteClothValue(component))
            {
                throw std::runtime_error(
                    "Cloth u_pbrFactors must be finite: " +
                    std::string(materialInstancePath));
            }
        }
        if (pbrFactors[0].get<double>() < 0.02 ||
            pbrFactors[0].get<double>() > 1.0 ||
            pbrFactors[1].get<double>() != 0.0 ||
            pbrFactors[2].get<double>() < 0.0 ||
            pbrFactors[2].get<double>() > 1.0)
        {
            throw std::runtime_error(
                "Cloth base PBR values require roughness [0.02, 1], metallic 0 and AO [0, 1]: " +
                std::string(materialInstancePath));
        }
    }
}
RenderMode MaterialInstanceValidator::ResolveRenderMode(
    const nlohmann::json& effectiveMaterialInstanceJson)
{
    if (!effectiveMaterialInstanceJson.contains("renderStates"))
    {
        return RenderMode::Opaque;
    }
    const auto& renderStatesJson =
        effectiveMaterialInstanceJson["renderStates"];
    const std::string renderMode =
        renderStatesJson.value("renderMode", std::string("Opaque"));
    if (renderMode == "Opaque")
    {
        return RenderMode::Opaque;
    }
    if (renderMode == "OpaqueClip")
    {
        return RenderMode::OpaqueClip;
    }
    if (renderMode == "ForwardOpaque")
    {
        return RenderMode::ForwardOpaque;
    }
    if (renderMode == "ForwardEyeInner")
    {
        return RenderMode::ForwardEyeInner;
    }
    if (renderMode == "ForwardEyeCornea")
    {
        return RenderMode::ForwardEyeCornea;
    }
    if (renderMode == "TransparentAlphaBlend")
    {
        return RenderMode::TransparentAlphaBlend;
    }
    if (renderMode == "TransparentAdditive")
    {
        return RenderMode::TransparentAdditive;
    }
    if (renderMode == "ThinTranslucent")
    {
        return RenderMode::ThinTranslucent;
    }
    throw std::runtime_error("Unsupported renderMode: " + renderMode);
}

MaterialInstanceBuildPlan MaterialInstanceValidator::BuildLoadPlan(
    std::string_view materialInstancePath,
    const PassPipelineContractKey& passPipelineContractKey,
    const nlohmann::json& materialInstanceJson,
    std::string_view materialPath,
    const nlohmann::json& materialJson,
    bool supportsDualSourceBlend)
{
    MaterialInstanceBuildPlan loadPlan;
    loadPlan.shaderVariantKey.shaderName =
        materialInstanceJson.at("shaderName").get<std::string>();
    loadPlan.shaderVariantKey.renderMode = ResolveRenderMode(materialInstanceJson);
    const std::string shadingModel =
        materialInstanceJson.at("shadingModel").get<std::string>();
    ValidateClothEffectiveParameters(materialInstanceJson, materialInstancePath);

    const bool usesThinTranslucentShadingModel =
        shadingModel == "ThinTranslucent";
    const bool usesEyeShadingModel = shadingModel == "Eye";
    const bool usesForwardOpaqueRenderMode =
        loadPlan.shaderVariantKey.renderMode == RenderMode::ForwardOpaque;
    const bool usesEyeLayerRenderMode =
        IsForwardEyeLayerRenderMode(loadPlan.shaderVariantKey.renderMode);
    const bool usesDeferredOpaqueRenderMode =
        loadPlan.shaderVariantKey.renderMode == RenderMode::Opaque;
    // Eye 可以显式选择单壳 Forward、双壳 layer 或版本化 Deferred fallback。
    if (usesEyeShadingModel &&
        !usesForwardOpaqueRenderMode &&
        !usesEyeLayerRenderMode &&
        !usesDeferredOpaqueRenderMode)
    {
        throw std::runtime_error(
            "Eye shadingModel requires an explicit Eye render path: " +
            std::string(materialInstancePath));
    }
    if (!usesEyeShadingModel &&
        (usesForwardOpaqueRenderMode || usesEyeLayerRenderMode))
    {
        throw std::runtime_error(
            "Only Eye shadingModel may use an Eye forward render path: " +
            std::string(materialInstancePath));
    }
    // RenderMode 决定 pass、混合和排序，ShadingModel 决定闭包数学；两者必须成对，
    // 否则会出现“薄透射公式写入普通透明混合”或“普通闭包写入双源混合”的错误组合。
    if (usesThinTranslucentShadingModel !=
        (loadPlan.shaderVariantKey.renderMode == RenderMode::ThinTranslucent))
    {
        throw std::runtime_error(
            "ThinTranslucent shadingModel and renderMode must be selected together: " +
            std::string(materialInstancePath));
    }

    loadPlan.shaderVariantKey.shadingModelMacro =
        MaterialAssetUtils::ShadingModelToShaderDefine(shadingModel);
    loadPlan.shaderVariantKey.macros = ParseMaterialMacros(materialInstanceJson);

    if (loadPlan.shaderVariantKey.renderMode == RenderMode::ThinTranslucent)
    {
        // 平台能力是运行时事实，由引擎独占该宏。禁止资产写入可避免内容数据伪造
        // 设备能力，也保证 shader variant 与最终 pipeline blend state 始终一致。
        if (HasMaterialMacroName(
                loadPlan.shaderVariantKey.macros,
                kThinTranslucentDualSourceMacro))
        {
            throw std::runtime_error(
                std::string(kThinTranslucentDualSourceMacro) + " is engine-owned and cannot be authored by a material: " +
                std::string(materialInstancePath));
        }
        loadPlan.shaderVariantKey.macros.push_back(
            supportsDualSourceBlend
                ? std::string(kThinTranslucentDualSourceMacro) + "=1"
                : std::string(kThinTranslucentDualSourceMacro) + "=0");
        loadPlan.shaderVariantKey.macros = NormalizeMaterialMacros(
            std::move(loadPlan.shaderVariantKey.macros));
    }

    loadPlan.blendMode = ResolveRenderModeBlendMode(
        loadPlan.shaderVariantKey.renderMode,
        supportsDualSourceBlend);
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
