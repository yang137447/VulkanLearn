#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace VL
{
inline std::filesystem::path TerrainCompositedShaderRelativePath(
    const std::string& identity, bool source)
{
    const std::filesystem::path path(identity);
    if (identity.empty() || identity.find(':') != std::string::npos ||
        identity.find('\\') != std::string::npos || identity.find('\0') != std::string::npos ||
        path.is_absolute() || path.has_root_path())
        throw std::invalid_argument("Invalid production shader manifest identity");
    for (const auto& component : path)
        if (component == "." || component == "..")
            throw std::invalid_argument("Production shader identity contains traversal");
    if (source)
    {
        if (*path.begin() == "glsl" || *path.begin() == "spv")
            throw std::invalid_argument("Production source identity must be GLSL-root relative");
        return std::filesystem::path("glsl") / path;
    }
    if (*path.begin() != "spv")
        throw std::invalid_argument("Production artifact identity must be shader-root relative SPV");
    return path;
}
}
