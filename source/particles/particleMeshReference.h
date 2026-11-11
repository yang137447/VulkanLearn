#pragma once

#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VL
{

inline std::string LowerParticleMeshExtension(const std::string& path)
{
    std::string extension =
        std::filesystem::path(path).extension().string();
    for (char& character : extension)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
}

// Service 侧作者引用校验：路径必须在 resourceRoot 内、存在且为 .json。
// 渲染线程仍会独立导入网格并在失败时回退 billboard。
inline std::filesystem::path ValidateParticleMeshReference(
    const std::filesystem::path& resourceRoot,
    const std::string& reference,
    std::string_view fieldName)
{
    std::string portable = reference;
    for (char& character : portable)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }
    const std::filesystem::path parsed(portable);
    if (reference.empty() || reference.find('\0') != std::string::npos ||
        parsed.is_absolute() || parsed.has_root_name() ||
        parsed.has_root_directory())
    {
        throw std::invalid_argument(
            "Particle mesh field '" + std::string(fieldName) +
            "' must be resource-root-relative: " + reference);
    }
    for (const auto& part : parsed)
    {
        if (part == "..")
        {
            throw std::invalid_argument(
                "Particle mesh field '" + std::string(fieldName) +
                "' must not contain traversal: " + reference);
        }
    }

    const std::filesystem::path absolute =
        std::filesystem::weakly_canonical(resourceRoot / parsed);
    const std::filesystem::path relative =
        absolute.lexically_relative(
            std::filesystem::weakly_canonical(resourceRoot));
    if (relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..")
    {
        throw std::invalid_argument(
            "Particle mesh field '" + std::string(fieldName) +
            "' escapes resource root: " + reference);
    }
    if (!std::filesystem::is_regular_file(absolute))
    {
        throw std::invalid_argument(
            "Particle mesh field '" + std::string(fieldName) +
            "' references a missing file: " + reference);
    }
    if (LowerParticleMeshExtension(reference) != ".json")
    {
        throw std::invalid_argument(
            "Particle mesh field '" + std::string(fieldName) +
            "' must use .json: " + reference);
    }
    return absolute;
}

} // namespace VL
