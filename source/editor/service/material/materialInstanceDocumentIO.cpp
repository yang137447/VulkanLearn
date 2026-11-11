#include "editor/service/material/materialInstanceDocumentIO.h"

#include "editor/service/material/materialInstanceDocumentInternal.h"
#include "editor/service/materialInstanceDocumentService.h"
#include "material/validation/materialAssetValidator.h"
#include "shader/build/contentHash.h"

#include <fstream>
#include <string>
#include <utility>

namespace VL::Editor
{

using MaterialDocumentDetail::ReadStringField;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::ReadJsonFile(
    const std::filesystem::path& path) const
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        ThrowServiceError(
            EditorErrorCode::AssetNotFound,
            "failed to open JSON asset: " + path.string());
    }
    try
    {
        Json value;
        input >> value;
        return value;
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::ValidationFailed,
            "failed to parse JSON asset '" + path.string() + "': " +
                exception.what());
    }
}
MaterialInstanceDocumentService::LoadedDocument
MaterialInstanceDocumentService::LoadDocumentFromDisk(
    std::string_view assetPath) const
{
    LoadedDocument loaded;
    try
    {
        loaded.assetPath = NormalizeMaterialInstancePath(assetPath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidAssetType,
            exception.what());
    }

    loaded.absolutePath = ResolveResourceFile(loaded.assetPath);
    loaded.sourceJson = ReadJsonFile(loaded.absolutePath);
    if (!loaded.sourceJson.is_object())
    {
        ThrowServiceError(
            EditorErrorCode::ValidationFailed,
            "material instance must be a JSON object: " +
                loaded.assetPath);
    }

    loaded.baseMaterialPath = ReadStringField(
        loaded.sourceJson, "material", loaded.assetPath);
    loaded.baseMaterialAbsolutePath =
        ResolveMaterialDefinitionPath(loaded.baseMaterialPath);
    loaded.materialJson = ReadJsonFile(loaded.baseMaterialAbsolutePath);
    try
    {
        MaterialAssetValidator::ValidateDefinition(
            loaded.materialJson, loaded.baseMaterialPath);
        loaded.defaults = Persistence::ParseMaterialInstanceDefaults(
            loaded.materialJson);
        loaded.overrides =
            Persistence::ParseMaterialInstanceSparseOverrides(
                loaded.sourceJson, loaded.defaults);
        ValidateManagedMaterialInstance(
            loaded.materialJson, loaded.sourceJson, loaded.assetPath);
    }
    catch (const ServiceError&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::ValidationFailed,
            exception.what());
    }

    const Persistence::MaterialInstanceFileSnapshot sourceSnapshot =
        Persistence::MaterialInstancePersistence::ReadSnapshot(
            loaded.absolutePath);
    if (!sourceSnapshot.Succeeded())
    {
        ThrowServiceError(
            EditorErrorCode::AssetNotFound,
            sourceSnapshot.errorMessage);
    }
    loaded.sourceDigest = sourceSnapshot.digest;
    try
    {
        loaded.schemaDigest =
            ContentHasher::HashFile(loaded.baseMaterialAbsolutePath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::AssetNotFound,
            "failed to hash material definition: " +
                std::string(exception.what()));
    }
    return loaded;
}
void MaterialInstanceDocumentService::RefreshExternalChange(
    Document& document)
{
    const Persistence::MaterialInstanceFileSnapshot snapshot =
        Persistence::MaterialInstancePersistence::ReadSnapshot(
            document.absolutePath);
    if (!snapshot.Succeeded())
    {
        document.state = MaterialEditorDocumentState::SourceChanged;
        document.validation = MaterialEditorValidationState::Invalid;
        document.validationMessage = snapshot.errorMessage;
        return;
    }
    if (snapshot.digest != document.sourceDigest)
    {
        document.state = MaterialEditorDocumentState::SourceChanged;
        document.validation = MaterialEditorValidationState::Invalid;
        document.validationMessage =
            "MI source changed outside the Material Editor; reload before editing.";
    }
}
MaterialEditorServiceResult
MaterialInstanceDocumentService::SaveMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            MaterialEditorServiceResult result = MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
            result.expectedDigest = document.sourceDigest.ToHex();
            const Persistence::MaterialInstanceFileSnapshot observed =
                Persistence::MaterialInstancePersistence::ReadSnapshot(
                    document.absolutePath);
            result.observedDigest = observed.digest.ToHex();
            return result;
        }

        Json candidate;
        try
        {
            candidate = BuildCandidateJson(
                document,
                document.workingOverrides);
            ValidateWorkingState(document, document.workingOverrides);
        }
        catch (const ServiceError& exception)
        {
            document.state = MaterialEditorDocumentState::SaveFailed;
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        catch (const std::exception& exception)
        {
            document.state = MaterialEditorDocumentState::SaveFailed;
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(
                EditorErrorCode::ValidationFailed,
                exception.what(),
                &document);
        }

        const std::string candidateText = JsonString(candidate);
        const Persistence::MaterialInstanceSaveResult saveResult =
            Persistence::MaterialInstancePersistence::SaveTextIfUnchanged(
                document.absolutePath,
                document.sourceDigest,
                candidateText);
        MaterialEditorServiceResult result;
        result.expectedDigest = saveResult.expectedDigest.ToHex();
        result.observedDigest = saveResult.observedDigest.ToHex();
        result.newDigest = saveResult.newDigest.ToHex();
        if (!saveResult.Succeeded())
        {
            const EditorErrorCode errorCode = saveResult.HasSourceConflict()
                ? EditorErrorCode::SourceChanged
                : (saveResult.status ==
                   Persistence::MaterialInstancePersistenceStatus::WriteFailed
                       ? EditorErrorCode::AtomicWriteFailed
                       : EditorErrorCode::AssetNotFound);
            document.state = saveResult.HasSourceConflict()
                ? MaterialEditorDocumentState::SourceChanged
                : MaterialEditorDocumentState::SaveFailed;
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = saveResult.errorMessage;
            result = MakeFailure(errorCode, saveResult.errorMessage, &document);
            result.expectedDigest = saveResult.expectedDigest.ToHex();
            result.observedDigest = saveResult.observedDigest.ToHex();
            result.newDigest = saveResult.newDigest.ToHex();
            return result;
        }

        document.sourceJson = std::move(candidate);
        document.baselineOverrides = document.workingOverrides;
        document.sourceDigest = saveResult.newDigest;
        document.state = MaterialEditorDocumentState::Clean;
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "material instance document saved";
        IncrementRevision(document);
        result = MakeSnapshotResult(
            MakeSuccess("material instance document saved", &document),
            &document);
        result.expectedDigest = saveResult.expectedDigest.ToHex();
        result.observedDigest = saveResult.observedDigest.ToHex();
        result.newDigest = saveResult.newDigest.ToHex();
        return result;
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::AtomicWriteFailed, exception.what());
    }
}

} // namespace VL::Editor
