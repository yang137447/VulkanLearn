#pragma once

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

enum class RenderMode
{
    Opaque,
    OpaqueClip,
    TransparentAlphaBlend,
    TransparentAdditive
};

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

inline std::vector<std::string> NormalizeArtMacros(std::vector<std::string> artMacros)
{
    for (std::string& macro : artMacros)
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
    artMacros.erase(
        std::remove_if(
            artMacros.begin(),
            artMacros.end(),
            [](const std::string& macro)
            {
                return macro.empty();
            }),
        artMacros.end());
    std::sort(artMacros.begin(), artMacros.end());
    artMacros.erase(std::unique(artMacros.begin(), artMacros.end()), artMacros.end());
    return artMacros;
}

struct ShaderVariantKey
{
    std::string shaderName;
    RenderMode renderMode = RenderMode::Opaque;
    std::vector<std::string> artMacros;

    bool operator==(const ShaderVariantKey& other) const
    {
        return shaderName == other.shaderName &&
            renderMode == other.renderMode &&
            artMacros == other.artMacros;
    }

    bool IsDefaultVariant() const
    {
        return renderMode == RenderMode::Opaque && artMacros.empty();
    }

    std::string GetNormalizedKey() const
    {
        std::ostringstream oss;
        oss << "shader=" << shaderName
            << "|renderMode=" << RenderModeToString(renderMode)
            << "|artMacros=";
        for (size_t i = 0; i < artMacros.size(); ++i)
        {
            if (i > 0)
            {
                oss << ",";
            }
            oss << artMacros[i];
        }
        return oss.str();
    }

    std::string GetVariantHash() const
    {
        const size_t hashValue = std::hash<std::string>{}(GetNormalizedKey());
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << static_cast<unsigned long long>(hashValue);
        return oss.str();
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
        return GetDisplayName() + " [" + RenderModeToString(renderMode) + "]";
    }
};

struct ShaderVariantKeyHash
{
    size_t operator()(const ShaderVariantKey& key) const
    {
        return std::hash<std::string>{}(key.GetNormalizedKey());
    }
};
