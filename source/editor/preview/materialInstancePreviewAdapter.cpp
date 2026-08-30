#include "editor/preview/materialInstancePreviewAdapter.h"

#include <utility>

namespace VL::Editor::Preview
{
namespace
{

constexpr const char* kDefaultUnavailableMessage =
    "Material instance runtime preview is unavailable: the renderer owner "
    "has not supplied a stable-frame preview adapter.";

MaterialInstancePreviewAdapterResult MakeUnavailableResult(
    const std::string& diagnosticMessage)
{
    MaterialInstancePreviewAdapterResult result;
    result.status = PreviewAdapterOperationStatus::Unavailable;
    result.message = diagnosticMessage;
    return result;
}

MaterialInstancePreviewAdapterResult MakeAdapterFailure(
    PreviewAdapterFailureStage failureStage,
    std::string message)
{
    MaterialInstancePreviewAdapterResult result;
    result.status = PreviewAdapterOperationStatus::Failed;
    result.failureStage = failureStage;
    result.message = std::move(message);
    return result;
}

MaterialInstancePreviewAdapterResult MakeCompletedResult(
    std::string message)
{
    MaterialInstancePreviewAdapterResult result;
    result.status = PreviewAdapterOperationStatus::Completed;
    result.liveSwapCommitted = false;
    result.replacesLiveResource = false;
    result.message = std::move(message);
    return result;
}

} // namespace

UnavailableMaterialInstancePreviewAdapter::
    UnavailableMaterialInstancePreviewAdapter(std::string diagnosticMessageValue)
    : diagnosticMessage(std::move(diagnosticMessageValue))
{
    if (diagnosticMessage.empty())
    {
        diagnosticMessage = kDefaultUnavailableMessage;
    }
}

MaterialInstancePreviewAdapterResult
UnavailableMaterialInstancePreviewAdapter::Execute(
    const MaterialInstancePreviewAdapterCommand&)
{
    return MakeUnavailableResult(diagnosticMessage);
}

MaterialInstancePreviewAdapterResult
UnavailableMaterialInstancePreviewAdapter::Poll(PreviewOperationId)
{
    return MakeUnavailableResult(diagnosticMessage);
}

void UnavailableMaterialInstancePreviewAdapter::Cancel(
    PreviewOperationId) noexcept
{
}

MaterialInstancePreviewAdapterResult
UnavailableMaterialInstancePreviewAdapter::Disconnect(uint64_t)
{
    return MakeUnavailableResult(diagnosticMessage);
}

RendererOwnedMaterialInstancePreviewAdapter::
    RendererOwnedMaterialInstancePreviewAdapter(
        IRendererMaterialInstancePreviewOwner& ownerValue)
    : owner(ownerValue)
{
}

MaterialInstancePreviewAdapterResult
RendererOwnedMaterialInstancePreviewAdapter::Execute(
    const MaterialInstancePreviewAdapterCommand& command)
{
    switch (command.type)
    {
    case MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview:
        return ExecuteConnect(command);
    case MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview:
    case MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline:
        return ExecuteLiveUpdate(command);
    case MaterialInstancePreviewCommandType::DisconnectMaterialInstancePreview:
        return Disconnect(command.bridgeLiveGeneration);
    }

    return MakeAdapterFailure(
        PreviewAdapterFailureStage::Prepare,
        "Unknown material instance preview adapter command.");
}

MaterialInstancePreviewAdapterResult
RendererOwnedMaterialInstancePreviewAdapter::Poll(PreviewOperationId operationId)
{
    MaterialInstancePreviewAdapterResult result = MakeAdapterFailure(
        PreviewAdapterFailureStage::Commit,
        "Renderer-owned numeric material preview is synchronous.");
    result.operationId = operationId;
    return result;
}

void RendererOwnedMaterialInstancePreviewAdapter::Cancel(
    PreviewOperationId) noexcept
{
}

MaterialInstancePreviewAdapterResult
RendererOwnedMaterialInstancePreviewAdapter::Disconnect(
    uint64_t bridgeLiveGeneration)
{
    if (!connection.has_value())
    {
        return MakeCompletedResult(
            "Renderer-owned material instance preview was already disconnected.");
    }

    if (bridgeLiveGeneration != connection->bridgeLiveGeneration)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview disconnect belongs to an obsolete bridge generation.");
    }

    // 断开只释放 owner session，不触碰 live MI；数值预览不需要反向写入
    // document baseline，也不创建或退休任何纹理/GPU 资源。
    connection.reset();
    return MakeCompletedResult(
        "Renderer-owned material instance preview disconnected.");
}

void RendererOwnedMaterialInstancePreviewAdapter::Reset() noexcept
{
    connection.reset();
}

MaterialInstancePreviewAdapterResult
RendererOwnedMaterialInstancePreviewAdapter::ExecuteConnect(
    const MaterialInstancePreviewAdapterCommand& command)
{
    if (connection.has_value())
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Renderer-owned material instance preview is already connected.");
    }
    if (!command.world.IsValid())
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview connect requires a valid World identity.");
    }
    if (!command.materialInstancePath.IsValid())
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview connect requires a normalized MI path.");
    }
    if (command.bridgeLiveGeneration == 0)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview connect requires a non-zero bridge generation.");
    }

    const std::optional<MaterialInstancePreviewWorldIdentity> activeWorld =
        owner.GetActiveWorldIdentity();
    if (!activeWorld.has_value())
    {
        return MakeUnavailableResult(
            "Renderer-owned material instance preview has no active World.");
    }
    if (*activeWorld != command.world)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview connect targets an obsolete World generation.");
    }

    std::shared_ptr<IRendererMaterialInstancePreviewSession> preparedSession;
    MaterialInstancePreviewAdapterResult result =
        owner.CaptureMaterialInstancePreviewSession(
            command,
            preparedSession);
    if (result.status != PreviewAdapterOperationStatus::Completed)
    {
        return result;
    }
    if (!preparedSession)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Renderer owner completed preview capture without a session.");
    }

    Connection preparedConnection;
    preparedConnection.world = command.world;
    preparedConnection.materialInstancePath = command.materialInstancePath;
    preparedConnection.bridgeLiveGeneration = command.bridgeLiveGeneration;
    preparedConnection.documentRevision = command.documentRevision;
    preparedConnection.session = std::move(preparedSession);
    connection = std::move(preparedConnection);
    return result;
}

MaterialInstancePreviewAdapterResult
RendererOwnedMaterialInstancePreviewAdapter::ValidateConnectionCommand(
    const MaterialInstancePreviewAdapterCommand& command) const
{
    if (!connection.has_value() || !connection->session)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview is not connected.");
    }
    if (command.bridgeLiveGeneration != connection->bridgeLiveGeneration)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview command belongs to an obsolete bridge generation.");
    }
    if (command.world != connection->world ||
        command.materialInstancePath != connection->materialInstancePath)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview command does not match the connected World or MI path.");
    }

    const std::optional<MaterialInstancePreviewWorldIdentity> activeWorld =
        owner.GetActiveWorldIdentity();
    if (!activeWorld.has_value())
    {
        return MakeUnavailableResult(
            "Renderer-owned material instance preview lost its active World.");
    }
    if (*activeWorld != connection->world)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview command targets an obsolete World generation.");
    }
    return MakeCompletedResult({});
}

MaterialInstancePreviewAdapterResult
RendererOwnedMaterialInstancePreviewAdapter::ExecuteLiveUpdate(
    const MaterialInstancePreviewAdapterCommand& command)
{
    MaterialInstancePreviewAdapterResult validation =
        ValidateConnectionCommand(command);
    if (validation.status != PreviewAdapterOperationStatus::Completed)
    {
        return validation;
    }
    if (!command.draft.has_value())
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview update requires a serialized draft.");
    }
    if (!command.documentRevision.has_value() ||
        *command.documentRevision != command.draft->documentRevision)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview update has a stale or missing document revision.");
    }
    if (connection->documentRevision.has_value() &&
        command.draft->documentRevision < *connection->documentRevision)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            "Material instance preview update rewinds the connected document revision.");
    }
    const MaterialInstancePreviewPathNormalizationResult draftPath =
        NormalizeMaterialInstancePath(command.draft->materialInstancePath);
    if (!draftPath.Succeeded() ||
        draftPath.path->value != connection->materialInstancePath.value)
    {
        return MakeAdapterFailure(
            PreviewAdapterFailureStage::Prepare,
            draftPath.Succeeded()
                ? "Material instance preview draft does not match the connected MI path."
                : draftPath.errorMessage);
    }

    MaterialInstancePreviewAdapterResult result;
    if (command.type ==
        MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline)
    {
        result = connection->session->RestoreBaseline(*command.draft);
    }
    else
    {
        result = connection->session->Apply(*command.draft);
    }
    if (result.status == PreviewAdapterOperationStatus::Completed)
    {
        connection->documentRevision = command.draft->documentRevision;
        if (result.replacesLiveResource)
        {
            // Controller 在收到资源替换成功后会推进一次 bridge generation；
            // adapter 连接必须同步到同一代际，否则下一次调参会被误判为过期。
            connection->bridgeLiveGeneration =
                command.bridgeLiveGeneration == UINT64_MAX
                ? 1
                : command.bridgeLiveGeneration + 1;
        }
    }
    return result;
}

} // namespace VL::Editor::Preview
