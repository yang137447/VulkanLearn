#include "editor/service/material/materialInstanceDocumentBatch.h"

#include "editor/service/material/materialInstanceDocumentReferences.h"
#include "editor/service/material/materialInstanceDocumentValidation.h"
#include "editor/service/materialInstanceDocumentService.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace VL::Editor
{
namespace
{

using Json = nlohmann::json;
using PersistenceNumericType =
    Persistence::MaterialInstanceNumericType;
using PersistenceNumericValue =
    Persistence::MaterialInstanceNumericValue;

using MaterialDocumentDetail::RenderStateFieldName;
using MaterialDocumentDetail::RenderStateValueName;
using MaterialDocumentDetail::ReadStringField;
using MaterialDocumentDetail::SameReference;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;
using MaterialDocumentDetail::ValidateEditorParameterRange;

} // namespace

MaterialEditorServiceResult
MaterialInstanceDocumentService::ResolveSceneMaterialAsset(
    const ResolveSceneMaterialAssetPayload& payload)
{
    try
    {
        const std::string scenePath = NormalizeRelativePath(
            payload.sceneIdentity);
        const Json sceneJson = ReadJsonFile(ResolveResourceFile(scenePath));
        if (!sceneJson.is_object() || !sceneJson.contains("objects") ||
            !sceneJson.at("objects").is_array())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene does not contain an objects array: " + scenePath);
        }

        const Json* selectedObject = nullptr;
        std::size_t selectedObjectIndex = 0;
        const Json& objects = sceneJson.at("objects");
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            const Json& object = objects.at(index);
            if (!object.is_object())
            {
                continue;
            }
            const bool identityMatches =
                object.value("name", std::string()) == payload.objectIdentity ||
                object.value("id", std::string()) == payload.objectIdentity ||
                object.value("objectId", std::string()) == payload.objectIdentity;
            if (identityMatches)
            {
                selectedObject = &object;
                selectedObjectIndex = index;
                break;
            }
        }

        if (selectedObject == nullptr)
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene object was not found: " + payload.objectIdentity);
        }

        const std::string objectType = selectedObject->value(
            "type", std::string());
        std::string sourceAssetPath;
        if (objectType == "mesh")
        {
            sourceAssetPath = ReadStringField(
                *selectedObject,
                "modelPath",
                payload.objectIdentity);
        }
        else if (objectType == "terrain")
        {
            sourceAssetPath = ReadStringField(
                *selectedObject,
                "terrainPath",
                payload.objectIdentity);
        }
        else
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene object is not a mesh or terrain: " +
                    payload.objectIdentity);
        }

        sourceAssetPath = NormalizeRelativePath(sourceAssetPath);
        const Json sourceAssetJson = ReadJsonFile(
            ResolveResourceFile(sourceAssetPath));
        if (!sourceAssetJson.is_object())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "referenced mesh/terrain asset is not an object: " +
                    sourceAssetPath);
        }

        Json slots = Json::array();
        if (sourceAssetJson.contains("materialSlots") &&
            sourceAssetJson.at("materialSlots").is_array())
        {
            slots = sourceAssetJson.at("materialSlots");
        }
        else if (sourceAssetJson.contains("materials") &&
                 sourceAssetJson.at("materials").is_array())
        {
            slots = sourceAssetJson.at("materials");
        }
        else if (sourceAssetJson.contains("materialInstancePath") &&
                 sourceAssetJson.at("materialInstancePath").is_string())
        {
            slots.push_back(Json{
                {"slot", "surface"},
                {"materialInstancePath",
                 sourceAssetJson.at("materialInstancePath")}});
        }

        if (slots.empty())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "referenced asset does not define material slots: " +
                    sourceAssetPath);
        }

        std::size_t slotIndex = 0;
        if (payload.section.has_value())
        {
            slotIndex = *payload.section;
        }
        else if (slots.size() != 1)
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene object has multiple material slots; section is required: " +
                    payload.objectIdentity);
        }
        if (slotIndex >= slots.size())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "material section is outside the slot array: " +
                    std::to_string(slotIndex));
        }

        const Json& slot = slots.at(slotIndex);
        std::string materialInstancePath;
        std::string slotName = "section" + std::to_string(slotIndex);
        if (slot.is_string())
        {
            materialInstancePath = slot.get<std::string>();
        }
        else if (slot.is_object())
        {
            materialInstancePath = ReadStringField(
                slot,
                "materialInstancePath",
                sourceAssetPath);
            slotName = slot.value(
                "slot",
                slot.value("name", slotName));
        }
        else
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "material slot is neither a string nor an object: " +
                    sourceAssetPath);
        }

        materialInstancePath = NormalizeMaterialInstancePath(
            materialInstancePath);
        MaterialEditorReferenceSnapshot reference;
        reference.sceneIdentity = scenePath;
        reference.objectIdentity = payload.objectIdentity;
        reference.objectType = objectType;
        reference.sourceAssetPath = sourceAssetPath;
        reference.selector = slotName + "[" +
            std::to_string(slotIndex) + "]";
        reference.materialInstancePath = materialInstancePath;

        MaterialEditorServiceResult result = MakeSuccess(
            "scene material reference resolved: " + materialInstancePath);
        result.references.push_back(reference);

        // Resolve 本身不打开文档；若同一路径已经打开，则只补全当前导航来源。
        Document* document = FindDocument(materialInstancePath);
        if (document != nullptr)
        {
            const auto duplicate = std::find_if(
                document->references.begin(),
                document->references.end(),
                [&reference](const MaterialEditorReferenceSnapshot& item)
                {
                    return SameReference(item, reference);
                });
            if (duplicate == document->references.end())
            {
                document->references.push_back(reference);
            }
            result.documentRevision = document->revision;
            result.document = MakeDocumentSnapshot(*document);
        }
        result.editor = BuildSnapshot();
        static_cast<void>(selectedObjectIndex);
        return result;
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(
            EditorErrorCode::ReferenceResolutionFailed,
            exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ExecuteBatch(
    const ExecuteEditorCommandBatchPayload& batch,
    std::optional<EditorDocumentRevision> expectedRevision)
{
    try
    {
        if (const auto validationError = ValidateEditorCommandBatch(batch))
        {
            return MakeFailure(
                *validationError,
                "editor command batch failed protocol validation");
        }

        const auto readPath = [](const EditorCommandBatchItem& item)
            -> std::string
        {
            return std::visit(
                [](const auto& payload) -> std::string
                {
                    return payload.assetPath;
                },
                item.payload);
        };

        const std::string assetPath = NormalizeMaterialInstancePath(
            readPath(batch.commands.front()));
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        if (expectedRevision.has_value() &&
            *expectedRevision != document.revision)
        {
            return MakeFailure(
                EditorErrorCode::StaleDocumentRevision,
                "expected document revision does not match the open document",
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        bool saveRequested = false;
        bool validationRequested = false;
        for (const EditorCommandBatchItem& item : batch.commands)
        {
            if (NormalizeMaterialInstancePath(readPath(item)) != assetPath)
            {
                ThrowServiceError(
                    EditorErrorCode::InvalidPayload,
                    "all commands in a batch must target one MI asset");
            }

            switch (item.type)
            {
            case EditorCommandType::SetMaterialParameterOverride:
            {
                const auto& payload =
                    std::get<SetMaterialParameterOverridePayload>(item.payload);
                const auto defaultIt = document.defaults.parameters.find(
                    payload.parameter);
                if (defaultIt == document.defaults.parameters.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownParameter,
                        "unknown material parameter: " + payload.parameter);
                }
                const PersistenceNumericType expectedType =
                    Persistence::GetMaterialInstanceNumericType(
                        defaultIt->second);
                const PersistenceNumericValue value = ToPersistenceValue(
                    payload.value);
                if (ToPersistenceParameterType(payload.parameterType) !=
                        expectedType ||
                    Persistence::GetMaterialInstanceNumericType(value) !=
                        expectedType ||
                    !IsFiniteEditorMaterialParameterValue(payload.value))
                {
                    ThrowServiceError(
                        EditorErrorCode::ParameterTypeMismatch,
                        "material parameter type/value does not match schema: " +
                            payload.parameter);
                }
                const Json& descriptor = document.materialJson.at(
                    "parameters").at(payload.parameter);
                ValidateEditorParameterRange(
                    descriptor,
                    payload.value,
                    payload.parameter);
                if (Persistence::MaterialInstanceNumericValuesExactlyEqual(
                        value,
                        defaultIt->second))
                {
                    proposed.parameters.erase(payload.parameter);
                }
                else
                {
                    proposed.parameters[payload.parameter] = value;
                }
                break;
            }
            case EditorCommandType::ClearMaterialParameterOverride:
            {
                const auto& payload =
                    std::get<ClearMaterialParameterOverridePayload>(item.payload);
                if (document.defaults.parameters.find(payload.parameter) ==
                    document.defaults.parameters.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownParameter,
                        "unknown material parameter: " + payload.parameter);
                }
                proposed.parameters.erase(payload.parameter);
                break;
            }
            case EditorCommandType::SetMaterialTextureOverride:
            {
                const auto& payload =
                    std::get<SetMaterialTextureOverridePayload>(item.payload);
                const auto defaultIt = document.defaults.textures.find(
                    payload.slot);
                if (defaultIt == document.defaults.textures.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownTextureSlot,
                        "unknown material texture slot: " + payload.slot);
                }
                const std::string texturePath = NormalizeTextureAssetPath(
                    payload.textureAssetPath);
                ValidateTextureReference(texturePath, document.assetPath);
                if (defaultIt->second.has_value() &&
                    *defaultIt->second == texturePath)
                {
                    proposed.textures.erase(payload.slot);
                }
                else
                {
                    proposed.textures[payload.slot] = texturePath;
                }
                break;
            }
            case EditorCommandType::ClearMaterialTextureOverride:
            {
                const auto& payload =
                    std::get<ClearMaterialTextureOverridePayload>(item.payload);
                if (document.defaults.textures.find(payload.slot) ==
                    document.defaults.textures.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownTextureSlot,
                        "unknown material texture slot: " + payload.slot);
                }
                proposed.textures.erase(payload.slot);
                break;
            }
            case EditorCommandType::SetMaterialRenderStateOverride:
            {
                const auto& payload =
                    std::get<SetMaterialRenderStateOverridePayload>(item.payload);
                const std::string fieldName =
                    RenderStateFieldName(payload.field);
                const auto defaultIt = document.defaults.renderStates.find(fieldName);
                if (defaultIt == document.defaults.renderStates.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::InvalidPayload,
                        "unknown material render state: " + fieldName);
                }
                const std::string valueName =
                    RenderStateValueName(payload.field, payload.value);
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
                break;
            }
            case EditorCommandType::ClearMaterialRenderStateOverride:
            {
                const auto& payload =
                    std::get<ClearMaterialRenderStateOverridePayload>(item.payload);
                const std::string fieldName =
                    RenderStateFieldName(payload.field);
                if (document.defaults.renderStates.find(fieldName) ==
                    document.defaults.renderStates.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::InvalidPayload,
                        "unknown material render state: " + fieldName);
                }
                proposed.renderStates.erase(fieldName);
                proposed.renderStatesManaged =
                    !proposed.renderStates.empty() ||
                    document.baselineOverrides.renderStatesManaged;
                break;
            }
            case EditorCommandType::ResetMaterialInstanceOverrides:
            {
                const auto& payload =
                    std::get<ResetMaterialInstanceOverridesPayload>(item.payload);
                if (payload.scope == EditorResetScope::Parameters ||
                    payload.scope == EditorResetScope::All)
                {
                    proposed.parameters.clear();
                }
                if (payload.scope == EditorResetScope::Textures ||
                    payload.scope == EditorResetScope::All)
                {
                    proposed.textures.clear();
                }
                if (payload.scope == EditorResetScope::RenderStates ||
                    payload.scope == EditorResetScope::All)
                {
                    proposed.renderStates.clear();
                }
                break;
            }
            case EditorCommandType::RevertMaterialInstanceDocument:
                proposed = document.baselineOverrides;
                break;
            case EditorCommandType::ValidateMaterialInstanceDocument:
                validationRequested = true;
                break;
            case EditorCommandType::SaveMaterialInstanceDocument:
                saveRequested = true;
                break;
            case EditorCommandType::ReloadMaterialInstanceDocument:
            case EditorCommandType::ConnectMaterialInstancePreview:
            case EditorCommandType::DisconnectMaterialInstancePreview:
            case EditorCommandType::ApplyMaterialInstancePreview:
            case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
            default:
                ThrowServiceError(
                    EditorErrorCode::InvalidPayload,
                    "command type is not valid inside a document batch");
            }
        }

        ValidateWorkingState(document, proposed);
        const bool draftChanged =
            proposed.parameters != document.workingOverrides.parameters ||
            proposed.textures != document.workingOverrides.textures ||
            proposed.renderStates != document.workingOverrides.renderStates;

        MaterialEditorServiceResult result;
        if (saveRequested)
        {
            const Json candidate = BuildCandidateJson(document, proposed);
            const Persistence::MaterialInstanceSaveResult saveResult =
                Persistence::MaterialInstancePersistence::SaveTextIfUnchanged(
                    document.absolutePath,
                    document.sourceDigest,
                    JsonString(candidate));
            if (!saveResult.Succeeded())
            {
                const EditorErrorCode errorCode = saveResult.HasSourceConflict()
                    ? EditorErrorCode::SourceChanged
                    : (saveResult.status ==
                           Persistence::MaterialInstancePersistenceStatus::WriteFailed
                           ? EditorErrorCode::AtomicWriteFailed
                           : EditorErrorCode::AssetNotFound);
                result = MakeFailure(errorCode, saveResult.errorMessage, &document);
                result.expectedDigest = saveResult.expectedDigest.ToHex();
                result.observedDigest = saveResult.observedDigest.ToHex();
                result.newDigest = saveResult.newDigest.ToHex();
                return result;
            }

            document.sourceJson = candidate;
            document.baselineOverrides = proposed;
            document.workingOverrides = proposed;
            document.sourceDigest = saveResult.newDigest;
            document.state = MaterialEditorDocumentState::Clean;
            document.validation = MaterialEditorValidationState::Valid;
            document.validationMessage = "batch validated and saved";
            IncrementRevision(document);
            result = MakeSnapshotResult(
                MakeSuccess("editor command batch saved", &document),
                &document);
            result.expectedDigest = saveResult.expectedDigest.ToHex();
            result.observedDigest = saveResult.observedDigest.ToHex();
            result.newDigest = saveResult.newDigest.ToHex();
            return result;
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = validationRequested
            ? "batch draft validation succeeded"
            : "editor command batch applied";
        UpdateDirtyState(document);
        if (draftChanged)
        {
            IncrementRevision(document);
        }
        return MakeSnapshotResult(
            MakeSuccess("editor command batch applied", &document),
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

} // namespace VL::Editor
