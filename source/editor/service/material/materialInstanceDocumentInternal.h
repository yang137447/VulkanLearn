#pragma once

// 文件职责：MaterialInstanceDocumentService 各分解切片共享的内部原语。
// 不对外暴露；只有 service/material/*.cpp 与 facade TU 包含。

#include "editor/command/editorCommand.h"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace VL::Editor::MaterialDocumentDetail
{

using Json = nlohmann::json;

class ServiceError final : public std::runtime_error
{
public:
    ServiceError(EditorErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), errorCode(code)
    {
    }

    EditorErrorCode errorCode;
};

[[noreturn]] inline void ThrowServiceError(
    EditorErrorCode code,
    std::string message)
{
    throw ServiceError(code, std::move(message));
}

inline std::string ReadStringField(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    if (!object.contains(std::string(field)) ||
        !object.at(std::string(field)).is_string())
    {
        ThrowServiceError(
            EditorErrorCode::ValidationFailed,
            std::string(context) + " is missing string field '" +
                std::string(field) + "'");
    }
    return object.at(std::string(field)).get<std::string>();
}

inline Json MakeManagedMaterialInstanceJson(const Json& source)
{
    Json managed = Json::object();
    static constexpr std::array<std::string_view, 11> fields = {
        "name",
        "type",
        "configHelp",
        "material",
        "renderStateOverrides",
        "macros",
        "parameters",
        "textures",
        "subsurfaceProfile",
        "skinLut",
        "eyeProfile"};
    for (const std::string_view field : fields)
    {
        if (source.contains(std::string(field)))
        {
            managed[std::string(field)] = source.at(std::string(field));
        }
    }
    return managed;
}

// 渲染状态字段/取值到持久化名称的转换被 overrides 与 batch 两个切片共用，
// 因此从 facade 的匿名 namespace 提升到这里；行为与原实现逐字一致。
inline std::string RenderStateFieldName(EditorMaterialRenderStateField field)
{
    return std::string(GetEditorMaterialRenderStateFieldName(field));
}

inline std::string RenderStateValueName(
    EditorMaterialRenderStateField field,
    const EditorMaterialRenderStateValue& value)
{
    if (!IsEditorMaterialRenderStateValueCompatible(field, value))
    {
        ThrowServiceError(
            EditorErrorCode::InvalidPayload,
            "material render state value does not match its field");
    }

    return std::visit(
        [](const auto& typedValue)
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, EditorMaterialRenderMode>)
            {
                return std::string(GetEditorMaterialRenderModeName(typedValue));
            }
            else if constexpr (std::is_same_v<ValueType, EditorMaterialCullMode>)
            {
                return std::string(GetEditorMaterialCullModeName(typedValue));
            }
            else
            {
                return std::string(GetEditorMaterialShadingModelName(typedValue));
            }
        },
        value);
}

} // namespace VL::Editor::MaterialDocumentDetail
