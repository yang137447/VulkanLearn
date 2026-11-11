#pragma once

#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/service/materialInstanceDocumentService.h"

#include <string>
#include <utility>

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
    case EditorCommandSource::RmlUi:
        return Preview::PreviewCommandSource::RmlUi;
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
} // namespace VL::Editor