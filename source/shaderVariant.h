#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "shader/build/contentHash.h"

enum class RenderMode
{
    Opaque,
    OpaqueClip,
    // Eye 使用独立 Forward Opaque pass，避免占用普通 GBuffer。
    ForwardOpaque,
    ForwardEyeInner,
    ForwardEyeCornea,
    TransparentAlphaBlend,
    TransparentAdditive,
    // 薄透射走 forwardTransparent，并根据设备能力选择双源或标量降级混合。
    ThinTranslucent
};

// 双源混合能力由引擎根据物理设备特性写入 variant，材质资产不能自行覆盖。
// 该宏同时参与 shader identity，确保原生双源路径和标量降级路径拥有独立缓存产物。
inline constexpr std::string_view kThinTranslucentDualSourceMacro =
    "VL_THIN_TRANSLUCENT_DUAL_SOURCE";

inline bool IsForwardOpaqueRenderMode(RenderMode renderMode)
{
    return renderMode == RenderMode::ForwardOpaque;
}

inline bool IsForwardEyeLayerRenderMode(RenderMode renderMode)
{
    return renderMode == RenderMode::ForwardEyeInner ||
        renderMode == RenderMode::ForwardEyeCornea;
}

inline bool IsGeometryRenderMode(RenderMode renderMode)
{
    return renderMode == RenderMode::Opaque ||
        renderMode == RenderMode::OpaqueClip;
}

inline bool IsTransparentRenderMode(RenderMode renderMode)
{
    return renderMode == RenderMode::TransparentAlphaBlend ||
        renderMode == RenderMode::TransparentAdditive ||
        renderMode == RenderMode::ThinTranslucent;
}

inline std::string RenderModeToString(RenderMode renderMode)
{
    switch (renderMode)
    {
    case RenderMode::Opaque:
        return "Opaque";
    case RenderMode::OpaqueClip:
        return "OpaqueClip";
    case RenderMode::ForwardOpaque:
        return "ForwardOpaque";
    case RenderMode::ForwardEyeInner:
        return "ForwardEyeInner";
    case RenderMode::ForwardEyeCornea:
        return "ForwardEyeCornea";
    case RenderMode::TransparentAlphaBlend:
        return "TransparentAlphaBlend";
    case RenderMode::TransparentAdditive:
        return "TransparentAdditive";
    case RenderMode::ThinTranslucent:
        return "ThinTranslucent";
    default:
        throw std::runtime_error("Unknown RenderMode");
    }
}

inline std::vector<std::string> NormalizeMaterialMacros(std::vector<std::string> macros)
{
    for (std::string& macro : macros)
    {
        const auto beginIt = std::find_if_not(macro.begin(), macro.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto endIt = std::find_if_not(macro.rbegin(), macro.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
        if (beginIt >= endIt)
        {
            macro.clear();
            continue;
        }
        macro = std::string(beginIt, endIt);
    }
    macros.erase(
        std::remove_if(
            macros.begin(),
            macros.end(),
            [](const std::string& macro)
            {
                return macro.empty();
            }),
        macros.end());
    std::sort(macros.begin(), macros.end());
    macros.erase(std::unique(macros.begin(), macros.end()), macros.end());
    return macros;
}

inline std::string_view TrimMaterialMacroToken(std::string_view token)
{
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())) != 0)
    {
        token.remove_prefix(1);
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0)
    {
        token.remove_suffix(1);
    }
    return token;
}

// 宏列表允许同时保存 NAME 和 NAME=VALUE 两种形式；按语义解析名称，
// 避免调用方依赖空格或等号两侧的具体格式。
inline bool HasMaterialMacroName(
    const std::vector<std::string>& macros,
    std::string_view macroName)
{
    for (const std::string& macro : macros)
    {
        const std::string_view macroView = macro;
        const size_t separator = macroView.find('=');
        const std::string_view parsedName = TrimMaterialMacroToken(
            separator == std::string_view::npos
                ? macroView
                : macroView.substr(0, separator));
        if (parsedName == macroName)
        {
            return true;
        }
    }
    return false;
}

// 仅匹配显式 NAME=VALUE。能力宏必须区分 0/1，不能把“宏存在”误判为“能力开启”。
inline bool HasMaterialMacroValue(
    const std::vector<std::string>& macros,
    std::string_view macroName,
    std::string_view expectedValue)
{
    for (const std::string& macro : macros)
    {
        const std::string_view macroView = macro;
        const size_t separator = macroView.find('=');
        if (separator == std::string_view::npos)
        {
            continue;
        }

        const std::string_view parsedName = TrimMaterialMacroToken(
            macroView.substr(0, separator));
        const std::string_view parsedValue = TrimMaterialMacroToken(
            macroView.substr(separator + 1));
        if (parsedName == macroName && parsedValue == expectedValue)
        {
            return true;
        }
    }
    return false;
}

struct ShaderVariantKey
{
    std::string shaderName;
    RenderMode renderMode = RenderMode::Opaque;
    std::string shadingModelMacro = "SHADING_MODEL_DEFAULT_LIT";
    std::vector<std::string> macros;

    bool operator==(const ShaderVariantKey& other) const
    {
        return shaderName == other.shaderName &&
            renderMode == other.renderMode &&
            shadingModelMacro == other.shadingModelMacro &&
            macros == other.macros;
    }

    bool IsDefaultVariant() const
    {
        return renderMode == RenderMode::Opaque &&
            shadingModelMacro == "SHADING_MODEL_DEFAULT_LIT" &&
            macros.empty();
    }

    std::string GetNormalizedKey() const
    {
        std::ostringstream oss;
        oss << "shaderName=" << shaderName
            << "|renderMode=" << RenderModeToString(renderMode)
            << "|shadingModel=" << shadingModelMacro
            << "|macros=";
        for (size_t i = 0; i < macros.size(); ++i)
        {
            if (i > 0)
            {
                oss << ",";
            }
            oss << macros[i];
        }
        return oss.str();
    }

    std::string GetVariantHash() const
    {
        VL::CanonicalFieldHasher hasher("GraphicsShaderLogicalBuildIdV1");
        hasher.AddString("normalizedKey", GetNormalizedKey());
        return hasher.Finalize().ToHex();
    }

    std::string GetDisplayName() const
    {
        if (IsDefaultVariant())
        {
            return shaderName;
        }
        return shaderName + "@" + GetVariantHash();
    }

    std::string GetStageSpvRelativePath(const std::string& stageExtension) const
    {
        if (IsDefaultVariant())
        {
            return shaderName + "_" + stageExtension + ".spv";
        }
        return shaderName + "/" + GetVariantHash() + "." + stageExtension + ".spv";
    }

    std::string GetStageDebugRelativePath(const std::string& stageExtension) const
    {
        if (IsDefaultVariant())
        {
            return shaderName + "_" + stageExtension + ".debug";
        }
        return shaderName + "/" + GetVariantHash() + "." + stageExtension + ".debug";
    }

    std::string GetShortDebugString() const
    {
        return GetDisplayName() + " [" + RenderModeToString(renderMode) + ", " + shadingModelMacro + "]";
    }
};

struct ShaderVariantKeyHash
{
    size_t operator()(const ShaderVariantKey& key) const
    {
        return std::hash<std::string>{}(key.GetNormalizedKey());
    }
};
