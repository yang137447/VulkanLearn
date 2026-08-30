#pragma once

// JSON 只负责稳定的跨边界表示；业务执行、资源查找和 Vulkan 生命周期不在此层。

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

#include "editor/command/editorCommand.h"

namespace VL
{

using EditorCommandJson = nlohmann::json;

class EditorCommandJsonCodecError : public std::runtime_error
{
public:
    EditorCommandJsonCodecError(
        EditorErrorCode errorCode,
        const std::string& message)
        : std::runtime_error(message), errorCodeValue(errorCode)
    {
    }

    EditorErrorCode Code() const noexcept { return errorCodeValue; }

private:
    EditorErrorCode errorCodeValue;
};

// 这些名称是 transport 的稳定 token，不使用 C++ enum 的展示名称。
std::string_view GetEditorCommandJsonSourceName(
    EditorCommandSource source) noexcept;
std::string_view GetEditorCommandJsonStatusName(
    EditorCommandStatus status) noexcept;
std::string_view GetEditorCommandJsonErrorName(
    EditorErrorCode errorCode) noexcept;

class EditorCommandJsonCodec final
{
public:
    using Json = EditorCommandJson;

    // 非法 wire 一律抛出带稳定 EditorErrorCode 与字段上下文的 codec 异常。
    static Json Encode(const EditorCommandEnvelope& command);
    static EditorCommandEnvelope DecodeCommand(const Json& wire);

    static Json EncodeResult(const EditorCommandResult& result);
    static EditorCommandResult DecodeResult(const Json& wire);

    static std::string EncodeText(const EditorCommandEnvelope& command);
    static EditorCommandEnvelope DecodeCommandText(std::string_view text);

    static std::string EncodeResultText(const EditorCommandResult& result);
    static EditorCommandResult DecodeResultText(std::string_view text);
};

EditorCommandJson EncodeEditorCommandEnvelope(const EditorCommandEnvelope& command);
EditorCommandEnvelope DecodeEditorCommandEnvelope(const EditorCommandJson& wire);
EditorCommandJson EncodeEditorCommandResult(const EditorCommandResult& result);
EditorCommandResult DecodeEditorCommandResult(const EditorCommandJson& wire);

} // namespace VL
