#pragma once

#include "terrain/terrainCompositedProduction.h"
#include "terrain/terrainCompositedVTStore.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace VL::TerrainPackageClosure
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

inline void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

inline std::string Text(const Json& value)
{
    Require(value.is_string(), "Terrain VT string required");
    auto result = value.get<std::string>();
    Require(!result.empty() && result.size() <= 4096 && result.find('\0') == std::string::npos,
        "Invalid terrain VT string");
    return result;
}

inline fs::path Relative(const std::string& name)
{
    auto normalized = name;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const fs::path path(normalized);
    Require(!name.empty() && name.find('\0') == std::string::npos && name.find(':') == std::string::npos &&
        !path.is_absolute() && !path.has_root_name() && !path.has_root_directory(),
        "Terrain VT path must be resource-root-relative");
    for (const auto& part : path)
        Require(part != ".." && part != ".", "Unsafe terrain VT path component");
    Require(!path.filename().empty(), "Terrain VT path requires a filename");
    return path;
}

inline fs::path Resolve(const fs::path& root, const std::string& name)
{
    const auto relative = Relative(name);
    auto path = fs::canonical(root);
    for (const auto& component : relative)
    {
        path /= component;
        Require(!fs::is_symlink(fs::symlink_status(path)), "Terrain VT dependency must not be a symlink");
    }
    Require(fs::is_regular_file(path), "Missing terrain VT dependency");
    const auto logical = fs::canonical(path).lexically_relative(fs::canonical(root));
    Require(!logical.empty() && *logical.begin() != "..", "Terrain VT dependency escaped resource root");
    return path;
}

inline std::vector<fs::path> ProductionFiles(
    const fs::path& inputRoot, const TerrainCompositedProductionClosure& production)
{
    production.Validate();
    std::vector<fs::path> result;
    result.reserve(production.objects.size());
    for (const auto& object : production.objects)
        result.push_back(Resolve(inputRoot, object.relativePath));
    return result;
}

struct Package
{
    TerrainCompositedVTStore store;
    std::vector<fs::path> files;
    std::set<std::string> materialInstances;
};

inline Package Open(const fs::path& root, const std::string& manifestName)
{
    static_assert(TerrainCompositedVTStore::Version == 2);
    const auto manifest = Resolve(root, manifestName);
    auto store = TerrainCompositedVTStore::Open(manifest);
    store.ValidateAll();
    auto files = ProductionFiles(manifest.parent_path(), store.Production());
    files.push_back(manifest);
    files.push_back(Resolve(manifest.parent_path(), store.PackagePath().filename().generic_string()));
    std::set<std::string> materials;
    for (const auto& surface : store.Production().surfaces)
    {
        for (const auto* source : {&surface.runtimeMaterialInstancePath,
                 &surface.runtimeDetailMaterialInstancePath})
        {
            if (source->empty()) continue;
            const auto path = Resolve(manifest.parent_path(), Relative(*source).generic_string());
            const auto name = path.lexically_relative(fs::canonical(root)).generic_string();
            materials.insert(name);
            files.push_back(path);
        }
    }
    return {std::move(store), std::move(files), std::move(materials)};
}
}
