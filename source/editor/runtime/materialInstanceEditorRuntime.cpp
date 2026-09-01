#include "editor/runtime/materialInstanceEditorRuntime.h"

#include "editor/service/materialInstanceDocumentService.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace VL::Editor
{
namespace
{

constexpr const char* RuntimeUnavailableMessage =
    "Material Instance editor service is unavailable; no business operation was executed.";
constexpr const char* PreviewUnavailableMessage =
    "Runtime preview bridge is not connected; asset editing and saving remain available.";

EditorCommandResult MakeResult(
    const EditorCommandEnvelope& command,
    EditorCommandStatus status,
    EditorErrorCode errorCode,
    std::string message)
{
    EditorCommandResult result;
    result.commandId = command.commandId;
    result.status = status;
    result.errorCode = errorCode;
    result.message = std::move(message);
    return result;
}

std::string FormatServiceMessage(const MaterialEditorServiceResult& serviceResult)
{
    if (serviceResult.succeeded)
    {
        return serviceResult.message;
    }

    std::string message = serviceResult.message;
    if (!message.empty())
    {
        message += " ";
    }
    message += "[";
    message += GetEditorErrorCodeName(serviceResult.errorCode);
    message += "]";
    return message;
}

Preview::PreviewCommandSource ToPreviewCommandSource(
    EditorCommandSource source) noexcept
{
    switch (source)
    {
    case EditorCommandSource::ImGui:
        return Preview::PreviewCommandSource::ImGui;
    case EditorCommandSource::Console:
        return Preview::PreviewCommandSource::Console;
    case EditorCommandSource::AI:
        return Preview::PreviewCommandSource::AI;
    case EditorCommandSource::RuntimeTest:
        return Preview::PreviewCommandSource::RuntimeTest;
    }
    return Preview::PreviewCommandSource::Unknown;
}

bool IsPreviewCommand(EditorCommandType type) noexcept
{
    return type == EditorCommandType::ConnectMaterialInstancePreview ||
        type == EditorCommandType::DisconnectMaterialInstancePreview ||
        type == EditorCommandType::ApplyMaterialInstancePreview ||
        type == EditorCommandType::RestoreMaterialInstancePreviewBaseline;
}

EditorErrorCode ToEditorPreviewError(
    Preview::MaterialInstancePreviewErrorCode errorCode) noexcept
{
    switch (errorCode)
    {
    case Preview::MaterialInstancePreviewErrorCode::None:
        return EditorErrorCode::None;
    case Preview::MaterialInstancePreviewErrorCode::PreviewGenerationChanged:
        return EditorErrorCode::PreviewGenerationChanged;
    case Preview::MaterialInstancePreviewErrorCode::StaleDocumentRevision:
        return EditorErrorCode::StaleDocumentRevision;
    case Preview::MaterialInstancePreviewErrorCode::PreviewPrepareFailed:
        return EditorErrorCode::PreviewPrepareFailed;
    case Preview::MaterialInstancePreviewErrorCode::PreviewCommitFailed:
        return EditorErrorCode::PreviewCommitFailed;
    case Preview::MaterialInstancePreviewErrorCode::PreviewAdapterUnavailable:
    case Preview::MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected:
    case Preview::MaterialInstancePreviewErrorCode::PreviewUnavailable:
    case Preview::MaterialInstancePreviewErrorCode::PreviewNotConnected:
        return EditorErrorCode::PreviewUnavailable;
    case Preview::MaterialInstancePreviewErrorCode::InvalidMaterialInstancePath:
    case Preview::MaterialInstancePreviewErrorCode::ValidationFailed:
        return EditorErrorCode::ValidationFailed;
    case Preview::MaterialInstancePreviewErrorCode::InvalidProtocolVersion:
    case Preview::MaterialInstancePreviewErrorCode::InvalidCommand:
    case Preview::MaterialInstancePreviewErrorCode::PreviewOperationInProgress:
    case Preview::MaterialInstancePreviewErrorCode::PreviewOperationNotFound:
    case Preview::MaterialInstancePreviewErrorCode::PreviewOperationCanceled:
    case Preview::MaterialInstancePreviewErrorCode::CommandIdConflict:
    case Preview::MaterialInstancePreviewErrorCode::ResultStoreFull:
    case Preview::MaterialInstancePreviewErrorCode::InternalError:
        return EditorErrorCode::ValidationFailed;
    }
    return EditorErrorCode::ValidationFailed;
}

EditorUi::EditorPreviewStatus ToEditorPreviewStatus(
    Preview::MaterialInstancePreviewState state) noexcept
{
    switch (state)
    {
    case Preview::MaterialInstancePreviewState::Connected:
        return EditorUi::EditorPreviewStatus::Connected;
    case Preview::MaterialInstancePreviewState::Connecting:
    case Preview::MaterialInstancePreviewState::Applying:
    case Preview::MaterialInstancePreviewState::RestoringBaseline:
        return EditorUi::EditorPreviewStatus::Applying;
    case Preview::MaterialInstancePreviewState::Failed:
        return EditorUi::EditorPreviewStatus::Failed;
    case Preview::MaterialInstancePreviewState::Unavailable:
        return EditorUi::EditorPreviewStatus::Unavailable;
    case Preview::MaterialInstancePreviewState::Disconnected:
        return EditorUi::EditorPreviewStatus::Disconnected;
    }
    return EditorUi::EditorPreviewStatus::Unavailable;
}

} // namespace

MaterialInstanceEditorRuntime::~MaterialInstanceEditorRuntime()
{
    Shutdown();
}

RuntimeResult<void> MaterialInstanceEditorRuntime::Initialize(Config configValue)
{
    Shutdown();
    config = std::move(configValue);

    try
    {
        if (config.previewAdapter != nullptr)
        {
            previewController =
                std::make_unique<Preview::MaterialInstancePreviewController>(
                    *config.previewAdapter);
        }
        else
        {
            previewController =
                std::make_unique<Preview::MaterialInstancePreviewController>();
        }
        commandBus = std::make_unique<EditorCommandBus>(config.maxResultCount);
        if (config.resourceRoot.empty())
        {
            service = std::make_unique<MaterialInstanceDocumentService>();
        }
        else
        {
            service = std::make_unique<MaterialInstanceDocumentService>(
                MaterialInstanceDocumentService::Config{
                    config.resourceRoot,
                    config.projectRoot,
                    true});
        }
        initialized = true;
        diagnostic.clear();
        snapshot = EditorUi::MaterialInstanceEditorSnapshot{};
        snapshot.statusMessage = "Loading material instance assets...";

        // 初始资产列表也走同一条命令链，避免初始化阶段产生旁路 service 调用。
        EditorCommandEnvelope listCommand;
        listCommand.source = EditorCommandSource::ImGui;
        listCommand.type = EditorCommandType::ListMaterialInstanceAssets;
        listCommand.payload = ListMaterialInstanceAssetsPayload{};
        SubmitInternal(std::move(listCommand));
        Tick();

        if (!std::filesystem::is_directory(service->GetResourceRoot()))
        {
            diagnostic =
                "Material Instance resource root does not exist: " +
                service->GetResourceRoot().string();
            SetStatus(diagnostic);
        }
        return RuntimeResult<void>::Success();
    }
    catch (const std::exception& exception)
    {
        initialized = false;
        service.reset();
        diagnostic = exception.what();
        snapshot = EditorUi::MaterialInstanceEditorSnapshot{};
        snapshot.statusMessage = diagnostic;
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "MaterialInstanceEditorRuntime.InitializeFailed",
            diagnostic));
    }
}

void MaterialInstanceEditorRuntime::Shutdown() noexcept
{
    if (previewController != nullptr)
    {
        previewController->Shutdown();
    }
    pendingPreviewCommand.reset();
    queuedAutomaticPreviewDisconnectAssetPath.reset();
    queuedAutomaticPreviewConnectionAssetPath.reset();
    queuedAutomaticPreviewApplyAssetPath.reset();
    service.reset();
    commandBus.reset();
    previewController.reset();
    snapshot = EditorUi::MaterialInstanceEditorSnapshot{};
    config = Config{};
    worldPath.clear();
    worldGeneration = 0;
    diagnostic.clear();
    lastCommandMessage.clear();
    initialized = false;
}

void MaterialInstanceEditorRuntime::Tick()
{
    if (!initialized)
    {
        return;
    }

    if (commandBus == nullptr)
    {
        return;
    }

    PollPreview();

    // 自动预览命令可能由本轮参数编辑追加到 bus；持续 drain，保证一次
    // 参数提交在同一个 UI/game tick 内完成 working draft -> live swap。
    while (true)
    {
        const std::vector<EditorCommandEnvelope> commands = commandBus->Drain();
        for (const EditorCommandEnvelope& command : commands)
        {
            EditorCommandResult result;
            try
            {
                result = ExecuteCommand(command);
            }
            catch (const std::exception& exception)
            {
                result = MakeDiagnosticResult(
                    command,
                    EditorCommandStatus::Failed,
                    EditorErrorCode::ValidationFailed,
                    exception.what());
            }

            if (result.status != EditorCommandStatus::Succeeded &&
                result.status != EditorCommandStatus::Running)
            {
                diagnostic = result.message;
                SetStatus(result.message);
            }

            if (result.status == EditorCommandStatus::Running)
            {
                // EditorCommandBus 已将该命令标记为 Running；终态由下一帧
                // PreviewController::Poll() 发布，避免把异步预览伪装成成功。
                continue;
            }

            if (!commandBus->PublishResult(std::move(result)))
            {
                diagnostic =
                    "Material Instance editor could not publish command result.";
            }
        }

        // 先消费 UI/外部已经排队的命令，再追加自动预览命令，避免关闭
        // 页签时自动重连抢在 close 前执行并访问已关闭的文档。
        SubmitQueuedAutomaticPreviewDisconnect();
        SubmitQueuedAutomaticPreviewConnection();
        SubmitQueuedAutomaticPreviewApply();
        if (commandBus->PendingCount() == 0)
        {
            break;
        }
    }

    PollPreview();
    ApplyRuntimeOverlay();
}

void MaterialInstanceEditorRuntime::NotifyWorldChanged(
    std::string_view path,
    uint64_t generation)
{
    const std::string nextPath(path);
    if (nextPath == worldPath && generation == worldGeneration)
    {
        return;
    }

    RejectPendingPreviewForWorldChange();
    worldPath = nextPath;
    worldGeneration = worldPath.empty() ? 0 : generation;
    if (worldPath.empty() || worldGeneration == 0)
    {
        previewController->ClearActiveWorld();
    }
    else
    {
        previewController->NotifyActiveWorldChanged(
            Preview::MaterialInstancePreviewWorldIdentity{
                worldGeneration,
                worldPath});
    }
    QueueAutomaticPreviewConnection();
    ApplyRuntimeOverlay();
    if (worldPath.empty())
    {
        SetStatus("World changed; " + std::string(PreviewUnavailableMessage));
    }
    else
    {
        SetStatus(
            "World changed to '" + worldPath + "'; " +
            PreviewUnavailableMessage);
    }
}

void MaterialInstanceEditorRuntime::Submit(EditorCommandEnvelope command)
{
    SubmitInternal(std::move(command));
}

bool MaterialInstanceEditorRuntime::SubmitSaveActiveDocument()
{
    if (!snapshot.activeDocument.has_value())
    {
        SetStatus("Save requested, but no Material Instance document is open.");
        return false;
    }

    EditorCommandEnvelope command;
    command.source = EditorCommandSource::ImGui;
    command.type = EditorCommandType::SaveMaterialInstanceDocument;
    command.expectedDocumentRevision = snapshot.activeDocument->revision;
    command.payload = MaterialInstanceAssetPathPayload{
        snapshot.activeDocument->assetPath};
    return SubmitInternal(std::move(command)).admission ==
        EditorCommandAdmission::Queued;
}

EditorCommandSubmission MaterialInstanceEditorRuntime::SubmitInternal(
    EditorCommandEnvelope command)
{
    if (commandBus == nullptr)
    {
        return EditorCommandSubmission{
            0,
            EditorCommandAdmission::Rejected,
            MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::ValidationFailed,
                RuntimeUnavailableMessage)};
    }

    const EditorCommandSubmission submission = commandBus->Submit(
        std::move(command));
    if (submission.admission != EditorCommandAdmission::Queued)
    {
        SetStatus(submission.result.message);
    }
    return submission;
}

EditorCommandSubmission MaterialInstanceEditorRuntime::SubmitAutomaticPreviewCommand(
    EditorCommandEnvelope command)
{
    if (commandBus == nullptr)
    {
        return EditorCommandSubmission{
            0,
            EditorCommandAdmission::Rejected,
            MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::ValidationFailed,
                RuntimeUnavailableMessage)};
    }

    const EditorCommandSubmission submission = commandBus->SubmitInternal(
        std::move(command));
    if (submission.admission != EditorCommandAdmission::Queued)
    {
        SetStatus(submission.result.message);
    }
    return submission;
}

EditorCommandResult MaterialInstanceEditorRuntime::ExecuteCommand(
    const EditorCommandEnvelope& command)
{
    if (service == nullptr)
    {
        return MakeDiagnosticResult(
            command,
            EditorCommandStatus::Failed,
            EditorErrorCode::ValidationFailed,
            RuntimeUnavailableMessage);
    }

    if (command.expectedDocumentRevision.has_value())
    {
        const std::optional<std::string> assetPath =
            GetEditorCommandAssetPath(command);
        if (assetPath.has_value())
        {
            const std::optional<EditorDocumentRevision> actualRevision =
                service->GetDocumentRevision(*assetPath);
            if (actualRevision.has_value() &&
                actualRevision.value() !=
                    command.expectedDocumentRevision.value())
            {
                return MakeDiagnosticResult(
                    command,
                    EditorCommandStatus::Rejected,
                    EditorErrorCode::StaleDocumentRevision,
                    "expected document revision does not match the open document");
            }
        }
    }

    switch (command.type)
    {
    case EditorCommandType::ListMaterialInstanceAssets:
    {
        const auto& payload = std::get<ListMaterialInstanceAssetsPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ListMaterialInstanceAssets(
                payload.searchText,
                payload.pageIndex,
                payload.pageSize));
    }
    case EditorCommandType::OpenMaterialInstanceAsset:
    {
        const auto& payload = std::get<OpenMaterialInstanceAssetPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->OpenMaterialInstanceAsset(
                payload.assetPath,
                payload.origin));
    }
    case EditorCommandType::SelectMaterialInstanceDocument:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->SelectMaterialInstanceDocument(payload.assetPath));
    }
    case EditorCommandType::CloseMaterialInstanceAsset:
    {
        const auto& payload = std::get<CloseMaterialInstanceAssetPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->CloseMaterialInstanceAsset(
                payload.assetPath,
                payload.dirtyPolicy));
    }
    case EditorCommandType::GetMaterialInstanceDocument:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->GetMaterialInstanceDocument(payload.assetPath));
    }
    case EditorCommandType::GetMaterialInstanceReferenceContext:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->GetMaterialInstanceReferenceContext(payload.assetPath));
    }
    case EditorCommandType::SetMaterialParameterOverride:
    {
        const auto& payload = std::get<SetMaterialParameterOverridePayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->SetMaterialParameterOverride(
                payload.assetPath,
                payload.parameter,
                payload.parameterType,
                payload.value));
    }
    case EditorCommandType::ClearMaterialParameterOverride:
    {
        const auto& payload = std::get<ClearMaterialParameterOverridePayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ClearMaterialParameterOverride(
                payload.assetPath,
                payload.parameter));
    }
    case EditorCommandType::SetMaterialTextureOverride:
    {
        const auto& payload = std::get<SetMaterialTextureOverridePayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->SetMaterialTextureOverride(
                payload.assetPath,
                payload.slot,
                payload.textureAssetPath));
    }
    case EditorCommandType::ClearMaterialTextureOverride:
    {
        const auto& payload = std::get<ClearMaterialTextureOverridePayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ClearMaterialTextureOverride(
                payload.assetPath,
                payload.slot));
    }
    case EditorCommandType::ResetMaterialInstanceOverrides:
    {
        const auto& payload = std::get<ResetMaterialInstanceOverridesPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ResetMaterialInstanceOverrides(
                payload.assetPath,
                payload.scope));
    }
    case EditorCommandType::RevertMaterialInstanceDocument:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->RevertMaterialInstanceDocument(payload.assetPath));
    }
    case EditorCommandType::ReloadMaterialInstanceDocument:
    {
        const auto& payload = std::get<ReloadMaterialInstanceDocumentPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ReloadMaterialInstanceDocument(
                payload.assetPath,
                payload.dirtyPolicy));
    }
    case EditorCommandType::ValidateMaterialInstanceDocument:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ValidateMaterialInstanceDocument(payload.assetPath));
    }
    case EditorCommandType::SaveMaterialInstanceDocument:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->SaveMaterialInstanceDocument(payload.assetPath));
    }
    case EditorCommandType::OpenTextureAsset:
        return MakeUnsupportedResult(
            command,
            EditorErrorCode::InvalidAssetType,
            "Texture Asset Editor is outside the Material Instance editor scope; "
            "no texture document was opened.");
    case EditorCommandType::ResolveSceneMaterialAsset:
    {
        const auto& payload = std::get<ResolveSceneMaterialAssetPayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ResolveSceneMaterialAsset(payload));
    }
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::ApplyMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
        return ExecutePreviewCommand(command);
    case EditorCommandType::GetEditorCommandResult:
    {
        const auto& payload = std::get<GetEditorCommandResultPayload>(
            command.payload);
        const std::optional<EditorCommandResult> queried = commandBus->GetResult(
            payload.commandId);
        if (!queried.has_value())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Failed,
                EditorErrorCode::AssetNotFound,
                "editor command result was not found");
        }
        return MakeDiagnosticResult(
            command,
            EditorCommandStatus::Succeeded,
            EditorErrorCode::None,
            "command #" + std::to_string(payload.commandId) + " is " +
                std::string(GetEditorCommandStatusName(queried->status)) +
                ": " + queried->message);
    }
    case EditorCommandType::ListEditorEvents:
        return MakeUnsupportedResult(
            command,
            EditorErrorCode::ValidationFailed,
            "Editor event stream is not connected to this runtime facade.");
    case EditorCommandType::ExecuteEditorCommandBatch:
    {
        const auto& payload = std::get<ExecuteEditorCommandBatchPayload>(
            command.payload);
        MaterialEditorServiceResult serviceResult = service->ExecuteBatch(
            payload,
            command.expectedDocumentRevision);
        EditorCommandResult result = FinishServiceResult(
            command,
            std::move(serviceResult));
        if (result.status == EditorCommandStatus::Succeeded)
        {
            result.payload = EditorBatchResultPayload{
                static_cast<uint32_t>(payload.commands.size()),
                result.documentRevision.value_or(0)};
        }
        return result;
    }
    }

    return MakeDiagnosticResult(
        command,
        EditorCommandStatus::Rejected,
        EditorErrorCode::InvalidCommandType,
        "unsupported editor command type");
}

EditorCommandResult MaterialInstanceEditorRuntime::FinishServiceResult(
    const EditorCommandEnvelope& command,
    MaterialEditorServiceResult serviceResult)
{
    ApplyServiceResult(serviceResult);

    if (serviceResult.succeeded)
    {
        QueueAutomaticPreviewConnection();
    }

    if (serviceResult.succeeded && serviceResult.document.has_value() &&
        (command.type == EditorCommandType::SetMaterialParameterOverride ||
         command.type == EditorCommandType::ClearMaterialParameterOverride))
    {
        QueueAutomaticPreviewApply(serviceResult.document->assetPath);
    }

    EditorCommandResult result = MakeResult(
        command,
        serviceResult.succeeded
            ? EditorCommandStatus::Succeeded
            : EditorCommandStatus::Failed,
        serviceResult.errorCode,
        FormatServiceMessage(serviceResult));
    if (serviceResult.documentRevision.has_value())
    {
        result.documentRevision = serviceResult.documentRevision;
    }
    else if (serviceResult.document.has_value())
    {
        result.documentRevision = serviceResult.document->revision;
    }

    if (serviceResult.document.has_value())
    {
        result.payload = EditorDocumentResultPayload{
            serviceResult.document->revision,
            serviceResult.document->state != MaterialEditorDocumentState::Clean};
    }
    else if (serviceResult.editor.has_value() &&
        serviceResult.editor->activeDocument.has_value())
    {
        const auto& document = serviceResult.editor->activeDocument.value();
        result.documentRevision = document.revision;
        result.payload = EditorDocumentResultPayload{
            document.revision,
            document.state != MaterialEditorDocumentState::Clean};
    }
    return result;
}

EditorCommandResult MaterialInstanceEditorRuntime::MakeDiagnosticResult(
    const EditorCommandEnvelope& command,
    EditorCommandStatus status,
    EditorErrorCode errorCode,
    std::string message) const
{
    return MakeResult(command, status, errorCode, std::move(message));
}

EditorCommandResult MaterialInstanceEditorRuntime::MakeUnsupportedResult(
    const EditorCommandEnvelope& command,
    EditorErrorCode errorCode,
    std::string message) const
{
    return MakeDiagnosticResult(
        command,
        EditorCommandStatus::Rejected,
        errorCode,
        std::move(message));
}

EditorCommandResult MaterialInstanceEditorRuntime::ExecutePreviewCommand(
    const EditorCommandEnvelope& command)
{
    Preview::MaterialInstancePreviewCommand previewCommand;
    previewCommand.commandId = command.commandId;
    previewCommand.correlationId = command.correlationId;
    previewCommand.source = ToPreviewCommandSource(command.source);
    previewCommand.expectedPreviewGeneration =
        previewController->GetStatus().liveGeneration;

    switch (command.type)
    {
    case EditorCommandType::DisconnectMaterialInstancePreview:
        previewCommand.type =
            Preview::MaterialInstancePreviewCommandType::
                DisconnectMaterialInstancePreview;
        previewCommand.payload = Preview::DisconnectMaterialInstancePreviewPayload{};
        break;
    case EditorCommandType::ConnectMaterialInstancePreview:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        if (worldPath.empty() || worldGeneration == 0)
        {
            return MakeUnsupportedResult(
                command,
                EditorErrorCode::PreviewUnavailable,
                "No active World is available for material instance preview.");
        }

        const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
            Preview::NormalizeMaterialInstancePath(payload.assetPath);
        if (!pathResult.Succeeded())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::ValidationFailed,
                pathResult.errorMessage);
        }
        if (!service->IsDocumentOpen(pathResult.path->value))
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::DocumentNotOpen,
                "Open the material instance document before connecting preview.");
        }
        const std::optional<MaterialEditorDocumentSnapshot> document =
            service->BuildDocumentSnapshot(pathResult.path->value);
        if (!document.has_value())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Failed,
                EditorErrorCode::ValidationFailed,
                "Material instance preview could not read the open document.");
        }
        if (command.expectedDocumentRevision.has_value() &&
            *command.expectedDocumentRevision != document->revision)
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::StaleDocumentRevision,
                "material instance preview connect revision is stale");
        }

        previewCommand.type =
            Preview::MaterialInstancePreviewCommandType::
                ConnectMaterialInstancePreview;
        previewCommand.expectedDocumentRevision =
            command.expectedDocumentRevision;
        previewCommand.payload = Preview::ConnectMaterialInstancePreviewPayload{
            Preview::MaterialInstancePreviewWorldIdentity{
                worldGeneration,
                worldPath},
            pathResult.path->value,
            document->revision};
        break;
    }
    case EditorCommandType::ApplyMaterialInstancePreview:
    {
        const auto& payload = std::get<ApplyMaterialInstancePreviewPayload>(
            command.payload);
        const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
            Preview::NormalizeMaterialInstancePath(payload.assetPath);
        if (!pathResult.Succeeded())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::ValidationFailed,
                pathResult.errorMessage);
        }
        if (!service->IsDocumentOpen(pathResult.path->value))
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::DocumentNotOpen,
                "Open the material instance document before applying preview.");
        }
        const std::optional<MaterialEditorDocumentSnapshot> document =
            service->BuildDocumentSnapshot(pathResult.path->value);
        if (!document.has_value())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Failed,
                EditorErrorCode::ValidationFailed,
                "Material instance preview could not read the open document.");
        }
        if (payload.documentRevision != document->revision)
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::StaleDocumentRevision,
                "material instance preview apply revision is stale");
        }
        if (document->serializedWorkingDraft.empty())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::ValidationFailed,
                "Material instance preview requires a serialized working draft.");
        }

        Preview::MaterialInstancePreviewDraft draft;
        draft.materialInstancePath = pathResult.path->value;
        draft.documentRevision = document->revision;
        draft.serializedWorkingDraft = document->serializedWorkingDraft;
        previewCommand.type =
            Preview::MaterialInstancePreviewCommandType::
                ApplyMaterialInstancePreview;
        previewCommand.expectedDocumentRevision = document->revision;
        previewCommand.payload = Preview::ApplyMaterialInstancePreviewPayload{
            std::move(draft)};
        break;
    }
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
    {
        const auto& payload = std::get<MaterialInstanceAssetPathPayload>(
            command.payload);
        const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
            Preview::NormalizeMaterialInstancePath(payload.assetPath);
        if (!pathResult.Succeeded())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::ValidationFailed,
                pathResult.errorMessage);
        }
        if (!service->IsDocumentOpen(pathResult.path->value))
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::DocumentNotOpen,
                "Open the material instance document before restoring preview baseline.");
        }
        const std::optional<MaterialEditorDocumentSnapshot> document =
            service->BuildDocumentSnapshot(pathResult.path->value);
        if (!document.has_value())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Failed,
                EditorErrorCode::ValidationFailed,
                "Material instance preview could not read the open document.");
        }
        if (command.expectedDocumentRevision.has_value() &&
            *command.expectedDocumentRevision != document->revision)
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Rejected,
                EditorErrorCode::StaleDocumentRevision,
                "material instance preview restore revision is stale");
        }

        const std::optional<Preview::MaterialInstancePreviewDraft> baselineDraft =
            BuildBaselinePreviewDraft(pathResult.path->value);
        if (!baselineDraft.has_value())
        {
            return MakeDiagnosticResult(
                command,
                EditorCommandStatus::Failed,
                EditorErrorCode::AssetNotFound,
                "Material instance preview could not read the baseline asset.");
        }

        previewCommand.type =
            Preview::MaterialInstancePreviewCommandType::
                RestoreMaterialInstancePreviewBaseline;
        previewCommand.expectedDocumentRevision = document->revision;
        previewCommand.payload =
            Preview::RestoreMaterialInstancePreviewBaselinePayload{
                pathResult.path->value,
                document->revision,
                *baselineDraft};
        break;
    }
    default:
        return MakeDiagnosticResult(
            command,
            EditorCommandStatus::Rejected,
            EditorErrorCode::InvalidCommandType,
            "unsupported material instance preview command type");
    }

    const Preview::MaterialInstancePreviewCommandResult previewResult =
        previewController->Execute(previewCommand);
    if (previewResult.status ==
        Preview::MaterialInstancePreviewCommandStatus::Running)
    {
        pendingPreviewCommand = command;
    }
    return ConvertPreviewResult(command, previewResult);
}

EditorCommandResult MaterialInstanceEditorRuntime::ConvertPreviewResult(
    const EditorCommandEnvelope& command,
    const Preview::MaterialInstancePreviewCommandResult& previewResult) const
{
    EditorCommandStatus statusValue = EditorCommandStatus::Failed;
    switch (previewResult.status)
    {
    case Preview::MaterialInstancePreviewCommandStatus::Accepted:
        statusValue = EditorCommandStatus::Accepted;
        break;
    case Preview::MaterialInstancePreviewCommandStatus::Running:
        statusValue = EditorCommandStatus::Running;
        break;
    case Preview::MaterialInstancePreviewCommandStatus::Succeeded:
        statusValue = EditorCommandStatus::Succeeded;
        break;
    case Preview::MaterialInstancePreviewCommandStatus::Rejected:
        statusValue = EditorCommandStatus::Rejected;
        break;
    case Preview::MaterialInstancePreviewCommandStatus::Failed:
        statusValue = EditorCommandStatus::Failed;
        break;
    }

    EditorCommandResult result = MakeDiagnosticResult(
        command,
        statusValue,
        ToEditorPreviewError(previewResult.errorCode),
        previewResult.message);
    result.documentRevision = previewResult.documentRevision;
    if (previewResult.documentRevision.has_value())
    {
        const bool dirty = snapshot.activeDocument.has_value() &&
            snapshot.activeDocument->revision ==
                previewResult.documentRevision.value() &&
            snapshot.activeDocument->status != EditorUi::EditorDocumentStatus::Clean;
        result.payload = EditorDocumentResultPayload{
            previewResult.documentRevision.value(),
            dirty};
    }
    return result;
}

std::optional<Preview::MaterialInstancePreviewDraft>
MaterialInstanceEditorRuntime::BuildBaselinePreviewDraft(
    std::string_view assetPath) const
{
    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(assetPath);
    if (!pathResult.Succeeded() || config.resourceRoot.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path absolutePath =
        config.resourceRoot / std::filesystem::path(pathResult.path->value);
    std::ifstream input(absolutePath);
    if (!input.is_open())
    {
        return std::nullopt;
    }

    try
    {
        nlohmann::json sourceJson;
        input >> sourceJson;
        Preview::MaterialInstancePreviewDraft draft;
        draft.materialInstancePath = pathResult.path->value;
        draft.documentRevision =
            service->GetDocumentRevision(pathResult.path->value).value_or(0);
        draft.serializedWorkingDraft = sourceJson.dump();
        return draft;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

void MaterialInstanceEditorRuntime::PollPreview()
{
    if (pendingPreviewCommand.has_value() == false || commandBus == nullptr)
    {
        return;
    }

    if (!previewController->HasPendingOperation())
    {
        EditorCommandResult result = MakeDiagnosticResult(
            pendingPreviewCommand.value(),
            EditorCommandStatus::Failed,
            EditorErrorCode::PreviewCommitFailed,
            "Material instance preview operation disappeared before completion.");
        commandBus->PublishResult(std::move(result));
        pendingPreviewCommand.reset();
        return;
    }

    const std::optional<Preview::MaterialInstancePreviewCommandResult> previewResult =
        previewController->Poll();
    if (!previewResult.has_value() ||
        previewResult->status ==
            Preview::MaterialInstancePreviewCommandStatus::Running)
    {
        return;
    }

    EditorCommandResult result = ConvertPreviewResult(
        pendingPreviewCommand.value(),
        previewResult.value());
    if (!commandBus->PublishResult(std::move(result)))
    {
        diagnostic = "Material instance editor could not publish preview result.";
    }
    pendingPreviewCommand.reset();
    ApplyRuntimeOverlay();
}

void MaterialInstanceEditorRuntime::RejectPendingPreviewForWorldChange()
{
    if (!pendingPreviewCommand.has_value())
    {
        return;
    }

    if (commandBus != nullptr)
    {
        EditorCommandResult result = MakeDiagnosticResult(
            pendingPreviewCommand.value(),
            EditorCommandStatus::Rejected,
            EditorErrorCode::PreviewGenerationChanged,
            "Active World changed before the preview operation completed.");
        commandBus->PublishResult(std::move(result));
    }
    pendingPreviewCommand.reset();
}

void MaterialInstanceEditorRuntime::QueueAutomaticPreviewConnection()
{
    if (previewController == nullptr)
    {
        return;
    }

    if (worldPath.empty() || worldGeneration == 0 ||
        !snapshot.activeDocument.has_value())
    {
        // 关闭最后一个页签后仍可能保留旧连接，必须在命令边界主动释放，
        // 不能只依赖“没有 active document”而静默跳过自动预览流程。
        queuedAutomaticPreviewConnectionAssetPath.reset();
        queuedAutomaticPreviewApplyAssetPath.reset();
        const Preview::MaterialInstancePreviewStatus previewStatus =
            previewController->GetStatus();
        if (previewController->HasPendingOperation() ||
            previewStatus.materialInstancePath.has_value() ||
            previewStatus.state != Preview::MaterialInstancePreviewState::Disconnected)
        {
            queuedAutomaticPreviewDisconnectAssetPath =
                previewStatus.materialInstancePath.has_value()
                ? previewStatus.materialInstancePath->value
                : std::string();
        }
        else
        {
            queuedAutomaticPreviewDisconnectAssetPath.reset();
        }
        return;
    }

    queuedAutomaticPreviewDisconnectAssetPath.reset();

    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(
            snapshot.activeDocument->assetPath);
    if (!pathResult.Succeeded())
    {
        return;
    }

    const Preview::MaterialInstancePreviewStatus previewStatus =
        previewController->GetStatus();
    if (previewStatus.materialInstancePath.has_value() &&
        previewStatus.materialInstancePath.value() == pathResult.path.value() &&
        previewStatus.IsConnected())
    {
        return;
    }

    // 连接和应用都在 Tick() 的命令边界执行，避免从文档 service 直接触碰
    // renderer-owned preview adapter。
    queuedAutomaticPreviewConnectionAssetPath = pathResult.path->value;
}

void MaterialInstanceEditorRuntime::SubmitQueuedAutomaticPreviewDisconnect()
{
    if (!queuedAutomaticPreviewDisconnectAssetPath.has_value() ||
        previewController == nullptr)
    {
        return;
    }

    const std::string assetPath =
        queuedAutomaticPreviewDisconnectAssetPath.value();
    queuedAutomaticPreviewDisconnectAssetPath.reset();
    EditorCommandEnvelope command;
    command.source = EditorCommandSource::ImGui;
    command.type = EditorCommandType::DisconnectMaterialInstancePreview;
    command.payload = MaterialInstanceAssetPathPayload{assetPath};
    SubmitAutomaticPreviewCommand(std::move(command));
}

void MaterialInstanceEditorRuntime::SubmitQueuedAutomaticPreviewConnection()
{
    if (!queuedAutomaticPreviewConnectionAssetPath.has_value() ||
        pendingPreviewCommand.has_value() || worldPath.empty() ||
        worldGeneration == 0 || !snapshot.activeDocument.has_value())
    {
        return;
    }

    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(
            snapshot.activeDocument->assetPath);
    if (!pathResult.Succeeded() ||
        pathResult.path->value != queuedAutomaticPreviewConnectionAssetPath.value())
    {
        queuedAutomaticPreviewConnectionAssetPath.reset();
        return;
    }

    const Preview::MaterialInstancePreviewStatus previewStatus =
        previewController->GetStatus();
    if (previewStatus.materialInstancePath.has_value() &&
        previewStatus.materialInstancePath.value() == pathResult.path.value() &&
        previewStatus.IsConnected())
    {
        queuedAutomaticPreviewConnectionAssetPath.reset();
        return;
    }

    queuedAutomaticPreviewConnectionAssetPath.reset();
    EditorCommandEnvelope command;
    command.source = EditorCommandSource::ImGui;
    command.type = EditorCommandType::ConnectMaterialInstancePreview;
    command.expectedDocumentRevision = snapshot.activeDocument->revision;
    command.payload = MaterialInstanceAssetPathPayload{
        snapshot.activeDocument->assetPath};
    SubmitAutomaticPreviewCommand(std::move(command));
}

void MaterialInstanceEditorRuntime::QueueAutomaticPreviewApply(
    std::string_view assetPath)
{
    if (previewController == nullptr || worldPath.empty() || worldGeneration == 0 ||
        !snapshot.activeDocument.has_value())
    {
        return;
    }

    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(assetPath);
    const Preview::MaterialInstancePreviewPathNormalizationResult documentPathResult =
        Preview::NormalizeMaterialInstancePath(
            snapshot.activeDocument->assetPath);
    if (!pathResult.Succeeded() || !documentPathResult.Succeeded() ||
        pathResult.path.value() != documentPathResult.path.value())
    {
        return;
    }

    // 只保留同一资产的最新请求；真正提交前重新读取当前文档 revision。
    queuedAutomaticPreviewApplyAssetPath = pathResult.path->value;
}

void MaterialInstanceEditorRuntime::SubmitQueuedAutomaticPreviewApply()
{
    if (!queuedAutomaticPreviewApplyAssetPath.has_value() ||
        pendingPreviewCommand.has_value() || !snapshot.activeDocument.has_value())
    {
        return;
    }

    const std::string assetPath = queuedAutomaticPreviewApplyAssetPath.value();
    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(assetPath);
    const auto& document = snapshot.activeDocument.value();
    const Preview::MaterialInstancePreviewPathNormalizationResult documentPathResult =
        Preview::NormalizeMaterialInstancePath(document.assetPath);
    if (!pathResult.Succeeded() || !documentPathResult.Succeeded() ||
        pathResult.path.value() != documentPathResult.path.value())
    {
        queuedAutomaticPreviewApplyAssetPath.reset();
        return;
    }

    queuedAutomaticPreviewApplyAssetPath.reset();
    EditorCommandEnvelope command;
    command.source = EditorCommandSource::ImGui;
    command.type = EditorCommandType::ApplyMaterialInstancePreview;
    command.expectedDocumentRevision = document.revision;
    command.payload = ApplyMaterialInstancePreviewPayload{
        document.assetPath,
        document.revision};
    SubmitAutomaticPreviewCommand(std::move(command));
}

void MaterialInstanceEditorRuntime::ApplyServiceResult(
    const MaterialEditorServiceResult& result)
{
    if (result.editor.has_value())
    {
        snapshot = ConvertSnapshot(result.editor.value());
    }
    else if (result.document.has_value() && snapshot.activeDocument.has_value() &&
        snapshot.activeDocument->assetPath == result.document->assetPath)
    {
        // 高频参数编辑只返回当前文档，保留资产目录和场景导航快照。
        snapshot.activeDocument = ConvertDocument(result.document.value());
    }
    else
    {
        RefreshSnapshotFromService();
    }

    if (!result.message.empty())
    {
        lastCommandMessage = FormatServiceMessage(result);
        SetStatus(lastCommandMessage);
    }
    UpdateBusRevision(result);
    ApplyRuntimeOverlay();
}

void MaterialInstanceEditorRuntime::RefreshSnapshotFromService()
{
    if (service == nullptr)
    {
        return;
    }

    try
    {
        snapshot = ConvertSnapshot(service->BuildSnapshot());
    }
    catch (const std::exception& exception)
    {
        diagnostic = exception.what();
        SetStatus(diagnostic);
    }
}

void MaterialInstanceEditorRuntime::ApplyRuntimeOverlay()
{
    if (!snapshot.activeDocument.has_value())
    {
        return;
    }

    EditorUi::MaterialInstanceDocumentSnapshot& document =
        snapshot.activeDocument.value();
    const Preview::MaterialInstancePreviewStatus previewStatus =
        previewController->GetStatus();
    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(document.assetPath);
    const bool targetsActiveDocument = pathResult.Succeeded() &&
        previewStatus.materialInstancePath.has_value() &&
        previewStatus.materialInstancePath.value() == pathResult.path.value();

    if (!targetsActiveDocument)
    {
        document.preview = EditorUi::EditorPreviewStatus::Unavailable;
        document.previewMessage = PreviewUnavailableMessage;
        if (!worldPath.empty())
        {
            document.previewMessage += " Active World: " + worldPath;
        }
        return;
    }

    document.preview = ToEditorPreviewStatus(previewStatus.state);
    document.previewMessage = previewStatus.diagnosticMessage;
    if (document.previewMessage.empty())
    {
        document.previewMessage = previewStatus.IsConnected()
            ? "Runtime material instance preview is connected."
            : PreviewUnavailableMessage;
    }
}

void MaterialInstanceEditorRuntime::UpdateBusRevision(
    const MaterialEditorServiceResult& result)
{
    if (result.document.has_value())
    {
        commandBus->SetDocumentRevision(
            result.document->assetPath,
            result.document->revision);
    }
    if (result.editor.has_value() &&
        result.editor->activeDocument.has_value())
    {
        const auto& document = result.editor->activeDocument.value();
        commandBus->SetDocumentRevision(document.assetPath, document.revision);
    }
}

void MaterialInstanceEditorRuntime::SetStatus(std::string message)
{
    if (message.empty())
    {
        return;
    }
    snapshot.statusMessage = std::move(message);
}

EditorUi::MaterialInstanceEditorSnapshot
MaterialInstanceEditorRuntime::ConvertSnapshot(
    const MaterialEditorSnapshot& source)
{
    EditorUi::MaterialInstanceEditorSnapshot result;
    result.selectedDocumentPath = source.selectedDocumentPath;
    result.statusMessage = source.statusMessage;
    result.browserLoading = source.browserLoading;

    result.assets.reserve(source.assets.size());
    for (const MaterialEditorAssetEntry& asset : source.assets)
    {
        result.assets.push_back(
            EditorUi::MaterialInstanceAssetEntry{asset.assetPath, asset.dirty});
    }

    result.documentTabs.reserve(source.documentTabs.size());
    for (const MaterialEditorAssetEntry& tab : source.documentTabs)
    {
        result.documentTabs.push_back(
            EditorUi::MaterialInstanceAssetEntry{tab.assetPath, tab.dirty});
    }

    result.textureAssets.reserve(source.textureAssets.size());
    for (const MaterialEditorTextureAssetSnapshot& asset : source.textureAssets)
    {
        result.textureAssets.push_back(EditorUi::EditorTextureAssetSnapshot{
            asset.assetPath,
            asset.valid,
            asset.diagnostic});
    }

    if (source.activeDocument.has_value())
    {
        result.activeDocument = ConvertDocument(source.activeDocument.value());
    }
    return result;
}

EditorUi::MaterialInstanceDocumentSnapshot
MaterialInstanceEditorRuntime::ConvertDocument(
    const MaterialEditorDocumentSnapshot& source)
{
    EditorUi::MaterialInstanceDocumentSnapshot result;
    result.assetPath = source.assetPath;
    result.baseMaterialPath = source.baseMaterialPath;
    result.schemaDigest = source.schemaDigest;
    result.revision = source.revision;
    result.status = ConvertDocumentState(source.state);
    result.validation = ConvertValidationState(source.validation);
    result.preview = EditorUi::EditorPreviewStatus::Unavailable;
    result.validationMessage = source.validationMessage;
    result.previewMessage = PreviewUnavailableMessage;

    result.parameters.reserve(source.parameters.size());
    for (const MaterialEditorParameterSnapshot& parameter : source.parameters)
    {
        result.parameters.push_back(ConvertParameter(parameter));
    }

    result.textures.reserve(source.textures.size());
    for (const MaterialEditorTextureBindingSnapshot& texture : source.textures)
    {
        result.textures.push_back(ConvertTexture(texture));
    }
    result.renderStates.reserve(source.renderStates.size());
    for (const MaterialEditorRenderStateSnapshot& renderState : source.renderStates)
    {
        result.renderStates.push_back(ConvertRenderState(renderState));
    }
    return result;
}

EditorUi::EditorParameterSnapshot
MaterialInstanceEditorRuntime::ConvertParameter(
    const MaterialEditorParameterSnapshot& source)
{
    EditorUi::EditorParameterSnapshot result;
    result.name = source.name;
    result.description = source.description;
    result.type = ConvertParameterType(source.type);
    result.defaultValue = ConvertParameterValue(source.defaultValue);
    result.effectiveValue = ConvertParameterValue(source.effectiveValue);
    if (source.overrideValue.has_value())
    {
        result.overrideValue = ConvertParameterValue(source.overrideValue.value());
    }
    result.active = source.active;
    if (source.min.has_value() && source.max.has_value())
    {
        result.hasRange = true;
        result.minValue = source.min.value();
        result.maxValue = source.max.value();
    }

    const std::size_t channelCount = std::min(
        source.channels.size(),
        result.channels.size());
    for (std::size_t index = 0; index < channelCount; ++index)
    {
        const MaterialEditorParameterChannelSnapshot& sourceChannel =
            source.channels[index];
        EditorUi::EditorParameterChannel& resultChannel = result.channels[index];
        resultChannel.name = sourceChannel.name;
        resultChannel.description = sourceChannel.description;
        if (sourceChannel.min.has_value() && sourceChannel.max.has_value())
        {
            resultChannel.hasRange = true;
            resultChannel.minValue = sourceChannel.min.value();
            resultChannel.maxValue = sourceChannel.max.value();
        }
    }
    return result;
}

EditorUi::EditorTextureBindingSnapshot
MaterialInstanceEditorRuntime::ConvertTexture(
    const MaterialEditorTextureBindingSnapshot& source)
{
    EditorUi::EditorTextureBindingSnapshot result;
    result.slotName = source.slotName;
    result.description = source.description;
    result.defaultAssetPath = source.defaultAssetPath;
    result.effectiveAssetPath = source.effectiveAssetPath;
    result.overrideAssetPath = source.overrideAssetPath;
    result.active = source.active;
    return result;
}

EditorUi::EditorRenderStateSnapshot
MaterialInstanceEditorRuntime::ConvertRenderState(
    const MaterialEditorRenderStateSnapshot& source)
{
    EditorUi::EditorRenderStateSnapshot result;
    result.name = source.name;
    result.defaultValue = source.defaultValue;
    result.effectiveValue = source.effectiveValue;
    result.overrideValue = source.overrideValue;
    return result;
}

EditorUi::EditorParameterType MaterialInstanceEditorRuntime::ConvertParameterType(
    EditorMaterialParameterType type)
{
    switch (type)
    {
    case EditorMaterialParameterType::Float:
        return EditorUi::EditorParameterType::Float;
    case EditorMaterialParameterType::Vec2:
        return EditorUi::EditorParameterType::Vec2;
    case EditorMaterialParameterType::Vec3:
        return EditorUi::EditorParameterType::Vec3;
    case EditorMaterialParameterType::Vec4:
        return EditorUi::EditorParameterType::Vec4;
    }
    return EditorUi::EditorParameterType::Float;
}

EditorUi::EditorParameterValue MaterialInstanceEditorRuntime::ConvertParameterValue(
    const EditorMaterialParameterValue& value)
{
    EditorUi::EditorParameterValue result;
    std::visit(
        [&result](const auto& sourceValue)
        {
            using ValueType = std::decay_t<decltype(sourceValue)>;
            if constexpr (std::is_same_v<ValueType, float>)
            {
                result.type = EditorUi::EditorParameterType::Float;
                result.values[0] = sourceValue;
            }
            else if constexpr (std::is_same_v<ValueType, EditorVec2>)
            {
                result.type = EditorUi::EditorParameterType::Vec2;
                std::copy(sourceValue.begin(), sourceValue.end(), result.values.begin());
            }
            else if constexpr (std::is_same_v<ValueType, EditorVec3>)
            {
                result.type = EditorUi::EditorParameterType::Vec3;
                std::copy(sourceValue.begin(), sourceValue.end(), result.values.begin());
            }
            else
            {
                result.type = EditorUi::EditorParameterType::Vec4;
                std::copy(sourceValue.begin(), sourceValue.end(), result.values.begin());
            }
        },
        value);
    return result;
}

EditorUi::EditorDocumentStatus MaterialInstanceEditorRuntime::ConvertDocumentState(
    MaterialEditorDocumentState state)
{
    switch (state)
    {
    case MaterialEditorDocumentState::Clean:
        return EditorUi::EditorDocumentStatus::Clean;
    case MaterialEditorDocumentState::Dirty:
        return EditorUi::EditorDocumentStatus::Dirty;
    case MaterialEditorDocumentState::SaveFailed:
        return EditorUi::EditorDocumentStatus::SaveFailed;
    case MaterialEditorDocumentState::SourceChanged:
        return EditorUi::EditorDocumentStatus::SourceChanged;
    }
    return EditorUi::EditorDocumentStatus::SaveFailed;
}

EditorUi::EditorValidationStatus
MaterialInstanceEditorRuntime::ConvertValidationState(
    MaterialEditorValidationState state)
{
    switch (state)
    {
    case MaterialEditorValidationState::Unknown:
        return EditorUi::EditorValidationStatus::Unknown;
    case MaterialEditorValidationState::Valid:
        return EditorUi::EditorValidationStatus::Valid;
    case MaterialEditorValidationState::Invalid:
        return EditorUi::EditorValidationStatus::Invalid;
    }
    return EditorUi::EditorValidationStatus::Unknown;
}

} // namespace VL::Editor
