#pragma once

// 执行器是 EditorCommandBus 与资产文档服务之间唯一的业务边界。
// 它运行在游戏线程稳定点，负责把值语义命令转换为服务调用；ImGui、Console
// 和自动化测试都不能绕过这层直接修改文档或文件。

#include "editor/command/editorCommandBus.h"
#include "editor/service/materialInstanceDocumentService.h"

#include <cstddef>
#include <optional>

namespace VL::Editor
{

class MaterialInstanceEditorCommandExecutor
{
public:
    explicit MaterialInstanceEditorCommandExecutor(
        MaterialInstanceDocumentService& documentService) noexcept;

    MaterialInstanceEditorCommandExecutor(
        const MaterialInstanceEditorCommandExecutor&) = delete;
    MaterialInstanceEditorCommandExecutor& operator=(
        const MaterialInstanceEditorCommandExecutor&) = delete;

    EditorCommandResult Execute(const EditorCommandEnvelope& command);

    std::size_t Drain(
        EditorCommandBus& commandBus,
        std::size_t maxCount = 0);

    MaterialEditorSnapshot BuildSnapshot();
    const std::optional<EditorCommandResult>& GetLastResult() const noexcept
    {
        return lastResult;
    }

private:
    static EditorCommandResult MakeRejected(
        const EditorCommandEnvelope& command,
        EditorErrorCode errorCode,
        std::string message);
    static EditorCommandResult ConvertResult(
        const EditorCommandEnvelope& command,
        const MaterialEditorServiceResult& serviceResult);
    static bool IsAdmissionError(EditorErrorCode errorCode) noexcept;
    static std::optional<std::string> GetCommandAssetPath(
        const EditorCommandEnvelope& command);

    EditorCommandResult ExecuteDocumentCommand(
        const EditorCommandEnvelope& command);

    MaterialInstanceDocumentService* documentService = nullptr;
    std::optional<EditorCommandResult> lastResult;
};

} // namespace VL::Editor
