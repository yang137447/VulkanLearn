#include "editor/service/materialInstanceDocumentService.h"

#include "editor/service/material/materialInstanceDocumentInternal.h"
#include "editor/service/material/materialInstanceDocumentReferences.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace VL::Editor
{
namespace
{

using PersistenceNumericType =
    Persistence::MaterialInstanceNumericType;
using PersistenceNumericValue =
    Persistence::MaterialInstanceNumericValue;

using MaterialDocumentDetail::MakeReferenceSnapshot;
using MaterialDocumentDetail::SameReference;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;

} // namespace

const char* ToString(MaterialEditorDocumentState state) noexcept
{
    switch (state)
    {
    case MaterialEditorDocumentState::Clean:
        return "Clean";
    case MaterialEditorDocumentState::Dirty:
        return "Dirty";
    case MaterialEditorDocumentState::SaveFailed:
        return "SaveFailed";
    case MaterialEditorDocumentState::SourceChanged:
        return "SourceChanged";
    }
    return "Unknown";
}

const char* ToString(MaterialEditorValidationState state) noexcept
{
    switch (state)
    {
    case MaterialEditorValidationState::Unknown:
        return "Unknown";
    case MaterialEditorValidationState::Valid:
        return "Valid";
    case MaterialEditorValidationState::Invalid:
        return "Invalid";
    }
    return "Unknown";
}

MaterialInstanceDocumentService::MaterialInstanceDocumentService()
    : MaterialInstanceDocumentService(DiscoverDefaultConfig())
{
}

MaterialInstanceDocumentService::MaterialInstanceDocumentService(
    std::filesystem::path resourceRoot,
    std::filesystem::path projectRoot)
    : MaterialInstanceDocumentService(
          Config{std::move(resourceRoot), std::move(projectRoot), true})
{
}

MaterialInstanceDocumentService::MaterialInstanceDocumentService(
    Config configValue)
    : config(std::move(configValue))
{
    if (config.resourceRoot.empty())
    {
        config.resourceRoot = std::filesystem::current_path();
    }
    if (config.projectRoot.empty())
    {
        config.projectRoot = config.resourceRoot;
    }

    std::error_code resourceError;
    config.resourceRoot = std::filesystem::absolute(
        config.resourceRoot, resourceError)
                              .lexically_normal();
    if (resourceError)
    {
        throw std::invalid_argument(
            "Failed to normalize Material Editor resource root: " +
            resourceError.message());
    }

    std::error_code projectError;
    config.projectRoot = std::filesystem::absolute(
        config.projectRoot, projectError)
                             .lexically_normal();
    if (projectError)
    {
        throw std::invalid_argument(
            "Failed to normalize Material Editor project root: " +
            projectError.message());
    }
}

EditorMaterialParameterType
MaterialInstanceDocumentService::ToEditorParameterType(
    PersistenceNumericType type) noexcept
{
    switch (type)
    {
    case PersistenceNumericType::Float:
        return EditorMaterialParameterType::Float;
    case PersistenceNumericType::Vec2:
        return EditorMaterialParameterType::Vec2;
    case PersistenceNumericType::Vec3:
        return EditorMaterialParameterType::Vec3;
    case PersistenceNumericType::Vec4:
        return EditorMaterialParameterType::Vec4;
    case PersistenceNumericType::Color:
        return EditorMaterialParameterType::Color;
    }
    return EditorMaterialParameterType::Float;
}

PersistenceNumericType
MaterialInstanceDocumentService::ToPersistenceParameterType(
    EditorMaterialParameterType type) noexcept
{
    switch (type)
    {
    case EditorMaterialParameterType::Float:
        return PersistenceNumericType::Float;
    case EditorMaterialParameterType::Vec2:
        return PersistenceNumericType::Vec2;
    case EditorMaterialParameterType::Vec3:
        return PersistenceNumericType::Vec3;
    case EditorMaterialParameterType::Vec4:
        return PersistenceNumericType::Vec4;
    case EditorMaterialParameterType::Color:
        return PersistenceNumericType::Color;
    }
    return PersistenceNumericType::Float;
}

EditorMaterialParameterValue
MaterialInstanceDocumentService::ToEditorValue(
    const PersistenceNumericValue& value)
{
    return std::visit(
        [](const auto& typedValue) -> EditorMaterialParameterValue
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, Persistence::MaterialInstanceColor>)
            {
                return EditorColor{typedValue.components};
            }
            else
            {
                return typedValue;
            }
        },
        value);
}

PersistenceNumericValue
MaterialInstanceDocumentService::ToPersistenceValue(
    const EditorMaterialParameterValue& value)
{
    return std::visit(
        [](const auto& typedValue) -> PersistenceNumericValue
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, EditorColor>)
            {
                return Persistence::MaterialInstanceColor{typedValue.components};
            }
            else
            {
                return typedValue;
            }
        },
        value);
}

void MaterialInstanceDocumentService::UpdateDirtyState(
    Document& document)
{
    if (document.state == MaterialEditorDocumentState::SourceChanged ||
        document.state == MaterialEditorDocumentState::SaveFailed)
    {
        return;
    }
    document.state =
        document.workingOverrides.parameters ==
                document.baselineOverrides.parameters &&
            document.workingOverrides.textures ==
                document.baselineOverrides.textures &&
            document.workingOverrides.renderStates ==
                document.baselineOverrides.renderStates
        ? MaterialEditorDocumentState::Clean
        : MaterialEditorDocumentState::Dirty;
}

void MaterialInstanceDocumentService::IncrementRevision(
    Document& document)
{
    if (document.revision == std::numeric_limits<EditorDocumentRevision>::max())
    {
        document.revision = 1;
    }
    else
    {
        ++document.revision;
    }
}

MaterialInstanceDocumentService::Document*
MaterialInstanceDocumentService::FindDocument(std::string_view assetPath)
{
    const auto iterator = documents.find(std::string(assetPath));
    return iterator == documents.end() ? nullptr : &iterator->second;
}

const MaterialInstanceDocumentService::Document*
MaterialInstanceDocumentService::FindDocument(
    std::string_view assetPath) const
{
    const auto iterator = documents.find(std::string(assetPath));
    return iterator == documents.end() ? nullptr : &iterator->second;
}

std::string MaterialInstanceDocumentService::RequireNormalizedPath(
    std::string_view assetPath) const
{
    try
    {
        return NormalizeMaterialInstancePath(assetPath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidAssetType,
            exception.what());
    }
}

MaterialInstanceDocumentService::Document&
MaterialInstanceDocumentService::RequireDocument(
    std::string_view assetPath)
{
    const std::string normalized = RequireNormalizedPath(assetPath);
    Document* document = FindDocument(normalized);
    if (document == nullptr)
    {
        ThrowServiceError(
            EditorErrorCode::DocumentNotOpen,
            "material instance document is not open: " + normalized);
    }
    return *document;
}

const MaterialInstanceDocumentService::Document&
MaterialInstanceDocumentService::RequireDocument(
    std::string_view assetPath) const
{
    const std::string normalized = RequireNormalizedPath(assetPath);
    const Document* document = FindDocument(normalized);
    if (document == nullptr)
    {
        ThrowServiceError(
            EditorErrorCode::DocumentNotOpen,
            "material instance document is not open: " + normalized);
    }
    return *document;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeFailure(
    EditorErrorCode errorCode,
    std::string message,
    const Document* document) const
{
    MaterialEditorServiceResult result;
    result.succeeded = false;
    result.errorCode = errorCode;
    result.message = std::move(message);
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(
            const_cast<Document&>(*document));
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeSuccess(
    std::string message,
    Document* document)
{
    MaterialEditorServiceResult result;
    result.succeeded = true;
    result.errorCode = EditorErrorCode::None;
    result.message = std::move(message);
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(*document);
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeSnapshotResult(
    MaterialEditorServiceResult result,
    Document* document)
{
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(*document);
    }
    result.editor = BuildSnapshot();
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeDocumentResult(
    MaterialEditorServiceResult result,
    Document* document)
{
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(*document);
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::OpenMaterialInstanceAsset(
    std::string_view assetPath,
    const std::optional<EditorNavigationOrigin>& origin)
{
    try
    {
        const std::string normalized =
            NormalizeMaterialInstancePath(assetPath);
        auto existing = documents.find(normalized);
        if (existing != documents.end())
        {
            selectedDocumentPath = normalized;
            if (origin.has_value())
            {
                const MaterialEditorReferenceSnapshot reference =
                    MakeReferenceSnapshot(origin.value(), normalized);
                const auto duplicate = std::find_if(
                    existing->second.references.begin(),
                    existing->second.references.end(),
                    [&reference](const MaterialEditorReferenceSnapshot& item)
                    {
                        return SameReference(item, reference);
                    });
                if (duplicate == existing->second.references.end())
                {
                    existing->second.references.push_back(reference);
                }
            }
            return MakeSnapshotResult(
                MakeSuccess("material instance document focused", &existing->second),
                &existing->second);
        }

        const LoadedDocument loaded = LoadDocumentFromDisk(normalized);
        Document document;
        document.assetPath = loaded.assetPath;
        document.absolutePath = loaded.absolutePath;
        document.baseMaterialPath = loaded.baseMaterialPath;
        document.baseMaterialAbsolutePath = loaded.baseMaterialAbsolutePath;
        document.sourceJson = loaded.sourceJson;
        document.materialJson = loaded.materialJson;
        document.defaults = loaded.defaults;
        document.baselineOverrides = loaded.overrides;
        document.workingOverrides = loaded.overrides;
        document.sourceDigest = loaded.sourceDigest;
        document.schemaDigest = loaded.schemaDigest;
        if (origin.has_value())
        {
            document.references.push_back(
                MakeReferenceSnapshot(origin.value(), normalized));
        }
        const auto inserted = documents.emplace(normalized, std::move(document));
        selectedDocumentPath = normalized;
        return MakeSnapshotResult(
            MakeSuccess("material instance document opened", &inserted.first->second),
            &inserted.first->second);
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

MaterialEditorServiceResult
MaterialInstanceDocumentService::SelectMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        const std::string normalized = NormalizeMaterialInstancePath(assetPath);
        Document& document = RequireDocument(normalized);
        selectedDocumentPath = normalized;
        return MakeSnapshotResult(
            MakeSuccess("material instance document selected", &document),
            &document);
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

MaterialEditorServiceResult
MaterialInstanceDocumentService::GetMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        return MakeSnapshotResult(
            MakeSuccess("material instance document read", &document),
            &document);
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

MaterialEditorServiceResult
MaterialInstanceDocumentService::GetSelectedMaterialInstanceDocument()
{
    if (selectedDocumentPath.empty())
    {
        return MakeFailure(
            EditorErrorCode::DocumentNotOpen,
            "no material instance document is selected");
    }
    return GetMaterialInstanceDocument(selectedDocumentPath);
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::CloseMaterialInstanceAsset(
    std::string_view assetPath,
    EditorDirtyDocumentPolicy dirtyPolicy)
{
    try
    {
        const std::string normalized = NormalizeMaterialInstancePath(assetPath);
        Document& document = RequireDocument(normalized);
        const bool dirty = document.state != MaterialEditorDocumentState::Clean;
        if (dirty && dirtyPolicy == EditorDirtyDocumentPolicy::RequireClean)
        {
            return MakeFailure(
                EditorErrorCode::DocumentDirty,
                "material instance document has unsaved changes: " + normalized,
                &document);
        }
        documents.erase(normalized);
        if (selectedDocumentPath == normalized)
        {
            selectedDocumentPath = documents.empty()
                ? std::string()
                : documents.begin()->first;
        }
        MaterialEditorServiceResult result = MakeSuccess(
            "material instance document closed");
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

bool MaterialInstanceDocumentService::IsDocumentOpen(
    std::string_view assetPath) const
{
    try
    {
        return FindDocument(NormalizeMaterialInstancePath(assetPath)) != nullptr;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::optional<EditorDocumentRevision>
MaterialInstanceDocumentService::GetDocumentRevision(
    std::string_view assetPath) const
{
    try
    {
        const Document* document = FindDocument(
            NormalizeMaterialInstancePath(assetPath));
        if (document == nullptr)
        {
            return std::nullopt;
        }
        return document->revision;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace VL::Editor
