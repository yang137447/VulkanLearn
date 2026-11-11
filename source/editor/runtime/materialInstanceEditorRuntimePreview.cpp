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
        previewController->HasLiveConnection())
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
    command.source = EditorCommandSource::RmlUi;
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
        previewController->HasLiveConnection())
    {
        queuedAutomaticPreviewConnectionAssetPath.reset();
        return;
    }

    queuedAutomaticPreviewConnectionAssetPath.reset();
    EditorCommandEnvelope command;
    command.source = EditorCommandSource::RmlUi;
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
    command.source = EditorCommandSource::RmlUi;
    command.type = EditorCommandType::ApplyMaterialInstancePreview;
    command.expectedDocumentRevision = document.revision;
    command.payload = ApplyMaterialInstancePreviewPayload{
        document.assetPath,
        document.revision};
    SubmitAutomaticPreviewCommand(std::move(command));
}

} // namespace VL::Editor
