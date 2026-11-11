#include "editor/service/material/materialInstanceDocumentOverrides.h"

#include "editor/service/material/materialInstanceDocumentValidation.h"
#include "editor/service/materialInstanceDocumentService.h"

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

using MaterialDocumentDetail::RenderStateFieldName;
using MaterialDocumentDetail::RenderStateValueName;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;
using MaterialDocumentDetail::ValidateEditorParameterRange;

} // namespace

MaterialEditorServiceResult
MaterialInstanceDocumentService::SetMaterialParameterOverride(
    std::string_view assetPath,
    std::string_view parameter,
    EditorMaterialParameterType parameterType,
    const EditorMaterialParameterValue& value)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }

        const auto defaultIt = document.defaults.parameters.find(
            std::string(parameter));
        if (defaultIt == document.defaults.parameters.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownParameter,
                "unknown material parameter: " + std::string(parameter),
                &document);
        }

        const PersistenceNumericType expectedType =
            Persistence::GetMaterialInstanceNumericType(defaultIt->second);
        if (ToPersistenceParameterType(parameterType) != expectedType ||
            Persistence::GetMaterialInstanceNumericType(
                ToPersistenceValue(value)) != expectedType ||
            !IsFiniteEditorMaterialParameterValue(value))
        {
            return MakeFailure(
                EditorErrorCode::ParameterTypeMismatch,
                "material parameter type/value does not match schema: " +
                    std::string(parameter),
                &document);
        }

        const auto descriptorIt = document.materialJson.at("parameters").find(
            std::string(parameter));
        if (descriptorIt != document.materialJson.at("parameters").end())
        {
            try
            {
                ValidateEditorParameterRange(
                    *descriptorIt,
                    value,
                    parameter);
            }
            catch (const ServiceError& exception)
            {
                return MakeFailure(
                    exception.errorCode,
                    exception.what(),
                    &document);
            }
        }

        PersistenceOverrides proposed = document.workingOverrides;
        const PersistenceNumericValue persistenceValue =
            ToPersistenceValue(value);
        if (Persistence::MaterialInstanceNumericValuesExactlyEqual(
                persistenceValue,
                defaultIt->second))
        {
            proposed.parameters.erase(std::string(parameter));
        }
        else
        {
            proposed.parameters[std::string(parameter)] = persistenceValue;
        }

        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft parameter is valid";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeDocumentResult(
            MakeSuccess("material parameter override updated", &document),
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
MaterialInstanceDocumentService::ClearMaterialParameterOverride(
    std::string_view assetPath,
    std::string_view parameter)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        if (document.defaults.parameters.find(std::string(parameter)) ==
            document.defaults.parameters.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownParameter,
                "unknown material parameter: " + std::string(parameter),
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        proposed.parameters.erase(std::string(parameter));
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft parameter reset to material default";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeDocumentResult(
            MakeSuccess("material parameter override cleared", &document),
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
MaterialInstanceDocumentService::SetMaterialTextureOverride(
    std::string_view assetPath,
    std::string_view slot,
    std::string_view textureAssetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        const std::string slotName(slot);
        const auto defaultIt = document.defaults.textures.find(slotName);
        if (defaultIt == document.defaults.textures.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownTextureSlot,
                "unknown material texture slot: " + slotName,
                &document);
        }

        std::string normalizedTexturePath;
        try
        {
            normalizedTexturePath =
                NormalizeTextureAssetPath(textureAssetPath);
            ValidateTextureReference(
                normalizedTexturePath,
                document.assetPath);
        }
        catch (const ServiceError& exception)
        {
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        catch (const std::exception& exception)
        {
            return MakeFailure(
                EditorErrorCode::InvalidTextureAssetReference,
                exception.what(),
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        if (defaultIt->second.has_value() &&
            *defaultIt->second == normalizedTexturePath)
        {
            proposed.textures.erase(slotName);
        }
        else
        {
            proposed.textures[slotName] = normalizedTexturePath;
        }
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft texture binding is valid";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material texture override updated", &document),
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
MaterialInstanceDocumentService::ClearMaterialTextureOverride(
    std::string_view assetPath,
    std::string_view slot)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        const std::string slotName(slot);
        if (document.defaults.textures.find(slotName) ==
            document.defaults.textures.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownTextureSlot,
                "unknown material texture slot: " + slotName,
                &document);
        }
        PersistenceOverrides proposed = document.workingOverrides;
        proposed.textures.erase(slotName);
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft texture binding reset to material default";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material texture override cleared", &document),
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
MaterialInstanceDocumentService::SetMaterialRenderStateOverride(
    std::string_view assetPath,
    EditorMaterialRenderStateField field,
    const EditorMaterialRenderStateValue& value)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }

        const std::string fieldName = RenderStateFieldName(field);
        const auto defaultIt = document.defaults.renderStates.find(fieldName);
        if (defaultIt == document.defaults.renderStates.end())
        {
            return MakeFailure(
                EditorErrorCode::InvalidPayload,
                "unknown material render state: " + fieldName,
                &document);
        }

        const std::string valueName = RenderStateValueName(field, value);
        PersistenceOverrides proposed = document.workingOverrides;
        if (valueName == defaultIt->second)
        {
            proposed.renderStates.erase(fieldName);
        }
        else
        {
            proposed.renderStates[fieldName] = valueName;
        }
        proposed.renderStatesManaged =
            !proposed.renderStates.empty() ||
            document.baselineOverrides.renderStatesManaged;

        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft render state is valid";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material render state override updated", &document),
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
MaterialInstanceDocumentService::ClearMaterialRenderStateOverride(
    std::string_view assetPath,
    EditorMaterialRenderStateField field)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }

        const std::string fieldName = RenderStateFieldName(field);
        if (document.defaults.renderStates.find(fieldName) ==
            document.defaults.renderStates.end())
        {
            return MakeFailure(
                EditorErrorCode::InvalidPayload,
                "unknown material render state: " + fieldName,
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        proposed.renderStates.erase(fieldName);
        proposed.renderStatesManaged =
            !proposed.renderStates.empty() ||
            document.baselineOverrides.renderStatesManaged;
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft render state reset to material default";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material render state override cleared", &document),
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
MaterialInstanceDocumentService::ResetMaterialInstanceOverrides(
    std::string_view assetPath,
    EditorResetScope scope)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        PersistenceOverrides proposed = document.workingOverrides;
        switch (scope)
        {
        case EditorResetScope::Parameters:
            proposed.parameters.clear();
            break;
        case EditorResetScope::Textures:
            proposed.textures.clear();
            break;
        case EditorResetScope::RenderStates:
            proposed.renderStates.clear();
            break;
        case EditorResetScope::All:
            proposed.parameters.clear();
            proposed.textures.clear();
            proposed.renderStates.clear();
            break;
        }
        if (scope == EditorResetScope::RenderStates ||
            scope == EditorResetScope::All)
        {
            proposed.renderStatesManaged =
                document.baselineOverrides.renderStatesManaged;
        }
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft overrides reset to material defaults";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material instance overrides reset", &document),
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
MaterialInstanceDocumentService::RevertMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        PersistenceOverrides proposed = document.baselineOverrides;
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft reverted to the last saved baseline";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material instance document reverted", &document),
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
MaterialInstanceDocumentService::ReloadMaterialInstanceDocument(
    std::string_view assetPath,
    EditorDirtyDocumentPolicy dirtyPolicy)
{
    try
    {
        const std::string normalized = NormalizeMaterialInstancePath(assetPath);
        Document& current = RequireDocument(normalized);
        const bool dirty = current.state != MaterialEditorDocumentState::Clean;
        if (dirty && dirtyPolicy == EditorDirtyDocumentPolicy::RequireClean)
        {
            return MakeFailure(
                EditorErrorCode::DocumentDirty,
                "reload requires an explicit discard policy for dirty document: " +
                    normalized,
                &current);
        }

        const LoadedDocument loaded = LoadDocumentFromDisk(normalized);
        Document reloaded;
        reloaded.assetPath = loaded.assetPath;
        reloaded.absolutePath = loaded.absolutePath;
        reloaded.baseMaterialPath = loaded.baseMaterialPath;
        reloaded.baseMaterialAbsolutePath = loaded.baseMaterialAbsolutePath;
        reloaded.sourceJson = loaded.sourceJson;
        reloaded.materialJson = loaded.materialJson;
        reloaded.defaults = loaded.defaults;
        reloaded.baselineOverrides = loaded.overrides;
        reloaded.workingOverrides = loaded.overrides;
        reloaded.sourceDigest = loaded.sourceDigest;
        reloaded.schemaDigest = loaded.schemaDigest;
        reloaded.references = current.references;
        reloaded.revision = current.revision;
        IncrementRevision(reloaded);
        reloaded.validation = MaterialEditorValidationState::Unknown;
        reloaded.validationMessage.clear();

        const auto iterator = documents.find(normalized);
        iterator->second = std::move(reloaded);
        selectedDocumentPath = normalized;
        return MakeSnapshotResult(
            MakeSuccess("material instance document reloaded", &iterator->second),
            &iterator->second);
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
