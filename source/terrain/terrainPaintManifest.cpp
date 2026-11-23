#include "terrain/terrainPaintManifest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "shader/build/atomicFile.h"
#include "terrain/terrainLayerRegistry.h"

namespace VL
{
namespace
{

constexpr uint32_t ManifestVersion = 2;
constexpr const char* ManifestKind = "terrainLayerWeights";

std::string ManifestError(
    const std::filesystem::path& path,
    const std::string& message)
{
    return "Terrain paint manifest " + path.string() + ": " + message;
}

const nlohmann::json& RequireField(
    const nlohmann::json& document,
    const char* name,
    const std::filesystem::path& path)
{
    const auto found = document.find(name);
    if (found == document.end())
        throw std::runtime_error(
            ManifestError(path, std::string("missing field '") + name + "'"));
    return *found;
}

uint32_t ReadUint32(
    const nlohmann::json& document,
    const char* name,
    const std::filesystem::path& path)
{
    const nlohmann::json& value = RequireField(document, name, path);
    if (value.is_number_unsigned())
    {
        const uint64_t unsignedValue = value.get<uint64_t>();
        if (unsignedValue <= static_cast<uint64_t>(UINT32_MAX))
            return static_cast<uint32_t>(unsignedValue);
    }
    if (value.is_number_integer())
    {
        const int64_t signedValue = value.get<int64_t>();
        if (signedValue >= 0 &&
            signedValue <= static_cast<int64_t>(UINT32_MAX))
            return static_cast<uint32_t>(signedValue);
    }
    throw std::runtime_error(ManifestError(
        path, std::string("field '") + name + "' must be a uint32"));
}

float ReadFloat(
    const nlohmann::json& document,
    const char* name,
    const std::filesystem::path& path)
{
    const nlohmann::json& value = RequireField(document, name, path);
    if (!value.is_number())
        throw std::runtime_error(ManifestError(
            path, std::string("field '") + name + "' must be a number"));
    return value.get<float>();
}

std::string ReadString(
    const nlohmann::json& document,
    const char* name,
    const std::filesystem::path& path)
{
    const nlohmann::json& value = RequireField(document, name, path);
    if (!value.is_string())
        throw std::runtime_error(ManifestError(
            path, std::string("field '") + name + "' must be a string"));
    return value.get<std::string>();
}

std::vector<std::string> ReadLayerKeys(
    const nlohmann::json& document,
    const std::filesystem::path& path)
{
    const nlohmann::json& value = RequireField(document, "layerKeys", path);
    if (!value.is_array())
        throw std::runtime_error(
            ManifestError(path, "field 'layerKeys' must be an array of strings"));
    std::vector<std::string> keys;
    keys.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index)
    {
        const nlohmann::json& key = value[index];
        if (!key.is_string())
            throw std::runtime_error(ManifestError(
                path,
                "field 'layerKeys'[" + std::to_string(index) +
                    "] must be a string"));
        keys.push_back(key.get<std::string>());
    }
    return keys;
}

std::string DescribeMissingOrValue(
    const std::vector<std::string>& keys,
    size_t index)
{
    if (index >= keys.size())
        return "<missing>";
    return keys[index];
}

bool IsValidContentSha256(const std::string& value)
{
    if (value.empty())
        return true;
    if (value.size() != 64)
        return false;
    for (char character : value)
    {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    return true;
}

} // namespace

std::filesystem::path TerrainPaintManifestPath(
    const std::filesystem::path& paintPath)
{
    std::filesystem::path path = paintPath;
    path += ".meta.json";
    return path;
}

std::vector<std::string> TerrainPaintLayerKeys(const TerrainAssetDesc& desc)
{
    std::string assetPath = desc.assetPath;
    std::replace(assetPath.begin(), assetPath.end(), '\\', '/');
    std::vector<std::string> keys;
    keys.reserve(desc.layers.size());
    for (const TerrainLayerDesc& layer : desc.layers)
        keys.push_back(TerrainLayerRegistry::ResolveLayerKey(layer, assetPath));
    return keys;
}

bool TerrainPaintManifestExists(
    const std::filesystem::path& paintPath,
    std::string& error)
{
    error.clear();
    const std::filesystem::path manifestPath =
        TerrainPaintManifestPath(paintPath);
#ifdef _WIN32
    // GetFileAttributesW 能区分“确实不存在”和权限/非法路径；MinGW 的
    // std::filesystem::exists 会把后者也折叠成 false 且不报错。
    const DWORD attributes = GetFileAttributesW(manifestPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return true;
    const DWORD lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND ||
        lastError == ERROR_PATH_NOT_FOUND)
    {
        return false;
    }
    error = ManifestError(
        manifestPath,
        "cannot query existence: " +
            std::error_code(
                static_cast<int>(lastError),
                std::system_category()).message() +
            " (win32=" + std::to_string(lastError) + ")");
    return false;
#else
    std::error_code queryError;
    const std::filesystem::file_status status =
        std::filesystem::status(manifestPath, queryError);
    if (queryError)
    {
        // ENOENT 是“确实没有 sidecar”，不是查询失败。
        if (queryError == std::errc::no_such_file_or_directory)
            return false;
        error = ManifestError(
            manifestPath,
            "cannot query existence: " + queryError.message());
        return false;
    }
    return status.type() != std::filesystem::file_type::not_found;
#endif
}

bool LoadTerrainPaintManifest(
    const std::filesystem::path& paintPath,
    TerrainPaintManifest& out,
    std::string& error)
{
    error.clear();
    const std::filesystem::path manifestPath =
        TerrainPaintManifestPath(paintPath);
    try
    {
        const std::vector<uint8_t> bytes = ReadBinaryFile(manifestPath);
        const std::string text(bytes.begin(), bytes.end());
        const nlohmann::json document = nlohmann::json::parse(text);
        if (!document.is_object())
            throw std::runtime_error(
                ManifestError(manifestPath, "root must be a JSON object"));

        const uint32_t version = ReadUint32(document, "version", manifestPath);
        if (version != ManifestVersion)
            throw std::runtime_error(ManifestError(
                manifestPath,
                "unsupported version " + std::to_string(version) +
                    " (expected " + std::to_string(ManifestVersion) + ")"));
        const std::string kind = ReadString(document, "kind", manifestPath);
        if (kind != ManifestKind)
            throw std::runtime_error(ManifestError(
                manifestPath,
                "unsupported kind '" + kind + "' (expected '" +
                    std::string(ManifestKind) + "')"));

        TerrainPaintManifest manifest;
        manifest.version = version;
        manifest.tileX = ReadUint32(document, "tileX", manifestPath);
        manifest.tileY = ReadUint32(document, "tileY", manifestPath);
        manifest.tileGrid = ReadUint32(document, "tileGrid", manifestPath);
        manifest.width = ReadUint32(document, "width", manifestPath);
        manifest.height = ReadUint32(document, "height", manifestPath);
        manifest.layerCount =
            ReadUint32(document, "layerCount", manifestPath);
        manifest.layerKeys = ReadLayerKeys(document, manifestPath);
        manifest.featherTexels =
            ReadFloat(document, "featherTexels", manifestPath);
        manifest.contentSha256 =
            ReadString(document, "contentSha256", manifestPath);

        out = std::move(manifest);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ValidateTerrainPaintManifest(
    const TerrainPaintManifest& manifest,
    const std::vector<std::string>& expectedLayerKeys,
    uint32_t width,
    uint32_t height,
    std::string& error)
{
    error.clear();
    const size_t expectedCount = expectedLayerKeys.size();
    if (static_cast<size_t>(manifest.layerCount) != expectedCount)
    {
        error =
            "layerCount " + std::to_string(manifest.layerCount) +
            " does not match the terrain layer table (" +
            std::to_string(expectedCount) + ")";
        return false;
    }
    if (manifest.width != width)
    {
        error =
            "width " + std::to_string(manifest.width) +
            " does not match the paint file (" + std::to_string(width) + ")";
        return false;
    }
    if (manifest.height != height)
    {
        error =
            "height " + std::to_string(manifest.height) +
            " does not match the paint file (" + std::to_string(height) + ")";
        return false;
    }

    const size_t mismatchCount =
        manifest.layerKeys.size() > expectedCount
            ? manifest.layerKeys.size()
            : expectedCount;
    for (size_t index = 0; index < mismatchCount; ++index)
    {
        const bool manifestHasKey = index < manifest.layerKeys.size();
        const bool expectedHasKey = index < expectedCount;
        const bool matches = manifestHasKey && expectedHasKey &&
            manifest.layerKeys[index] == expectedLayerKeys[index];
        if (matches)
            continue;
        error =
            "layerKeys mismatch at layer " + std::to_string(index) +
            " (expected '" + DescribeMissingOrValue(expectedLayerKeys, index) +
            "', got '" + DescribeMissingOrValue(manifest.layerKeys, index) +
            "')";
        return false;
    }
    return true;
}

bool SaveTerrainPaintManifest(
    const std::filesystem::path& paintPath,
    const TerrainPaintManifest& manifest,
    std::string& error)
{
    error.clear();
    const std::filesystem::path manifestPath =
        TerrainPaintManifestPath(paintPath);
    std::string text;
    std::string buildError;
    if (!BuildTerrainPaintManifestBytes(manifest, text, buildError))
    {
        error = ManifestError(manifestPath, buildError);
        return false;
    }
    try
    {
        WriteTextFileAtomically(manifestPath, text);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = ManifestError(
            manifestPath, std::string("write failed: ") + exception.what());
        return false;
    }
}

bool BuildTerrainPaintManifestBytes(
    const TerrainPaintManifest& manifest,
    std::string& out,
    std::string& error)
{
    out.clear();
    error.clear();
    if (manifest.version != ManifestVersion)
    {
        error =
            "version " + std::to_string(manifest.version) +
            " is not supported (expected " +
            std::to_string(ManifestVersion) + ")";
        return false;
    }
    if (manifest.width != manifest.height)
    {
        error = "width and height must match (non-square paint maps are not supported)";
        return false;
    }
    if (manifest.width < 2 || manifest.width > 4096)
    {
        error = "width and height must be in [2, 4096]";
        return false;
    }
    if (manifest.layerCount == 0)
    {
        error = "layerCount must be greater than zero";
        return false;
    }
    if (manifest.layerCount > 256)
    {
        error = "layerCount " + std::to_string(manifest.layerCount) +
            " exceeds the 256 layer limit";
        return false;
    }
    if (manifest.tileGrid == 0)
    {
        error = "tileGrid must be greater than zero";
        return false;
    }
    if (manifest.tileX >= manifest.tileGrid)
    {
        error =
            "tileX " + std::to_string(manifest.tileX) +
            " must be smaller than tileGrid " +
            std::to_string(manifest.tileGrid);
        return false;
    }
    if (manifest.tileY >= manifest.tileGrid)
    {
        error =
            "tileY " + std::to_string(manifest.tileY) +
            " must be smaller than tileGrid " +
            std::to_string(manifest.tileGrid);
        return false;
    }
    if (static_cast<size_t>(manifest.layerCount) !=
        manifest.layerKeys.size())
    {
        error =
            "layerCount " + std::to_string(manifest.layerCount) +
            " does not match layerKeys size (" +
            std::to_string(manifest.layerKeys.size()) + ")";
        return false;
    }
    if (!std::isfinite(manifest.featherTexels) ||
        manifest.featherTexels < 0.0f)
    {
        error = "featherTexels must be finite and non-negative";
        return false;
    }
    if (!IsValidContentSha256(manifest.contentSha256))
    {
        error =
            "contentSha256 must be empty or 64 hexadecimal characters";
        return false;
    }
    try
    {
        nlohmann::json document = nlohmann::json::object();
        document["version"] = manifest.version;
        document["kind"] = ManifestKind;
        document["tileX"] = manifest.tileX;
        document["tileY"] = manifest.tileY;
        document["tileGrid"] = manifest.tileGrid;
        document["width"] = manifest.width;
        document["height"] = manifest.height;
        document["layerCount"] = manifest.layerCount;
        document["layerKeys"] = manifest.layerKeys;
        document["featherTexels"] = manifest.featherTexels;
        document["contentSha256"] = manifest.contentSha256;
        // 与离线工具保持一致：2 空格缩进、UTF-8 原样输出、末尾换行。
        out = document.dump(2, ' ', false) + "\n";
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("serialize failed: ") + exception.what();
        return false;
    }
}

} // namespace VL
