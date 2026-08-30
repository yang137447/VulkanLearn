#include "editor/preview/materialInstancePreviewController.h"

#include <exception>
#include <utility>

namespace VL::Editor::Preview
{
namespace
{

bool HasPayloadForType(const MaterialInstancePreviewCommand& command)
{
    switch (command.type)
    {
    case MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview:
        return std::holds_alternative<ConnectMaterialInstancePreviewPayload>(command.payload);
    case MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview:
        return std::holds_alternative<DisconnectMaterialInstancePreviewPayload>(command.payload);
    case MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview:
        return std::holds_alternative<ApplyMaterialInstancePreviewPayload>(command.payload);
    case MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline:
        return std::holds_alternative<RestoreMaterialInstancePreviewBaselinePayload>(command.payload);
    }
    return false;
}

std::optional<uint64_t> GetCommandDocumentRevision(
    const MaterialInstancePreviewCommand& command)
{
    switch (command.type)
    {
    case MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview:
    {
        const auto* payload = std::get_if<ConnectMaterialInstancePreviewPayload>(&command.payload);
        return payload == nullptr ? std::nullopt : payload->documentRevision;
    }
    case MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview:
    {
        const auto* payload = std::get_if<ApplyMaterialInstancePreviewPayload>(&command.payload);
        return payload == nullptr ? std::nullopt : std::optional<uint64_t>(payload->workingDraft.documentRevision);
    }
    case MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline:
    {
        const auto* payload = std::get_if<RestoreMaterialInstancePreviewBaselinePayload>(&command.payload);
        return payload == nullptr ? std::nullopt : std::optional<uint64_t>(payload->documentRevision);
    }
    case MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview:
        return std::nullopt;
    }
    return std::nullopt;
}

std::string AdapterMessage(
    const MaterialInstancePreviewAdapterResult& result,
    const char* fallback)
{
    return result.message.empty() ? std::string(fallback) : result.message;
}

} // namespace

MaterialInstancePreviewController::MaterialInstancePreviewController()
    : unavailableAdapter()
    , adapter(&unavailableAdapter)
{
}

MaterialInstancePreviewController::MaterialInstancePreviewController(
    IMaterialInstancePreviewAdapter& adapterValue)
    : unavailableAdapter()
    , adapter(&adapterValue)
{
}

void MaterialInstancePreviewController::NotifyActiveWorldChanged(
    MaterialInstancePreviewWorldIdentity activeWorldValue)
{
    activeWorldValue = NormalizeWorldIdentity(std::move(activeWorldValue));
    if (!activeWorldValue.IsValid())
    {
        ClearActiveWorld();
        return;
    }
    if (activeWorld.has_value() && *activeWorld == activeWorldValue)
    {
        return;
    }

    CancelPendingOperation();

    MaterialInstancePreviewErrorCode releaseError =
        MaterialInstancePreviewErrorCode::None;
    std::string diagnosticMessage =
        "Active World changed; material instance preview disconnected.";
    if (liveConnectionEstablished)
    {
        MaterialInstancePreviewAdapterResult adapterResult =
            ExecuteAdapter(MakeDisconnectAdapterCommand());
        if (adapterResult.status == PreviewAdapterOperationStatus::Pending &&
            adapterResult.operationId != 0)
        {
            adapter->Cancel(adapterResult.operationId);
        }
        if (adapterResult.status != PreviewAdapterOperationStatus::Completed)
        {
            releaseError = adapterResult.status ==
                    PreviewAdapterOperationStatus::Unavailable
                ? MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected
                : ResolveAdapterError(adapterResult);
            diagnosticMessage = AdapterMessage(
                adapterResult,
                "Previous material instance preview connection could not be released.");
        }
    }

    liveConnectionEstablished = false;
    activeWorld = std::move(activeWorldValue);
    AdvanceLiveGeneration();
    status.state = releaseError == MaterialInstancePreviewErrorCode::None
        ? MaterialInstancePreviewState::Disconnected
        : (releaseError == MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected
               ? MaterialInstancePreviewState::Unavailable
               : MaterialInstancePreviewState::Failed);
    status.world = activeWorld;
    status.materialInstancePath.reset();
    status.runtimeResourceGeneration = 0;
    status.observedDocumentRevision.reset();
    status.appliedDocumentRevision.reset();
    status.lastError = releaseError;
    status.diagnosticMessage = std::move(diagnosticMessage);
    TouchStatus();
}

void MaterialInstancePreviewController::ClearActiveWorld()
{
    const bool hadState = activeWorld.has_value() ||
        pendingOperation.has_value() ||
        liveConnectionEstablished ||
        status.world.has_value() ||
        status.materialInstancePath.has_value() ||
        status.state != MaterialInstancePreviewState::Disconnected;
    CancelPendingOperation();

    MaterialInstancePreviewErrorCode releaseError =
        MaterialInstancePreviewErrorCode::None;
    std::string diagnosticMessage =
        "Active World is unavailable; material instance preview disconnected.";
    if (liveConnectionEstablished)
    {
        MaterialInstancePreviewAdapterResult adapterResult =
            ExecuteAdapter(MakeDisconnectAdapterCommand());
        if (adapterResult.status == PreviewAdapterOperationStatus::Pending &&
            adapterResult.operationId != 0)
        {
            adapter->Cancel(adapterResult.operationId);
        }
        if (adapterResult.status != PreviewAdapterOperationStatus::Completed)
        {
            releaseError = adapterResult.status ==
                    PreviewAdapterOperationStatus::Unavailable
                ? MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected
                : ResolveAdapterError(adapterResult);
            diagnosticMessage = AdapterMessage(
                adapterResult,
                "Previous material instance preview connection could not be released.");
        }
    }

    activeWorld.reset();
    liveConnectionEstablished = false;
    if (hadState)
    {
        AdvanceLiveGeneration();
    }
    status.state = releaseError == MaterialInstancePreviewErrorCode::None
        ? MaterialInstancePreviewState::Unavailable
        : MaterialInstancePreviewState::Failed;
    status.world.reset();
    status.materialInstancePath.reset();
    status.runtimeResourceGeneration = 0;
    status.observedDocumentRevision.reset();
    status.appliedDocumentRevision.reset();
    status.lastError = releaseError;
    status.diagnosticMessage = std::move(diagnosticMessage);
    TouchStatus();
}

void MaterialInstancePreviewController::Shutdown()
{
    CancelPendingOperation();
    if (liveConnectionEstablished)
    {
        MaterialInstancePreviewAdapterResult adapterResult =
            ExecuteAdapter(MakeDisconnectAdapterCommand());
        if (adapterResult.status == PreviewAdapterOperationStatus::Pending &&
            adapterResult.operationId != 0)
        {
            adapter->Cancel(adapterResult.operationId);
        }
    }

    const uint64_t previousLiveGeneration = status.liveGeneration;
    activeWorld.reset();
    pendingOperation.reset();
    liveConnectionEstablished = false;
    status = MaterialInstancePreviewStatus{};
    status.liveGeneration = previousLiveGeneration;
    AdvanceLiveGeneration();
    status.state = MaterialInstancePreviewState::Disconnected;
    status.diagnosticMessage = "Material instance preview controller shut down.";
    TouchStatus();
}

MaterialInstancePreviewStatus MaterialInstancePreviewController::GetStatus() const
{
    return status;
}

std::optional<PreviewOperationId>
MaterialInstancePreviewController::GetPendingOperationId() const noexcept
{
    if (!pendingOperation.has_value())
    {
        return std::nullopt;
    }
    return pendingOperation->operationId;
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::MakeResult(
    const MaterialInstancePreviewCommand& command,
    MaterialInstancePreviewCommandStatus commandStatus,
    MaterialInstancePreviewErrorCode errorCode,
    std::string message,
    std::optional<uint64_t> documentRevision,
    std::optional<PreviewOperationId> operationId,
    bool liveSwapCommitted) const
{
    MaterialInstancePreviewCommandResult result;
    result.protocolVersion = kMaterialInstancePreviewProtocolVersion;
    result.commandId = command.commandId;
    result.correlationId = command.correlationId;
    result.status = commandStatus;
    result.errorCode = errorCode;
    result.message = std::move(message);
    result.documentRevision = documentRevision;
    result.payload.previewStatus = status;
    result.payload.appliedDocumentRevision = status.appliedDocumentRevision;
    result.payload.operationId = operationId;
    result.payload.liveSwapCommitted = liveSwapCommitted;
    return result;
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::Reject(
    const MaterialInstancePreviewCommand& command,
    MaterialInstancePreviewErrorCode errorCode,
    std::string message) const
{
    return MakeResult(
        command,
        MaterialInstancePreviewCommandStatus::Rejected,
        errorCode,
        std::move(message),
        GetCommandDocumentRevision(command));
}

std::optional<MaterialInstancePreviewController::NormalizedCommand>
MaterialInstancePreviewController::NormalizeCommand(
    const MaterialInstancePreviewCommand& command,
    MaterialInstancePreviewCommandResult& rejection) const
{
    if (command.protocolVersion != kMaterialInstancePreviewProtocolVersion)
    {
        rejection = Reject(
            command,
            MaterialInstancePreviewErrorCode::InvalidProtocolVersion,
            "Unsupported material instance preview protocol version.");
        return std::nullopt;
    }
    if (command.commandId == 0)
    {
        rejection = Reject(
            command,
            MaterialInstancePreviewErrorCode::InvalidCommand,
            "Material instance preview commandId must be non-zero.");
        return std::nullopt;
    }
    if (!HasPayloadForType(command))
    {
        rejection = Reject(
            command,
            MaterialInstancePreviewErrorCode::InvalidCommand,
            "Preview command payload does not match its command type.");
        return std::nullopt;
    }
    if (pendingOperation.has_value() &&
        command.type != MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview)
    {
        rejection = Reject(
            command,
            MaterialInstancePreviewErrorCode::PreviewOperationInProgress,
            "Another preview operation is still running.");
        return std::nullopt;
    }
    if (command.expectedPreviewGeneration.has_value() &&
        *command.expectedPreviewGeneration != status.liveGeneration)
    {
        rejection = Reject(
            command,
            MaterialInstancePreviewErrorCode::PreviewGenerationChanged,
            "Material instance preview generation is stale.");
        return std::nullopt;
    }

    NormalizedCommand normalized;
    normalized.adapterCommand.type = command.type;
    normalized.adapterCommand.bridgeLiveGeneration = status.liveGeneration;

    switch (command.type)
    {
    case MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview:
    {
        const auto& payload = std::get<ConnectMaterialInstancePreviewPayload>(command.payload);
        const MaterialInstancePreviewPathNormalizationResult pathResult =
            NormalizeMaterialInstancePath(payload.materialInstancePath);
        if (!pathResult.Succeeded())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::InvalidMaterialInstancePath,
                pathResult.errorMessage);
            return std::nullopt;
        }
        const MaterialInstancePreviewWorldIdentity world =
            NormalizeWorldIdentity(payload.world);
        if (!world.IsValid())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewUnavailable,
                "The active World identity is not available.");
            return std::nullopt;
        }
        if (!activeWorld.has_value())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewUnavailable,
                "The preview controller has not received an active World.");
            return std::nullopt;
        }
        if (*activeWorld != world)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewGenerationChanged,
                "Preview connect targets an obsolete World generation.");
            return std::nullopt;
        }
        if (command.expectedDocumentRevision.has_value() &&
            (!payload.documentRevision.has_value() ||
             *command.expectedDocumentRevision != *payload.documentRevision))
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::StaleDocumentRevision,
                "Preview connect document revision is stale.");
            return std::nullopt;
        }
        normalized.adapterCommand.world = world;
        normalized.adapterCommand.materialInstancePath = *pathResult.path;
        normalized.adapterCommand.documentRevision = payload.documentRevision;
        normalized.documentRevision = payload.documentRevision;
        return normalized;
    }
    case MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview:
        return normalized;
    case MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview:
    {
        if (!status.IsConnected() || !status.world.has_value() ||
            !status.materialInstancePath.has_value() || !activeWorld.has_value() ||
            *status.world != *activeWorld)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewNotConnected,
                "Material instance preview is not connected to the active World.");
            return std::nullopt;
        }
        const auto& payload = std::get<ApplyMaterialInstancePreviewPayload>(command.payload);
        const MaterialInstancePreviewPathNormalizationResult pathResult =
            NormalizeMaterialInstancePath(payload.workingDraft.materialInstancePath);
        if (!pathResult.Succeeded())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::InvalidMaterialInstancePath,
                pathResult.errorMessage);
            return std::nullopt;
        }
        if (*pathResult.path != *status.materialInstancePath)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewNotConnected,
                "Preview draft does not match the connected material instance.");
            return std::nullopt;
        }
        if (payload.workingDraft.serializedWorkingDraft.empty())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::ValidationFailed,
                "Preview working draft must contain serialized content.");
            return std::nullopt;
        }
        if (!command.expectedDocumentRevision.has_value() ||
            *command.expectedDocumentRevision != payload.workingDraft.documentRevision)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::StaleDocumentRevision,
                "Preview apply requires a matching expected document revision.");
            return std::nullopt;
        }
        MaterialInstancePreviewDraft normalizedDraft = payload.workingDraft;
        normalizedDraft.materialInstancePath = pathResult.path->value;
        normalized.adapterCommand.world = *status.world;
        normalized.adapterCommand.materialInstancePath = *pathResult.path;
        normalized.adapterCommand.bridgeLiveGeneration = status.liveGeneration;
        normalized.adapterCommand.draft = std::move(normalizedDraft);
        normalized.adapterCommand.documentRevision = payload.workingDraft.documentRevision;
        normalized.documentRevision = payload.workingDraft.documentRevision;
        return normalized;
    }
    case MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline:
    {
        if (!status.IsConnected() || !status.world.has_value() ||
            !status.materialInstancePath.has_value() || !activeWorld.has_value() ||
            *status.world != *activeWorld)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewNotConnected,
                "Material instance preview is not connected to the active World.");
            return std::nullopt;
        }
        const auto& payload = std::get<RestoreMaterialInstancePreviewBaselinePayload>(command.payload);
        const MaterialInstancePreviewPathNormalizationResult pathResult =
            NormalizeMaterialInstancePath(payload.materialInstancePath);
        const MaterialInstancePreviewPathNormalizationResult baselinePathResult =
            NormalizeMaterialInstancePath(payload.baselineDraft.materialInstancePath);
        if (!pathResult.Succeeded() || !baselinePathResult.Succeeded())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::InvalidMaterialInstancePath,
                !pathResult.Succeeded() ? pathResult.errorMessage : baselinePathResult.errorMessage);
            return std::nullopt;
        }
        if (*pathResult.path != *baselinePathResult.path ||
            *pathResult.path != *status.materialInstancePath)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::PreviewNotConnected,
                "Preview baseline does not match the connected material instance.");
            return std::nullopt;
        }
        if (payload.baselineDraft.serializedWorkingDraft.empty())
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::ValidationFailed,
                "Preview baseline must contain serialized content.");
            return std::nullopt;
        }
        if (payload.documentRevision != payload.baselineDraft.documentRevision ||
            !command.expectedDocumentRevision.has_value() ||
            *command.expectedDocumentRevision != payload.documentRevision)
        {
            rejection = Reject(
                command,
                MaterialInstancePreviewErrorCode::StaleDocumentRevision,
                "Preview restore requires matching baseline and expected revisions.");
            return std::nullopt;
        }
        MaterialInstancePreviewDraft normalizedBaseline = payload.baselineDraft;
        normalizedBaseline.materialInstancePath = pathResult.path->value;
        normalized.adapterCommand.world = *status.world;
        normalized.adapterCommand.materialInstancePath = *pathResult.path;
        normalized.adapterCommand.bridgeLiveGeneration = status.liveGeneration;
        normalized.adapterCommand.draft = std::move(normalizedBaseline);
        normalized.adapterCommand.documentRevision = payload.documentRevision;
        normalized.documentRevision = payload.documentRevision;
        return normalized;
    }
    }

    rejection = Reject(
        command,
        MaterialInstancePreviewErrorCode::InvalidCommand,
        "Unknown material instance preview command type.");
    return std::nullopt;
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::Execute(
    const MaterialInstancePreviewCommand& command)
{
    try
    {
        MaterialInstancePreviewCommandResult rejection;
        const std::optional<NormalizedCommand> normalized =
            NormalizeCommand(command, rejection);
        if (!normalized.has_value())
        {
            return rejection;
        }
        return ExecuteNormalized(command, *normalized);
    }
    catch (const std::exception& exception)
    {
        MaterialInstancePreviewAdapterResult failure;
        failure.status = PreviewAdapterOperationStatus::Failed;
        failure.failureStage = PreviewAdapterFailureStage::Prepare;
        failure.message = exception.what();
        return CompleteAdapterFailure(command, failure);
    }
    catch (...)
    {
        MaterialInstancePreviewAdapterResult failure;
        failure.status = PreviewAdapterOperationStatus::Failed;
        failure.failureStage = PreviewAdapterFailureStage::Prepare;
        failure.message = "Unknown material instance preview exception.";
        return CompleteAdapterFailure(command, failure);
    }
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::ExecuteNormalized(
    const MaterialInstancePreviewCommand& command,
    NormalizedCommand normalized)
{
    if (command.type == MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview)
    {
        return ExecuteDisconnect(command);
    }

    if (command.type == MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview)
    {
        if (!DisconnectCurrentForReplacement())
        {
            return MakeResult(
                command,
                status.lastError == MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected
                    ? MaterialInstancePreviewCommandStatus::Failed
                    : MaterialInstancePreviewCommandStatus::Rejected,
                status.lastError,
                status.diagnosticMessage,
                normalized.documentRevision);
        }
        BeginConnection(
            normalized.adapterCommand.world,
            normalized.adapterCommand.materialInstancePath,
            normalized.documentRevision);
        normalized.adapterCommand.bridgeLiveGeneration = status.liveGeneration;
    }
    else
    {
        status.state = command.type == MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview
            ? MaterialInstancePreviewState::Applying
            : MaterialInstancePreviewState::RestoringBaseline;
        status.lastError = MaterialInstancePreviewErrorCode::None;
        status.diagnosticMessage.clear();
        TouchStatus();
    }

    const MaterialInstancePreviewAdapterResult adapterResult =
        ExecuteAdapter(normalized.adapterCommand);
    if (adapterResult.status == PreviewAdapterOperationStatus::Pending)
    {
        if (adapterResult.operationId == 0)
        {
            MaterialInstancePreviewAdapterResult invalidResult = adapterResult;
            invalidResult.status = PreviewAdapterOperationStatus::Failed;
            invalidResult.failureStage = PreviewAdapterFailureStage::Prepare;
            invalidResult.message =
                "Preview adapter returned Pending without an operationId.";
            return CompleteAdapterFailure(command, invalidResult);
        }

        pendingOperation = PendingOperation{
            command,
            std::move(normalized.adapterCommand),
            adapterResult.operationId};
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Running,
            MaterialInstancePreviewErrorCode::None,
            AdapterMessage(adapterResult, "Preview operation is running."),
            normalized.documentRevision,
            adapterResult.operationId);
    }

    return CompleteAdapterResult(
        command,
        normalized.adapterCommand,
        adapterResult);
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::ExecuteDisconnect(
    const MaterialInstancePreviewCommand& command)
{
    const bool hadState =
        pendingOperation.has_value() || liveConnectionEstablished ||
        status.materialInstancePath.has_value() ||
        status.state != MaterialInstancePreviewState::Disconnected;
    CancelPendingOperation();
    if (!liveConnectionEstablished)
    {
        if (hadState)
        {
            AdvanceLiveGeneration();
        }
        ClearConnectionState("Material instance preview disconnected.");
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Succeeded,
            MaterialInstancePreviewErrorCode::None,
            "Material instance preview disconnected.");
    }

    MaterialInstancePreviewAdapterResult adapterResult =
        ExecuteAdapter(MakeDisconnectAdapterCommand());
    if (adapterResult.status == PreviewAdapterOperationStatus::Completed)
    {
        liveConnectionEstablished = false;
        AdvanceLiveGeneration();
        ClearConnectionState("Material instance preview disconnected.");
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Succeeded,
            MaterialInstancePreviewErrorCode::None,
            "Material instance preview disconnected.");
    }
    if (adapterResult.status == PreviewAdapterOperationStatus::Pending)
    {
        if (adapterResult.operationId != 0)
        {
            adapter->Cancel(adapterResult.operationId);
        }
        adapterResult.status = PreviewAdapterOperationStatus::Failed;
        adapterResult.failureStage = PreviewAdapterFailureStage::Commit;
        adapterResult.message =
            "Preview adapter cannot disconnect asynchronously without a "
            "disconnecting state.";
    }
    AdvanceLiveGeneration();
    if (adapterResult.status == PreviewAdapterOperationStatus::Unavailable)
    {
        status.lastError = MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected;
        status.state = MaterialInstancePreviewState::Unavailable;
        status.diagnosticMessage = AdapterMessage(
            adapterResult,
            "Material instance preview adapter could not disconnect.");
        TouchStatus();
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Failed,
            MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected,
            status.diagnosticMessage);
    }
    return CompleteAdapterFailure(command, adapterResult);
}

MaterialInstancePreviewAdapterResult
MaterialInstancePreviewController::ExecuteAdapter(
    const MaterialInstancePreviewAdapterCommand& command)
{
    try
    {
        return adapter->Execute(command);
    }
    catch (const std::exception& exception)
    {
        MaterialInstancePreviewAdapterResult result;
        result.status = PreviewAdapterOperationStatus::Failed;
        result.failureStage = PreviewAdapterFailureStage::Prepare;
        result.message = exception.what();
        return result;
    }
    catch (...)
    {
        MaterialInstancePreviewAdapterResult result;
        result.status = PreviewAdapterOperationStatus::Failed;
        result.failureStage = PreviewAdapterFailureStage::Prepare;
        result.message = "Unknown material instance preview adapter exception.";
        return result;
    }
}

MaterialInstancePreviewAdapterCommand
MaterialInstancePreviewController::MakeDisconnectAdapterCommand() const
{
    MaterialInstancePreviewAdapterCommand command;
    command.type = MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview;
    command.bridgeLiveGeneration = status.liveGeneration;
    if (status.world.has_value())
    {
        command.world = *status.world;
    }
    else if (activeWorld.has_value())
    {
        command.world = *activeWorld;
    }
    if (status.materialInstancePath.has_value())
    {
        command.materialInstancePath = *status.materialInstancePath;
    }
    return command;
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::CompleteAdapterResult(
    const MaterialInstancePreviewCommand& command,
    const MaterialInstancePreviewAdapterCommand& adapterCommand,
    const MaterialInstancePreviewAdapterResult& adapterResult)
{
    if (adapterResult.status != PreviewAdapterOperationStatus::Completed)
    {
        return CompleteAdapterFailure(command, adapterResult);
    }
    if (adapterCommand.bridgeLiveGeneration != status.liveGeneration ||
        !activeWorld.has_value() || adapterCommand.world != *activeWorld)
    {
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Rejected,
            MaterialInstancePreviewErrorCode::PreviewGenerationChanged,
            "Preview operation completed for an obsolete World or bridge generation.",
            GetCommandDocumentRevision(command),
            adapterResult.operationId == 0
                ? std::nullopt
                : std::optional<PreviewOperationId>(adapterResult.operationId));
    }

    if (command.type == MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview)
    {
        liveConnectionEstablished = true;
        status.state = MaterialInstancePreviewState::Connected;
        status.runtimeResourceGeneration = adapterResult.runtimeResourceGeneration;
        status.lastError = MaterialInstancePreviewErrorCode::None;
        status.diagnosticMessage = adapterResult.message;
        TouchStatus();
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Succeeded,
            MaterialInstancePreviewErrorCode::None,
            AdapterMessage(adapterResult, "Material instance preview connected."),
            GetCommandDocumentRevision(command),
            adapterResult.operationId == 0
                ? std::nullopt
                : std::optional<PreviewOperationId>(adapterResult.operationId));
    }

    if (command.type == MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview ||
        command.type == MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline)
    {
        // 纹理等资源替换必须明确报告 live commit；数值原地更新不要求
        // 产生新的 resource generation，也不能被误判为资源 swap 失败。
        if (adapterResult.replacesLiveResource &&
            !adapterResult.liveSwapCommitted)
        {
            MaterialInstancePreviewAdapterResult incompleteResult = adapterResult;
            incompleteResult.status = PreviewAdapterOperationStatus::Failed;
            incompleteResult.failureStage = PreviewAdapterFailureStage::Commit;
            incompleteResult.message =
                "Preview adapter completed without committing the live update.";
            return CompleteAdapterFailure(command, incompleteResult);
        }
        if (adapterResult.replacesLiveResource &&
            adapterResult.runtimeResourceGeneration == 0)
        {
            MaterialInstancePreviewAdapterResult incompleteResult = adapterResult;
            incompleteResult.status = PreviewAdapterOperationStatus::Failed;
            incompleteResult.failureStage = PreviewAdapterFailureStage::Commit;
            incompleteResult.message =
                "Preview resource replacement did not provide a resource generation.";
            return CompleteAdapterFailure(command, incompleteResult);
        }
        if (adapterResult.replacesLiveResource)
        {
            AdvanceLiveGeneration();
        }
        if (adapterResult.runtimeResourceGeneration != 0)
        {
            status.runtimeResourceGeneration = adapterResult.runtimeResourceGeneration;
        }
        const std::optional<uint64_t> documentRevision =
            GetCommandDocumentRevision(command);
        status.observedDocumentRevision = documentRevision;
        status.appliedDocumentRevision = documentRevision;
        status.state = MaterialInstancePreviewState::Connected;
        status.lastError = MaterialInstancePreviewErrorCode::None;
        status.diagnosticMessage = adapterResult.message;
        liveConnectionEstablished = true;
        TouchStatus();
        return MakeResult(
            command,
            MaterialInstancePreviewCommandStatus::Succeeded,
            MaterialInstancePreviewErrorCode::None,
            AdapterMessage(adapterResult, "Preview working draft applied."),
            documentRevision,
            adapterResult.operationId == 0
                ? std::nullopt
                : std::optional<PreviewOperationId>(adapterResult.operationId),
            adapterResult.liveSwapCommitted);
    }

    MaterialInstancePreviewAdapterResult invalidResult = adapterResult;
    invalidResult.status = PreviewAdapterOperationStatus::Failed;
    invalidResult.failureStage = PreviewAdapterFailureStage::Commit;
    invalidResult.message = "Preview adapter completed an unknown command type.";
    return CompleteAdapterFailure(command, invalidResult);
}

MaterialInstancePreviewCommandResult
MaterialInstancePreviewController::CompleteAdapterFailure(
    const MaterialInstancePreviewCommand& command,
    const MaterialInstancePreviewAdapterResult& adapterResult)
{
    const MaterialInstancePreviewErrorCode errorCode =
        ResolveAdapterError(adapterResult);
    if (command.type == MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview)
    {
        liveConnectionEstablished = false;
    }
    status.lastError = errorCode;
    status.diagnosticMessage = AdapterMessage(
        adapterResult,
        "Material instance runtime preview operation failed.");
    status.state = adapterResult.status == PreviewAdapterOperationStatus::Unavailable
        ? MaterialInstancePreviewState::Unavailable
        : MaterialInstancePreviewState::Failed;
    TouchStatus();

    const MaterialInstancePreviewCommandStatus commandStatus =
        adapterResult.status == PreviewAdapterOperationStatus::Unavailable
            ? MaterialInstancePreviewCommandStatus::Rejected
            : MaterialInstancePreviewCommandStatus::Failed;
    return MakeResult(
        command,
        commandStatus,
        errorCode,
        status.diagnosticMessage,
        GetCommandDocumentRevision(command),
        adapterResult.operationId == 0
            ? std::nullopt
            : std::optional<PreviewOperationId>(adapterResult.operationId));
}

MaterialInstancePreviewErrorCode
MaterialInstancePreviewController::ResolveAdapterError(
    const MaterialInstancePreviewAdapterResult& adapterResult) const noexcept
{
    if (adapterResult.status == PreviewAdapterOperationStatus::Unavailable)
    {
        return MaterialInstancePreviewErrorCode::PreviewAdapterUnavailable;
    }
    switch (adapterResult.failureStage)
    {
    case PreviewAdapterFailureStage::Prepare:
        return MaterialInstancePreviewErrorCode::PreviewPrepareFailed;
    case PreviewAdapterFailureStage::Commit:
        return MaterialInstancePreviewErrorCode::PreviewCommitFailed;
    case PreviewAdapterFailureStage::None:
        return MaterialInstancePreviewErrorCode::InternalError;
    }
    return MaterialInstancePreviewErrorCode::InternalError;
}

void MaterialInstancePreviewController::BeginConnection(
    const MaterialInstancePreviewWorldIdentity& world,
    const NormalizedMaterialInstancePath& materialInstancePath,
    std::optional<uint64_t> documentRevision)
{
    AdvanceLiveGeneration();
    status.state = MaterialInstancePreviewState::Connecting;
    status.world = world;
    status.materialInstancePath = materialInstancePath;
    status.runtimeResourceGeneration = 0;
    status.observedDocumentRevision = documentRevision;
    status.appliedDocumentRevision.reset();
    status.lastError = MaterialInstancePreviewErrorCode::None;
    status.diagnosticMessage.clear();
    liveConnectionEstablished = false;
    TouchStatus();
}

bool MaterialInstancePreviewController::DisconnectCurrentForReplacement()
{
    CancelPendingOperation();
    if (!liveConnectionEstablished)
    {
        if (status.materialInstancePath.has_value() ||
            status.state != MaterialInstancePreviewState::Disconnected)
        {
            ClearConnectionState("Previous preview connection was discarded.");
        }
        return true;
    }

    MaterialInstancePreviewAdapterResult adapterResult =
        ExecuteAdapter(MakeDisconnectAdapterCommand());
    if (adapterResult.status == PreviewAdapterOperationStatus::Completed)
    {
        liveConnectionEstablished = false;
        AdvanceLiveGeneration();
        ClearConnectionState("Previous preview connection was replaced.");
        return true;
    }
    if (adapterResult.status == PreviewAdapterOperationStatus::Pending)
    {
        if (adapterResult.operationId != 0)
        {
            adapter->Cancel(adapterResult.operationId);
        }
        adapterResult.status = PreviewAdapterOperationStatus::Failed;
        adapterResult.failureStage = PreviewAdapterFailureStage::Commit;
        adapterResult.message =
            "Preview adapter cannot disconnect asynchronously during reconnect.";
    }

    status.lastError = adapterResult.status == PreviewAdapterOperationStatus::Unavailable
        ? MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected
        : ResolveAdapterError(adapterResult);
    status.diagnosticMessage = AdapterMessage(
        adapterResult,
        "Previous material instance preview connection could not be released.");
    status.state = adapterResult.status == PreviewAdapterOperationStatus::Unavailable
        ? MaterialInstancePreviewState::Unavailable
        : MaterialInstancePreviewState::Failed;
    TouchStatus();
    return false;
}

void MaterialInstancePreviewController::ClearConnectionState(
    std::string diagnosticMessage)
{
    status.state = activeWorld.has_value()
        ? MaterialInstancePreviewState::Disconnected
        : MaterialInstancePreviewState::Unavailable;
    status.world = activeWorld;
    status.materialInstancePath.reset();
    status.runtimeResourceGeneration = 0;
    status.observedDocumentRevision.reset();
    status.appliedDocumentRevision.reset();
    status.lastError = MaterialInstancePreviewErrorCode::None;
    status.diagnosticMessage = std::move(diagnosticMessage);
    liveConnectionEstablished = false;
    TouchStatus();
}

void MaterialInstancePreviewController::AdvanceLiveGeneration() noexcept
{
    ++status.liveGeneration;
    if (status.liveGeneration == 0)
    {
        status.liveGeneration = 1;
    }
}

void MaterialInstancePreviewController::TouchStatus() noexcept
{
    ++status.statusRevision;
    if (status.statusRevision == 0)
    {
        status.statusRevision = 1;
    }
}

void MaterialInstancePreviewController::CancelPendingOperation() noexcept
{
    if (!pendingOperation.has_value())
    {
        return;
    }
    adapter->Cancel(pendingOperation->operationId);
    pendingOperation.reset();
}

std::optional<MaterialInstancePreviewCommandResult>
MaterialInstancePreviewController::Poll()
{
    if (!pendingOperation.has_value())
    {
        return std::nullopt;
    }

    PendingOperation operation = std::move(*pendingOperation);
    pendingOperation.reset();

    MaterialInstancePreviewAdapterResult adapterResult;
    try
    {
        adapterResult = adapter->Poll(operation.operationId);
    }
    catch (const std::exception& exception)
    {
        adapterResult.status = PreviewAdapterOperationStatus::Failed;
        adapterResult.failureStage = PreviewAdapterFailureStage::Commit;
        adapterResult.message = exception.what();
    }
    catch (...)
    {
        adapterResult.status = PreviewAdapterOperationStatus::Failed;
        adapterResult.failureStage = PreviewAdapterFailureStage::Commit;
        adapterResult.message =
            "Unknown material instance preview adapter poll exception.";
    }

    if (adapterResult.operationId != 0 &&
        adapterResult.operationId != operation.operationId)
    {
        adapterResult.status = PreviewAdapterOperationStatus::Failed;
        adapterResult.failureStage = PreviewAdapterFailureStage::Commit;
        adapterResult.message =
            "Preview adapter returned a mismatched operationId.";
    }

    if (adapterResult.status == PreviewAdapterOperationStatus::Pending)
    {
        if (adapterResult.operationId == 0)
        {
            adapterResult.status = PreviewAdapterOperationStatus::Failed;
            adapterResult.failureStage = PreviewAdapterFailureStage::Commit;
            adapterResult.message =
                "Preview adapter returned Pending without an operationId.";
            return CompleteAdapterFailure(operation.command, adapterResult);
        }
        if (operation.adapterCommand.bridgeLiveGeneration != status.liveGeneration ||
            !activeWorld.has_value() ||
            operation.adapterCommand.world != *activeWorld)
        {
            adapter->Cancel(operation.operationId);
            return MakeResult(
                operation.command,
                MaterialInstancePreviewCommandStatus::Rejected,
                MaterialInstancePreviewErrorCode::PreviewGenerationChanged,
                "Pending preview operation belongs to an obsolete generation.",
                GetCommandDocumentRevision(operation.command),
                operation.operationId);
        }
        pendingOperation = std::move(operation);
        return MakeResult(
            pendingOperation->command,
            MaterialInstancePreviewCommandStatus::Running,
            MaterialInstancePreviewErrorCode::None,
            AdapterMessage(adapterResult, "Preview operation is still running."),
            GetCommandDocumentRevision(pendingOperation->command),
            pendingOperation->operationId);
    }

    if (operation.adapterCommand.bridgeLiveGeneration != status.liveGeneration ||
        !activeWorld.has_value() ||
        operation.adapterCommand.world != *activeWorld)
    {
        return MakeResult(
            operation.command,
            MaterialInstancePreviewCommandStatus::Rejected,
            MaterialInstancePreviewErrorCode::PreviewGenerationChanged,
            "Preview operation completed for an obsolete generation.",
            GetCommandDocumentRevision(operation.command),
            operation.operationId);
    }

    return CompleteAdapterResult(
        operation.command,
        operation.adapterCommand,
        adapterResult);
}

} // namespace VL::Editor::Preview
