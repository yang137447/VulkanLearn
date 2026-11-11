#include "editor/service/material/materialInstanceDocumentPaths.h"

#include "editor/service/materialInstanceDocumentService.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>
#include <utility>

namespace VL::Editor::MaterialDocumentDetail
{

std::string NormalizeSeparators(std::string_view path)
{
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool IsRegularFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool ContainsParentComponent(const std::filesystem::path& path)
{
    for (const auto& component : path)
    {
        if (component == "..")
        {
            return true;
        }
    }
    return false;
}

bool HasAssetFileName(
    std::string_view path,
    std::string_view prefix)
{
    const std::filesystem::path filePath(
        NormalizeSeparators(path));
    const std::string fileName = filePath.filename().generic_string();
    return fileName.size() > prefix.size() + 5 &&
           fileName.rfind(prefix, 0) == 0 &&
           filePath.extension().generic_string() == ".json";
}

} // namespace VL::Editor::MaterialDocumentDetail

namespace VL::Editor
{

using MaterialDocumentDetail::ContainsParentComponent;
using MaterialDocumentDetail::HasAssetFileName;
using MaterialDocumentDetail::IsRegularFile;
using MaterialDocumentDetail::Json;
using MaterialDocumentDetail::NormalizeSeparators;
using MaterialDocumentDetail::ThrowServiceError;

MaterialInstanceDocumentService::Config
MaterialInstanceDocumentService::DiscoverDefaultConfig()
{
    Config result;
    std::filesystem::path searchRoot =
        std::filesystem::current_path();
    for (int depth = 0; depth < 8 && !searchRoot.empty(); ++depth)
    {
        const std::filesystem::path configPath =
            searchRoot / "config" / "config.json";
        if (IsRegularFile(configPath))
        {
            result.projectRoot = searchRoot;
            try
            {
                std::ifstream input(configPath);
                Json configJson;
                input >> configJson;
                if (configJson.contains("resourcePath") &&
                    configJson.at("resourcePath").is_string())
                {
                    result.resourceRoot =
                        configJson.at("resourcePath").get<std::string>();
                    if (result.resourceRoot.is_relative())
                    {
                        result.resourceRoot =
                            searchRoot / result.resourceRoot;
                    }
                }
            }
            catch (const std::exception&)
            {
                // 默认构造只负责发现根目录；具体 JSON 错误由 Open 诊断。
            }
            break;
        }
        searchRoot = searchRoot.parent_path();
    }
    if (result.projectRoot.empty())
    {
        result.projectRoot = std::filesystem::current_path();
    }
    if (result.resourceRoot.empty())
    {
        result.resourceRoot = result.projectRoot;
    }
    return result;
}
bool MaterialInstanceDocumentService::IsParentPath(
    const std::filesystem::path& path)
{
    return ContainsParentComponent(path);
}

std::string MaterialInstanceDocumentService::NormalizeRelativePath(
    std::string_view path)
{
    if (path.empty())
    {
        throw std::invalid_argument("asset path must not be empty");
    }
    if (path.find('\0') != std::string_view::npos)
    {
        throw std::invalid_argument("asset path contains a NUL byte");
    }

    const std::filesystem::path input(
        NormalizeSeparators(path));
    if (input.has_root_name() || input.has_root_directory() ||
        input.is_absolute())
    {
        throw std::invalid_argument(
            "asset path must be relative to the resource root");
    }
    const std::filesystem::path normalized = input.lexically_normal();
    if (normalized.empty() || normalized == "." ||
        IsParentPath(normalized))
    {
        throw std::invalid_argument(
            "asset path must remain inside the resource root");
    }
    return normalized.generic_string();
}

std::string MaterialInstanceDocumentService::NormalizeMaterialInstancePath(
    std::string_view path)
{
    const std::string normalized = NormalizeRelativePath(path);
    if (!HasAssetFileName(normalized, "MI_"))
    {
        throw std::invalid_argument(
            "material instance path must name an MI_*.json asset");
    }
    return normalized;
}

std::string MaterialInstanceDocumentService::NormalizeTextureAssetPath(
    std::string_view path)
{
    const std::string normalized = NormalizeRelativePath(path);
    if (!HasAssetFileName(normalized, "T_"))
    {
        throw std::invalid_argument(
            "texture binding must reference a T_*.json asset");
    }
    return normalized;
}
std::filesystem::path
MaterialInstanceDocumentService::ResolveResourceFile(
    std::string_view resourceRelativePath) const
{
    const std::string normalized =
        NormalizeRelativePath(resourceRelativePath);
    const std::filesystem::path path =
        (config.resourceRoot / normalized).lexically_normal();
    if (!IsRegularFile(path))
    {
        ThrowServiceError(
            EditorErrorCode::AssetNotFound,
            "resource file does not exist: " + normalized);
    }
    return path;
}

std::filesystem::path
MaterialInstanceDocumentService::ResolveMaterialDefinitionPath(
    std::string_view materialPath) const
{
    std::string normalized;
    try
    {
        normalized = NormalizeRelativePath(materialPath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidAssetType,
            "invalid material definition path: " +
                std::string(exception.what()));
    }

    if (!HasAssetFileName(normalized, "M_"))
    {
        ThrowServiceError(
            EditorErrorCode::InvalidAssetType,
            "material definition path must name an M_*.json asset: " +
                normalized);
    }

    const std::filesystem::path relativePath(normalized);
    const std::filesystem::path fileName = relativePath.filename();
    const std::array<std::filesystem::path, 4> candidates = {
        config.projectRoot / relativePath,
        config.resourceRoot / relativePath,
        config.projectRoot / "shader" / "glsl" / fileName,
        config.resourceRoot / fileName};
    for (const std::filesystem::path& candidate : candidates)
    {
        if (IsRegularFile(candidate))
        {
            return candidate.lexically_normal();
        }
    }
    ThrowServiceError(
        EditorErrorCode::AssetNotFound,
        "material definition does not exist: " + normalized);
}

} // namespace VL::Editor
