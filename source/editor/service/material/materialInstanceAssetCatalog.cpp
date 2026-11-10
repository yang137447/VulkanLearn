#include "editor/service/material/materialInstanceAssetCatalog.h"

#include "editor/service/material/materialInstanceDocumentPaths.h"
#include "editor/service/materialInstanceDocumentService.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VL::Editor
{
namespace
{

using MaterialDocumentDetail::HasAssetFileName;
using MaterialDocumentDetail::IsRegularFile;
using MaterialDocumentDetail::ReadStringField;
using MaterialDocumentDetail::ServiceError;

} // namespace

std::vector<MaterialEditorAssetEntry>
MaterialInstanceDocumentService::ListOpenDocumentEntries() const
{
    std::vector<MaterialEditorAssetEntry> result;
    result.reserve(documents.size());
    for (const auto& [path, document] : documents)
    {
        MaterialEditorAssetEntry entry;
        entry.assetPath = path;
        entry.revision = document.revision;
        entry.state = document.state;
        entry.message = document.validationMessage;
        entry.dirty =
            document.state == MaterialEditorDocumentState::Dirty ||
            document.state == MaterialEditorDocumentState::SaveFailed ||
            document.state == MaterialEditorDocumentState::SourceChanged;
        result.push_back(std::move(entry));
    }
    return result;
}

MaterialEditorTextureAssetSnapshot
MaterialInstanceDocumentService::InspectTextureAsset(
    const std::filesystem::path& absolutePath,
    std::string logicalPath) const
{
    MaterialEditorTextureAssetSnapshot result;
    result.assetPath = std::move(logicalPath);
    try
    {
        const Json textureJson = ReadJsonFile(absolutePath);
        if (!textureJson.is_object() ||
            textureJson.value("type", std::string()) != "texture")
        {
            throw std::runtime_error("asset type is not texture");
        }
        const std::string source =
            ReadStringField(textureJson, "source", result.assetPath);
        const std::string normalizedSource = NormalizeRelativePath(source);
        if (config.validateTextureSources)
        {
            static_cast<void>(ResolveResourceFile(normalizedSource));
        }
        result.valid = true;
    }
    catch (const std::exception& exception)
    {
        result.valid = false;
        result.diagnostic = exception.what();
    }
    return result;
}

std::vector<MaterialEditorTextureAssetSnapshot>
MaterialInstanceDocumentService::ListTextureAssets() const
{
    if (textureAssetCatalogCached)
    {
        return textureAssetCatalog;
    }

    std::vector<MaterialEditorTextureAssetSnapshot> result;
    if (!std::filesystem::is_directory(config.resourceRoot))
    {
        textureAssetCatalogCached = true;
        textureAssetCatalog.clear();
        return textureAssetCatalog;
    }
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        config.resourceRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(iteratorError))
    {
        if (iteratorError)
        {
            iteratorError.clear();
            continue;
        }
        const std::filesystem::path path = iterator->path();
        if (!IsRegularFile(path) || !HasAssetFileName(
                path.filename().generic_string(), "T_"))
        {
            continue;
        }
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(
            path,
            config.resourceRoot,
            relativeError);
        if (relativeError)
        {
            continue;
        }
        result.push_back(InspectTextureAsset(
            path,
            relative.generic_string()));
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const MaterialEditorTextureAssetSnapshot& left,
           const MaterialEditorTextureAssetSnapshot& right)
        {
            return left.assetPath < right.assetPath;
        });
    textureAssetCatalog = std::move(result);
    textureAssetCatalogCached = true;
    return textureAssetCatalog;
}

MaterialEditorSnapshot MaterialInstanceDocumentService::BuildSnapshot()
{
    MaterialEditorSnapshot result;
    result.selectedDocumentPath = selectedDocumentPath;
    result.documentTabs = ListOpenDocumentEntries();
    result.textureAssets = ListTextureAssets();

    const MaterialEditorServiceResult assets =
        ListMaterialInstanceAssets({}, 0, std::numeric_limits<uint32_t>::max());
    if (assets.editor.has_value())
    {
        result.assets = assets.editor->assets;
    }
    if (!selectedDocumentPath.empty())
    {
        const auto selected = documents.find(selectedDocumentPath);
        if (selected != documents.end())
        {
            result.activeDocument = MakeDocumentSnapshot(
                const_cast<Document&>(selected->second));
            if (result.activeDocument->state !=
                MaterialEditorDocumentState::Clean)
            {
                result.statusMessage = ToString(
                    result.activeDocument->state);
            }
        }
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ListMaterialInstanceAssets(
    std::string_view searchText,
    uint32_t pageIndex,
    uint32_t pageSize) const
{
    MaterialEditorServiceResult result;
    result.succeeded = true;
    result.errorCode = EditorErrorCode::None;
    result.message = "material instance assets listed";

    MaterialEditorSnapshot snapshot;
    snapshot.selectedDocumentPath = selectedDocumentPath;
    snapshot.documentTabs = ListOpenDocumentEntries();
    snapshot.textureAssets = ListTextureAssets();

    if (!materialInstanceAssetCatalogCached &&
        std::filesystem::is_directory(config.resourceRoot))
    {
        std::vector<MaterialEditorAssetEntry> discoveredAssets;
        std::error_code iteratorError;
        std::filesystem::recursive_directory_iterator iterator(
            config.resourceRoot,
            std::filesystem::directory_options::skip_permission_denied,
            iteratorError);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(iteratorError))
        {
            if (iteratorError)
            {
                iteratorError.clear();
                continue;
            }
            const std::filesystem::path path = iterator->path();
            if (!IsRegularFile(path) || !HasAssetFileName(
                    path.filename().generic_string(), "MI_"))
            {
                continue;
            }
            std::error_code relativeError;
            const std::filesystem::path relative = std::filesystem::relative(
                path,
                config.resourceRoot,
                relativeError);
            if (relativeError)
            {
                continue;
            }
            const std::string logicalPath = relative.generic_string();
            MaterialEditorAssetEntry entry;
            entry.assetPath = logicalPath;
            discoveredAssets.push_back(std::move(entry));
        }
        std::sort(
            discoveredAssets.begin(),
            discoveredAssets.end(),
            [](const MaterialEditorAssetEntry& left,
               const MaterialEditorAssetEntry& right)
            {
                return left.assetPath < right.assetPath;
            });
        materialInstanceAssetCatalog = std::move(discoveredAssets);
        materialInstanceAssetCatalogCached = true;
    }

    std::vector<MaterialEditorAssetEntry> allAssets;
    allAssets.reserve(materialInstanceAssetCatalog.size());
    for (const MaterialEditorAssetEntry& cachedAsset : materialInstanceAssetCatalog)
    {
        if (!searchText.empty() &&
            cachedAsset.assetPath.find(searchText) == std::string::npos)
        {
            continue;
        }
        MaterialEditorAssetEntry entry = cachedAsset;
        const auto openIt = documents.find(entry.assetPath);
        entry.dirty = openIt != documents.end() &&
            openIt->second.state != MaterialEditorDocumentState::Clean;
        allAssets.push_back(std::move(entry));
    }

    const std::size_t begin = std::min<std::size_t>(
        static_cast<std::size_t>(pageIndex) * pageSize,
        allAssets.size());
    const std::size_t end = std::min<std::size_t>(
        begin + pageSize,
        allAssets.size());
    snapshot.assets.assign(
        allAssets.begin() + static_cast<std::ptrdiff_t>(begin),
        allAssets.begin() + static_cast<std::ptrdiff_t>(end));
    if (!selectedDocumentPath.empty())
    {
        const auto selected = documents.find(selectedDocumentPath);
        if (selected != documents.end())
        {
            snapshot.activeDocument = MakeDocumentSnapshot(
                const_cast<Document&>(selected->second));
        }
    }
    result.editor = std::move(snapshot);
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::GetMaterialInstanceReferenceContext(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        MaterialEditorServiceResult result = MakeSuccess(
            "material instance reference context read", &document);
        result.references = document.references;
        result.editor = BuildSnapshot();
        return result;
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

} // namespace VL::Editor
