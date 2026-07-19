#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

// Small shared helpers used by material asset validation, resolving, and include generation.
// Keep this namespace limited to pure formatting/parsing helpers; file IO and policy decisions
// should live in loader, validation, or generator classes.
namespace MaterialAssetUtils
{
    struct ShadingModelDesc
    {
        std::string_view name;
        uint32_t id;
        std::string_view shaderDefine;
    };

    inline constexpr std::array<ShadingModelDesc, 10> kShadingModels = {{
        {"DefaultLit", 0u, "SHADING_MODEL_DEFAULT_LIT"},
        {"Unlit", 1u, "SHADING_MODEL_UNLIT"},
        {"Subsurface", 2u, "SHADING_MODEL_SUBSURFACE"},
        {"PreintegratedSkin", 3u, "SHADING_MODEL_PREINTEGRATED_SKIN"},
        {"ClearCoat", 4u, "SHADING_MODEL_CLEAR_COAT"},
        {"SubsurfaceProfile", 5u, "SHADING_MODEL_SUBSURFACE_PROFILE"},
        {"TwoSidedFoliage", 6u, "SHADING_MODEL_TWOSIDED_FOLIAGE"},
        {"Hair", 7u, "SHADING_MODEL_HAIR"},
        {"Cloth", 8u, "SHADING_MODEL_CLOTH"},
        {"Eye", 9u, "SHADING_MODEL_EYE"},
    }};

    inline const ShadingModelDesc& FindShadingModel(std::string_view shadingModel)
    {
        for (const ShadingModelDesc& desc : kShadingModels)
        {
            if (shadingModel == desc.name)
            {
                return desc;
            }
        }
        throw std::runtime_error("Unsupported shadingModel: " + std::string(shadingModel));
    }

    inline std::string ToGenericString(const std::filesystem::path& path)
    {
        return path.generic_string();
    }

    inline std::string NormalizeAssetPath(std::string_view path)
    {
        return std::filesystem::path(path).lexically_normal().generic_string();
    }

    inline std::string Trim(std::string value)
    {
        const auto beginIt = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
        const auto endIt = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        if (beginIt >= endIt)
        {
            return {};
        }
        return std::string(beginIt, endIt);
    }

    inline std::string ToUpperIdentifier(std::string value)
    {
        for (char& ch : value)
        {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (std::isalnum(uch) != 0)
            {
                ch = static_cast<char>(std::toupper(uch));
            }
            else
            {
                ch = '_';
            }
        }
        return value;
    }

    inline bool IsNumericMacroValue(const nlohmann::json& value)
    {
        return value.is_number_integer() || value.is_number_unsigned() || value.is_number_float();
    }

    inline std::string MacroValueToString(const nlohmann::json& value)
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

    inline bool IsMaterialParameterType(std::string_view type)
    {
        return type == "float" || type == "vec2" || type == "vec3" || type == "vec4";
    }

    inline bool MaterialParameterValueMatchesType(
        const nlohmann::json& value,
        std::string_view type)
    {
        if (type == "float") return value.is_number();
        if (!value.is_array()) return false;
        if (type == "vec2") return value.size() == 2;
        if (type == "vec3") return value.size() == 3;
        if (type == "vec4") return value.size() == 4;
        return false;
    }

    inline uint32_t ShadingModelToId(std::string_view shadingModel)
    {
        return FindShadingModel(shadingModel).id;
    }

    inline std::string ShadingModelToShaderDefine(std::string_view shadingModel)
    {
        return std::string(FindShadingModel(shadingModel).shaderDefine);
    }
}
