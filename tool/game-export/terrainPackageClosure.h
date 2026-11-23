#pragma once

#include "terrain/terrainCompositedProduction.h"
#include "terrain/terrainCompositedVTStore.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace VL::TerrainPackageClosure
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

inline void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// VT 输入名带着两个 64 位摘要，叠加深层资源工作树后很容易越过 MAX_PATH。
// std::filesystem 的 canonical/status/copy 会因此把真实存在的文件判成缺失，
// 所以这里对所有 Win32 访问统一补 \\?\ 前缀；路径本身仍保持普通形式，
// 便于 lexically_relative、日志和 manifest 继续使用资源根相对字符串。
#ifdef _WIN32
inline std::wstring ExtendedPath(const fs::path& path)
{
    auto native = path.native();
    if (native.rfind(L"\\\\?\\", 0) == 0)
        return native;
    if (!path.is_absolute())
        return native;
    // \\?\ 只接受反斜杠；资源名来自 JSON 时可能带 '/'，先统一分隔符。
    std::replace(native.begin(), native.end(), L'/', L'\\');
    return L"\\\\?\\" + native;
}
#endif

// 只读属性探测，不跟随链接；超长路径也能生效。
inline bool IsLinkedEntry(const fs::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(ExtendedPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}

inline bool IsRegularFile(const fs::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(ExtendedPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    return fs::is_regular_file(path);
#endif
}

// 长路径上 fs::canonical 会失败；失败时退回绝对+词法规范化，
// 仅用于同一目标去重，不构成安全边界（边界由 Relative() 与逐级链接检查保证）。
inline fs::path CanonicalOrAbsolute(const fs::path& path)
{
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    if (!error)
        return canonical;
    error.clear();
    const auto absolute = fs::absolute(path, error);
    return error ? path : absolute.lexically_normal();
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
        Require(!IsLinkedEntry(path), "Terrain VT dependency must not be a symlink");
    }
    if (!IsRegularFile(path))
        throw std::runtime_error("Missing terrain VT dependency: " + path.generic_string());
    // Relative() 已排除 ..、绝对路径与 root name，root 又是 canonical，
    // 逐段拼接不会逃出根；不再对可能超长的路径做 canonical 复算。
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
