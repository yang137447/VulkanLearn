#pragma once

#include <algorithm>
#include <cctype>
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
    inline std::string ToGenericString(const std::filesystem::path& path)
    {
        return path.generic_string();
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

    inline uint32_t ShadingModelToId(std::string_view shadingModel)
    {
        if (shadingModel == "DefaultLit")
        {
            return 0u;
        }
        if (shadingModel == "Unlit")
        {
            return 1u;
        }
        throw std::runtime_error("Unsupported shadingModel: " + std::string(shadingModel));
    }
}
