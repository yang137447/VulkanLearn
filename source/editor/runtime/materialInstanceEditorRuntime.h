#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/runtimeResult.h"
#include "editor/command/editorCommandBus.h"
#include "editor/preview/materialInstancePreviewController.h"
#include "editor/service/materialInstanceDocumentService.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"

namespace VL::Editor
{

// 这是 ImGui 与 Asset Document 之间的 game-thread facade。
// 面板只提交 EditorCommand，服务只在 Tick() 的 drain 阶段被调用。
class MaterialInstanceEditorRuntime final :
    public EditorUi::IEditorCommandSink
{
public:
    struct Config
    {
        std::filesystem::path resourceRoot;
        std::filesystem::path projectRoot;
        std::size_t maxResultCount = 256;
        // Runtime 只借用 renderer owner 提供的 adapter；adapter 生命周期必须
        // 覆盖本 Runtime，并且所有 live MaterialInstance 访问都发生在稳定帧边界。
        Preview::IMaterialInstancePreviewAdapter* previewAdapter = nullptr;
    };

    MaterialInstanceEditorRuntime() = default;
    ~MaterialInstanceEditorRuntime();

    MaterialInstanceEditorRuntime(const MaterialInstanceEditorRuntime&) = delete;
    MaterialInstanceEditorRuntime& operator=(
        const MaterialInstanceEditorRuntime&) = delete;

    RuntimeResult<void> Initialize(Config config);
    void Shutdown() noexcept;

    // 在固定的 UI/game-thread 边界执行本帧命令，并更新只读面板快照。
    void Tick();

    // World 换代不关闭 Asset Document；当前 preview adapter 未接入时只更新诊断。
    // WorldHandle 的 generation 是唯一权威代际；path 仅用于同代诊断和定位。
    void NotifyWorldChanged(std::string_view worldPath, uint64_t generation);

    void Submit(EditorCommandEnvelope command) override;

    // 给 UiSubsystem 的全局快捷键入口使用；实际保存仍经由中央 CommandBus。
    bool SubmitSaveActiveDocument();

    const EditorUi::MaterialInstanceEditorSnapshot& GetSnapshot() const noexcept
    {
        return snapshot;
    }

    const std::string& GetDiagnostic() const noexcept
    {
        return diagnostic;
    }

    bool IsInitialized() const noexcept
    {
        return initialized;
    }

    std::size_t PendingCommandCount() const noexcept
    {
        return commandBus == nullptr ? 0 : commandBus->PendingCount();
    }

private:
    EditorCommandSubmission SubmitInternal(EditorCommandEnvelope command);
    EditorCommandSubmission SubmitAutomaticPreviewCommand(
        EditorCommandEnvelope command);
    EditorCommandResult ExecuteCommand(const EditorCommandEnvelope& command);
    EditorCommandResult FinishServiceResult(
        const EditorCommandEnvelope& command,
        MaterialEditorServiceResult serviceResult);
    EditorCommandResult MakeDiagnosticResult(
        const EditorCommandEnvelope& command,
        EditorCommandStatus status,
        EditorErrorCode errorCode,
        std::string message) const;
    EditorCommandResult MakeUnsupportedResult(
        const EditorCommandEnvelope& command,
        EditorErrorCode errorCode,
        std::string message) const;

    EditorCommandResult ExecutePreviewCommand(
        const EditorCommandEnvelope& command);
    EditorCommandResult ConvertPreviewResult(
        const EditorCommandEnvelope& command,
        const Preview::MaterialInstancePreviewCommandResult& result) const;
    std::optional<Preview::MaterialInstancePreviewDraft>
        BuildBaselinePreviewDraft(std::string_view assetPath) const;
    void PollPreview();
    void RejectPendingPreviewForWorldChange();
    void QueueAutomaticPreviewConnection();
    void SubmitQueuedAutomaticPreviewDisconnect();
    void SubmitQueuedAutomaticPreviewConnection();
    void QueueAutomaticPreviewApply(std::string_view assetPath);
    void SubmitQueuedAutomaticPreviewApply();

    void ApplyServiceResult(const MaterialEditorServiceResult& result);
    void RefreshSnapshotFromService();
    void ApplyRuntimeOverlay();
    void UpdateBusRevision(const MaterialEditorServiceResult& result);
    void SetStatus(std::string message);

    static EditorUi::MaterialInstanceEditorSnapshot ConvertSnapshot(
        const MaterialEditorSnapshot& source);
    static EditorUi::MaterialInstanceDocumentSnapshot ConvertDocument(
        const MaterialEditorDocumentSnapshot& source);
    static EditorUi::EditorParameterSnapshot ConvertParameter(
        const MaterialEditorParameterSnapshot& source);
    static EditorUi::EditorTextureBindingSnapshot ConvertTexture(
        const MaterialEditorTextureBindingSnapshot& source);
    static EditorUi::EditorRenderStateSnapshot ConvertRenderState(
        const MaterialEditorRenderStateSnapshot& source);
    static EditorUi::EditorParameterType ConvertParameterType(
        EditorMaterialParameterType type);
    static EditorUi::EditorParameterValue ConvertParameterValue(
        const EditorMaterialParameterValue& value);
    static EditorUi::EditorDocumentStatus ConvertDocumentState(
        MaterialEditorDocumentState state);
    static EditorUi::EditorValidationStatus ConvertValidationState(
        MaterialEditorValidationState state);

    Config config;
    std::unique_ptr<EditorCommandBus> commandBus;
    std::unique_ptr<MaterialInstanceDocumentService> service;
    EditorUi::MaterialInstanceEditorSnapshot snapshot;
    std::string worldPath;
    std::string diagnostic;
    std::string lastCommandMessage;
    std::unique_ptr<Preview::MaterialInstancePreviewController> previewController;
    std::optional<EditorCommandEnvelope> pendingPreviewCommand;
    std::optional<std::string> queuedAutomaticPreviewDisconnectAssetPath;
    std::optional<std::string> queuedAutomaticPreviewConnectionAssetPath;
    std::optional<std::string> queuedAutomaticPreviewApplyAssetPath;
    uint64_t worldGeneration = 0;
    bool initialized = false;
};

} // namespace VL::Editor
