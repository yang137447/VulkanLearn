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

// 材质资产校验、解析和 include 生成共享的纯格式化/解析工具。
// 文件 IO 与策略判断应留在 loader、validator 或 generator，避免工具层承担运行时职责。
namespace MaterialAssetUtils
{
    struct ShadingModelDesc
    {
        std::string_view name;
        uint32_t id;
        std::string_view shaderDefine;
    };

    // ID 会写入 GBuffer/MaterialSurface，并与 GLSL 常量一一对应；只能追加，不能重排。
    inline constexpr std::array<ShadingModelDesc, 11> kShadingModels = {{
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
        {"ThinTranslucent", 10u, "SHADING_MODEL_THIN_TRANSLUCENT"},
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

    inline std::string NormalizeShaderGlslRelativePath(std::string_view path)
    {
        const std::filesystem::path normalizedPath =
            std::filesystem::path(path).lexically_normal();
        std::filesystem::path relativePath;
        bool foundGlsl = false;
        for (const std::filesystem::path& part : normalizedPath)
        {
            if (foundGlsl)
            {
                relativePath /= part;
            }
            else if (part == "glsl")
            {
                foundGlsl = true;
            }
        }
        if (!foundGlsl || relativePath.empty())
        {
            throw std::runtime_error(
                "Shader source path must be under shader/glsl: " +
                std::string(path));
        }
        return relativePath.generic_string();
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
