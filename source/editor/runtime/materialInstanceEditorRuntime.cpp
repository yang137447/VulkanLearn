#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/command/editorCommandJsonCodec.h"

#include "editor/service/materialInstanceDocumentService.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>
#include "editor/runtime/materialInstanceEditorRuntimeInternal.h"

namespace VL::Editor
{
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
        sceneDocuments = std::make_unique<SceneDocumentService>(service->GetResourceRoot());
        assetDocuments = std::make_unique<AssetCommandExecutor>(service->GetResourceRoot(), *sceneDocuments);
        initialized = true;
        diagnostic.clear();
        snapshot = EditorUi::MaterialInstanceEditorSnapshot{};
        snapshot.statusMessage = "Loading material instance assets...";

        // 初始资产列表也走同一条命令链，避免初始化阶段产生旁路 service 调用。
        EditorCommandEnvelope listCommand;
        listCommand.source = EditorCommandSource::RmlUi;
        listCommand.type = EditorCommandType::ListMaterialInstanceAssets;
        // 浏览器在只读快照上本地筛选，初始目录不能被命令协议的默认分页截断。
        listCommand.payload = ListMaterialInstanceAssetsPayload{
            {}, 0, std::numeric_limits<uint32_t>::max()};
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
    assetDocuments.reset();
    sceneDocuments.reset();
    commandBus.reset();
    previewController.reset();
    snapshot = EditorUi::MaterialInstanceEditorSnapshot{};
    config = Config{};
    worldPath.clear();
    worldGeneration = 0;
    saveSequence = 0;
    saveFeedback.clear();
    blueprintCommandSequence = 0;
    blueprintCommandFeedback.clear();
    particleCommandSequence = 0;
    particleCommandFeedback.clear();
    diagnostic.clear();
    lastCommandMessage.clear();
    initialized = false;
}

bool MaterialInstanceEditorRuntime::Tick()
{
    if (!initialized)
    {
        return false;
    }

    if (commandBus == nullptr)
    {
        return false;
    }

    PollPreview();
    bool executedCommand = false;

    // 自动预览命令可能由本轮参数编辑追加到 bus；持续 drain，保证一次
    // 参数提交在同一个 UI/game tick 内完成 working draft -> live swap。
    while (true)
    {
        const std::vector<EditorCommandEnvelope> commands = commandBus->Drain();
        for (const EditorCommandEnvelope& command : commands)
        {
            // 任何编辑器命令都可能改变草稿/历史/预览/反馈，需要面板在下一帧立即重建。
            executedCommand = true;
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

            RecordSaveFeedback(command, result);
            RecordBlueprintCommandFeedback(command, result);
            RecordParticleCommandFeedback(command, result);
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
    RefreshModifiedDocuments();
    return executedCommand;
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
    if (sceneDocuments && !worldPath.empty())
    {
        std::string error;
        if (!sceneDocuments->Open(worldPath, error)) diagnostic = error;
        else sceneDocuments->NotifyWorldLoaded(worldPath);
    }
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

EditorCommandSubmission MaterialInstanceEditorRuntime::SubmitJson(std::string_view text)
{
    return SubmitInternal(EditorCommandJsonCodec::DecodeCommandText(text));
}

std::optional<EditorCommandResult> MaterialInstanceEditorRuntime::GetCommandResult(EditorCommandId id) const
{
    return commandBus ? commandBus->GetResult(id) : std::nullopt;
}

bool MaterialInstanceEditorRuntime::SubmitSaveActiveDocument()
{
    if (!snapshot.activeDocument.has_value())
    {
        SetStatus("Save requested, but no Material Instance document is open.");
        return false;
    }

    EditorCommandEnvelope command;
    command.source = EditorCommandSource::RmlUi;
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

    const EditorCommandSubmission submission = commandBus->Submit(command);
    if (submission.admission != EditorCommandAdmission::Queued)
    {
        SetStatus(submission.result.message);
        RecordSaveFeedback(command, submission.result);
        RecordBlueprintCommandFeedback(command, submission.result);
        RecordParticleCommandFeedback(command, submission.result);
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
    if (GetEditorAssetDocumentKind(command.type) && assetDocuments)
    {
        auto result = assetDocuments->Execute(command);
        if (const auto* asset = std::get_if<EditorAssetDocumentResultPayload>(&result.payload))
            commandBus->SetDocumentRevision(asset->assetPath, asset->documentRevision);
        SetStatus(result.message);
        return result;
    }
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
    case EditorCommandType::SetMaterialRenderStateOverride:
    {
        // 直接提交必须和 ExecuteEditorCommandBatch 落到同一 service 实现：batch 只在
        // 隔离草稿上循环调用这些方法，facade 不再复制一份逻辑，否则 JSON codec /
        // Console / AI 客户端单独发这条命令只会拿到 InvalidCommandType。
        const auto& payload = std::get<SetMaterialRenderStateOverridePayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->SetMaterialRenderStateOverride(
                payload.assetPath,
                payload.field,
                payload.value));
    }
    case EditorCommandType::ClearMaterialRenderStateOverride:
    {
        const auto& payload = std::get<ClearMaterialRenderStateOverridePayload>(
            command.payload);
        return FinishServiceResult(
            command,
            service->ClearMaterialRenderStateOverride(
                payload.assetPath,
                payload.field));
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

    // render state 与数值参数一样属于 draft 编辑：直接提交时也要刷新 live preview，
    // 否则 batch 路径会刷新而直接路径不会，两条路径的可见结果就不一致。
    if (serviceResult.succeeded && serviceResult.document.has_value() &&
        (command.type == EditorCommandType::SetMaterialParameterOverride ||
         command.type == EditorCommandType::ClearMaterialParameterOverride ||
         command.type == EditorCommandType::SetMaterialRenderStateOverride ||
         command.type == EditorCommandType::ClearMaterialRenderStateOverride ||
         command.type == EditorCommandType::ExecuteEditorCommandBatch))
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

} // namespace VL::Editor
