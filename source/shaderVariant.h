#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "shader/build/contentHash.h"

enum class RenderMode
{
    Opaque,
    OpaqueClip,
    TransparentAlphaBlend,
    TransparentAdditive
};

inline bool IsTransparentRenderMode(RenderMode renderMode)
{
    return renderMode == RenderMode::TransparentAlphaBlend ||
        renderMode == RenderMode::TransparentAdditive;
}

inline std::string RenderModeToString(RenderMode renderMode)
{
    switch (renderMode)
    {
    case RenderMode::Opaque:
        return "Opaque";
    case RenderMode::OpaqueClip:
        return "OpaqueClip";
    case RenderMode::TransparentAlphaBlend:
        return "TransparentAlphaBlend";
    case RenderMode::TransparentAdditive:
        return "TransparentAdditive";
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
