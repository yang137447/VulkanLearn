#pragma once

#include <optional>
#include <string>

#include "editor/preview/materialInstancePreviewAdapter.h"

namespace VL::Editor::Preview
{

// Controller 只维护可复制的 preview 状态和代际；live MaterialInstance、
// descriptor package 与 Vulkan 生命周期必须由 owner-thread adapter 管理。
class MaterialInstancePreviewController final
{
public:
    MaterialInstancePreviewController();
    explicit MaterialInstancePreviewController(
        IMaterialInstancePreviewAdapter& adapter);
    ~MaterialInstancePreviewController() = default;

    MaterialInstancePreviewController(
        const MaterialInstancePreviewController&) = delete;
    MaterialInstancePreviewController& operator=(
        const MaterialInstancePreviewController&) = delete;

    MaterialInstancePreviewCommandResult Execute(
        const MaterialInstancePreviewCommand& command);

    // 有 pending operation 时返回 Running 或终态结果；没有 pending 时返回
    // nullopt。调用者应把返回值发布到统一 result store。
    std::optional<MaterialInstancePreviewCommandResult> Poll();

    // 由 World owner 在稳定边界通知当前 World。旧连接和 pending 结果会被
    // 取消/失效，但不会触碰 Asset Document 的 baseline 或 dirty 状态。
    void NotifyActiveWorldChanged(
        MaterialInstancePreviewWorldIdentity activeWorld);
    void ClearActiveWorld();
    void Shutdown();

    MaterialInstancePreviewStatus GetStatus() const;

    bool HasPendingOperation() const noexcept
    {
        return pendingOperation.has_value();
    }

    std::optional<PreviewOperationId> GetPendingOperationId() const noexcept;

private:
    struct PendingOperation
    {
        MaterialInstancePreviewCommand command;
        MaterialInstancePreviewAdapterCommand adapterCommand;
        PreviewOperationId operationId = 0;
    };

    struct NormalizedCommand
    {
        MaterialInstancePreviewAdapterCommand adapterCommand;
        std::optional<uint64_t> documentRevision;
    };

    MaterialInstancePreviewCommandResult MakeResult(
        const MaterialInstancePreviewCommand& command,
        MaterialInstancePreviewCommandStatus commandStatus,
        MaterialInstancePreviewErrorCode errorCode,
        std::string message,
        std::optional<uint64_t> documentRevision = std::nullopt,
        std::optional<PreviewOperationId> operationId = std::nullopt,
        bool liveSwapCommitted = false) const;

    MaterialInstancePreviewCommandResult Reject(
        const MaterialInstancePreviewCommand& command,
        MaterialInstancePreviewErrorCode errorCode,
        std::string message) const;

    std::optional<NormalizedCommand> NormalizeCommand(
        const MaterialInstancePreviewCommand& command,
        MaterialInstancePreviewCommandResult& rejection) const;

    MaterialInstancePreviewCommandResult ExecuteNormalized(
        const MaterialInstancePreviewCommand& command,
        NormalizedCommand normalized);

    MaterialInstancePreviewCommandResult ExecuteDisconnect(
        const MaterialInstancePreviewCommand& command);

    MaterialInstancePreviewCommandResult CompleteAdapterResult(
        const MaterialInstancePreviewCommand& command,
        const MaterialInstancePreviewAdapterCommand& adapterCommand,
        const MaterialInstancePreviewAdapterResult& adapterResult);

    MaterialInstancePreviewCommandResult CompleteAdapterFailure(
        const MaterialInstancePreviewCommand& command,
        const MaterialInstancePreviewAdapterResult& adapterResult);

    MaterialInstancePreviewErrorCode ResolveAdapterError(
        const MaterialInstancePreviewAdapterResult& adapterResult) const noexcept;

    bool DisconnectCurrentForReplacement();
    MaterialInstancePreviewAdapterResult ExecuteAdapter(
        const MaterialInstancePreviewAdapterCommand& command);
    MaterialInstancePreviewAdapterCommand MakeDisconnectAdapterCommand() const;

    void BeginConnection(
        const MaterialInstancePreviewWorldIdentity& world,
        const NormalizedMaterialInstancePath& materialInstancePath,
        std::optional<uint64_t> documentRevision);
    void ClearConnectionState(std::string diagnosticMessage);
    void AdvanceLiveGeneration() noexcept;
    void TouchStatus() noexcept;
    void CancelPendingOperation() noexcept;

    UnavailableMaterialInstancePreviewAdapter unavailableAdapter;
    IMaterialInstancePreviewAdapter* adapter = nullptr;
    std::optional<MaterialInstancePreviewWorldIdentity> activeWorld;
    MaterialInstancePreviewStatus status;
    std::optional<PendingOperation> pendingOperation;
    bool liveConnectionEstablished = false;
};

} // namespace VL::Editor::Preview
