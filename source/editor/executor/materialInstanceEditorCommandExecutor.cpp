#include "editor/executor/materialInstanceEditorCommandExecutor.h"

#include <exception>
#include <string>
#include <utility>

namespace VL::Editor
{
namespace
{

bool IsDirty(const MaterialEditorDocumentSnapshot& document) noexcept
{
    return document.state != MaterialEditorDocumentState::Clean;
}

EditorCommandResult BaseResult(const EditorCommandEnvelope& command)
{
    EditorCommandResult result;
    result.protocolVersion = kEditorCommandProtocolVersion;
    result.commandId = command.commandId;
    return result;
}

} // namespace

MaterialInstanceEditorCommandExecutor::MaterialInstanceEditorCommandExecutor(
    MaterialInstanceDocumentService& documentServiceValue) noexcept
    : documentService(&documentServiceValue)
{
}

EditorCommandResult
MaterialInstanceEditorCommandExecutor::MakeRejected(
    const EditorCommandEnvelope& command,
    EditorErrorCode errorCode,
    std::string message)
{
    EditorCommandResult result = BaseResult(command);
    result.status = EditorCommandStatus::Rejected;
    result.errorCode = errorCode;
    result.message = std::move(message);
    return result;
}

bool MaterialInstanceEditorCommandExecutor::IsAdmissionError(
    EditorErrorCode errorCode) noexcept
{
    switch (errorCode)
    {
    case EditorErrorCode::InvalidProtocolVersion:
    case EditorErrorCode::InvalidCommandId:
    case EditorErrorCode::InvalidCommandType:
    case EditorErrorCode::InvalidPayload:
    case EditorErrorCode::MissingExpectedDocumentRevision:
    case EditorErrorCode::StaleDocumentRevision:
    case EditorErrorCode::DuplicateCommandId:
        return true;
    default:
        return false;
    }
}

std::optional<std::string>
MaterialInstanceEditorCommandExecutor::GetCommandAssetPath(
    const EditorCommandEnvelope& command)
{
    return GetEditorCommandAssetPath(command);
}

EditorCommandResult
MaterialInstanceEditorCommandExecutor::ConvertResult(
    const EditorCommandEnvelope& command,
    const MaterialEditorServiceResult& serviceResult)
{
    EditorCommandResult result = BaseResult(command);
    result.status = serviceResult.succeeded
        ? EditorCommandStatus::Succeeded
        : (IsAdmissionError(serviceResult.errorCode)
               ? EditorCommandStatus::Rejected
               : EditorCommandStatus::Failed);
    result.errorCode = serviceResult.succeeded
        ? EditorErrorCode::None
        : serviceResult.errorCode;
    result.message = serviceResult.message;

    const MaterialEditorDocumentSnapshot* document = nullptr;
    if (serviceResult.document.has_value())
    {
        document = &serviceResult.document.value();
    }

    if (serviceResult.documentRevision.has_value())
    {
        result.documentRevision = serviceResult.documentRevision;
    }
    else if (document != nullptr)
    {
        result.documentRevision = document->revision;
    }

    if (document != nullptr)
    {
        result.payload = EditorDocumentResultPayload{
            document->revision,
            IsDirty(*document)};
    }
    else if (serviceResult.documentRevision.has_value())
    {
        result.payload = EditorDocumentResultPayload{
            serviceResult.documentRevision.value(),
            false};
    }

    return result;
}

EditorCommandResult
MaterialInstanceEditorCommandExecutor::ExecuteDocumentCommand(
    const EditorCommandEnvelope& command)
{
    if (documentService == nullptr)
    {
        return MakeRejected(
            command,
            EditorErrorCode::ValidationFailed,
            "material instance document service is unavailable");
    }

    MaterialEditorServiceResult serviceResult;
    switch (command.type)
    {
    case EditorCommandType::ResolveSceneMaterialAsset:
        serviceResult = documentService->ResolveSceneMaterialAsset(
            std::get<ResolveSceneMaterialAssetPayload>(command.payload));
        break;
    case EditorCommandType::ListMaterialInstanceAssets:
    {
        const auto& payload =
            std::get<ListMaterialInstanceAssetsPayload>(command.payload);
        serviceResult = documentService->ListMaterialInstanceAssets(
            payload.searchText,
            payload.pageIndex,
            payload.pageSize);
        break;
    }
    case EditorCommandType::OpenMaterialInstanceAsset:
    {
        const auto& payload =
            std::get<OpenMaterialInstanceAssetPayload>(command.payload);
        serviceResult = documentService->OpenMaterialInstanceAsset(
            payload.assetPath,
            payload.origin);
        break;
    }
    case EditorCommandType::SelectMaterialInstanceDocument:
        serviceResult = documentService->SelectMaterialInstanceDocument(
            std::get<MaterialInstanceAssetPathPayload>(command.payload)
                .assetPath);
        break;
    case EditorCommandType::CloseMaterialInstanceAsset:
    {
        const auto& payload =
            std::get<CloseMaterialInstanceAssetPayload>(command.payload);
        serviceResult = documentService->CloseMaterialInstanceAsset(
            payload.assetPath,
            payload.dirtyPolicy);
        break;
    }
    case EditorCommandType::GetMaterialInstanceDocument:
        serviceResult = documentService->GetMaterialInstanceDocument(
            std::get<MaterialInstanceAssetPathPayload>(command.payload)
                .assetPath);
        break;
    case EditorCommandType::GetMaterialInstanceReferenceContext:
        serviceResult = documentService->GetMaterialInstanceReferenceContext(
            std::get<MaterialInstanceAssetPathPayload>(command.payload)
                .assetPath);
        break;
    case EditorCommandType::SetMaterialParameterOverride:
    {
        const auto& payload = std::get<SetMaterialParameterOverridePayload>(
            command.payload);
        serviceResult = documentService->SetMaterialParameterOverride(
            payload.assetPath,
            payload.parameter,
            payload.parameterType,
            payload.value);
        break;
    }
    case EditorCommandType::ClearMaterialParameterOverride:
    {
        const auto& payload = std::get<ClearMaterialParameterOverridePayload>(
            command.payload);
        serviceResult = documentService->ClearMaterialParameterOverride(
            payload.assetPath,
            payload.parameter);
        break;
    }
    case EditorCommandType::SetMaterialTextureOverride:
    {
        const auto& payload = std::get<SetMaterialTextureOverridePayload>(
            command.payload);
        serviceResult = documentService->SetMaterialTextureOverride(
            payload.assetPath,
            payload.slot,
            payload.textureAssetPath);
        break;
    }
    case EditorCommandType::ClearMaterialTextureOverride:
    {
        const auto& payload = std::get<ClearMaterialTextureOverridePayload>(
            command.payload);
        serviceResult = documentService->ClearMaterialTextureOverride(
            payload.assetPath,
            payload.slot);
        break;
    }
    case EditorCommandType::SetMaterialRenderStateOverride:
    {
        const auto& payload = std::get<SetMaterialRenderStateOverridePayload>(
            command.payload);
        serviceResult = documentService->SetMaterialRenderStateOverride(
            payload.assetPath,
            payload.field,
            payload.value);
        break;
    }
    case EditorCommandType::ClearMaterialRenderStateOverride:
    {
        const auto& payload = std::get<ClearMaterialRenderStateOverridePayload>(
            command.payload);
        serviceResult = documentService->ClearMaterialRenderStateOverride(
            payload.assetPath,
            payload.field);
        break;
    }
    case EditorCommandType::ResetMaterialInstanceOverrides:
    {
        const auto& payload = std::get<ResetMaterialInstanceOverridesPayload>(
            command.payload);
        serviceResult = documentService->ResetMaterialInstanceOverrides(
            payload.assetPath,
            payload.scope);
        break;
    }
    case EditorCommandType::RevertMaterialInstanceDocument:
        serviceResult = documentService->RevertMaterialInstanceDocument(
            std::get<MaterialInstanceAssetPathPayload>(command.payload)
                .assetPath);
        break;
    case EditorCommandType::ReloadMaterialInstanceDocument:
    {
        const auto& payload = std::get<ReloadMaterialInstanceDocumentPayload>(
            command.payload);
        serviceResult = documentService->ReloadMaterialInstanceDocument(
            payload.assetPath,
            payload.dirtyPolicy);
        break;
    }
    case EditorCommandType::ValidateMaterialInstanceDocument:
        serviceResult = documentService->ValidateMaterialInstanceDocument(
            std::get<MaterialInstanceAssetPathPayload>(command.payload)
                .assetPath);
        break;
    case EditorCommandType::SaveMaterialInstanceDocument:
        serviceResult = documentService->SaveMaterialInstanceDocument(
            std::get<MaterialInstanceAssetPathPayload>(command.payload)
                .assetPath);
        break;
    case EditorCommandType::ExecuteEditorCommandBatch:
    {
        const auto& payload = std::get<ExecuteEditorCommandBatchPayload>(
            command.payload);
        serviceResult = documentService->ExecuteBatch(
            payload,
            command.expectedDocumentRevision);
        if (serviceResult.succeeded)
        {
            EditorCommandResult result = ConvertResult(command, serviceResult);
            result.payload = EditorBatchResultPayload{
                static_cast<uint32_t>(payload.commands.size()),
                serviceResult.documentRevision.value_or(0)};
            return result;
        }
        break;
    }
    case EditorCommandType::OpenTextureAsset:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::ApplyMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
    case EditorCommandType::GetEditorCommandResult:
    case EditorCommandType::ListEditorEvents:
        return MakeRejected(
            command,
            EditorErrorCode::PreviewUnavailable,
            "command requires an optional runtime adapter and is not available");
    }

    return ConvertResult(command, serviceResult);
}

EditorCommandResult
MaterialInstanceEditorCommandExecutor::Execute(
    const EditorCommandEnvelope& command)
{
    if (const auto error = ValidateEditorCommandEnvelope(command))
    {
        EditorCommandResult result = MakeRejected(
            command,
            *error,
            std::string(GetEditorErrorCodeName(*error)));
        lastResult = result;
        return result;
    }

    try
    {
        EditorCommandResult result = ExecuteDocumentCommand(command);
        lastResult = result;
        return result;
    }
    catch (const std::exception& exception)
    {
        EditorCommandResult result = BaseResult(command);
        result.status = EditorCommandStatus::Failed;
        result.errorCode = EditorErrorCode::ValidationFailed;
        result.message = exception.what();
        lastResult = result;
        return result;
    }
}

std::size_t MaterialInstanceEditorCommandExecutor::Drain(
    EditorCommandBus& commandBus,
    std::size_t maxCount)
{
    const std::vector<EditorCommandEnvelope> commands =
        commandBus.Drain(maxCount);
    for (const EditorCommandEnvelope& command : commands)
    {
        EditorCommandResult result = Execute(command);
        commandBus.PublishResult(result);

        if (const auto path = GetCommandAssetPath(command))
        {
            if (documentService != nullptr)
            {
                if (const auto revision = documentService->GetDocumentRevision(*path))
                {
                    commandBus.SetDocumentRevision(*path, *revision);
                }
            }
        }
    }
    return commands.size();
}

MaterialEditorSnapshot
MaterialInstanceEditorCommandExecutor::BuildSnapshot()
{
    return documentService->BuildSnapshot();
}

} // namespace VL::Editor
