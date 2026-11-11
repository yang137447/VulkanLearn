#pragma once

#include <cctype>
#include <filesystem>
#include <string>

namespace VL
{
namespace UiSubsystemDetail
{

inline std::string NormalizePathForComparison(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    std::error_code error;
    std::filesystem::path normalizedPath = std::filesystem::absolute(path, error);
    if (error)
    {
        normalizedPath = path;
    }
    std::string normalized = normalizedPath.lexically_normal().generic_string();
#ifdef _WIN32
    for (char& character : normalized)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
#endif
    return normalized;
}

} // namespace UiSubsystemDetail
} // namespace VL
