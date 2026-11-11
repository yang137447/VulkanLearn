#pragma once

#include "editor/command/editorCommandJsonCodec.h"

#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace VL
{
namespace EditorCommandJsonDetail
{

using Json = nlohmann::json;

[[noreturn]] void ThrowCodecError(
    EditorErrorCode errorCode,
    std::string_view context,
    std::string_view detail);
void Require(
    bool condition,
    EditorErrorCode errorCode,
    std::string_view context,
    std::string_view detail);
void RequireObject(
    const Json& value,
    std::string_view context,
    EditorErrorCode errorCode = EditorErrorCode::InvalidPayload);
bool ContainsField(
    const Json& object,
    std::string_view field);
void RequireAllowedFields(
    const Json& object,
    std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional,
    std::string_view context);

template <typename T>
T ReadUnsigned(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    RequireObject(object, context);
    const auto iterator = object.find(std::string(field));
    Require(
        iterator != object.end(),
        EditorErrorCode::InvalidPayload,
        context,
        std::string("missing field ") + std::string(field));

    const Json& value = *iterator;
    uint64_t converted = 0;
    if (value.is_number_unsigned())
    {
        converted = value.get<uint64_t>();
    }
    else if (value.is_number_integer())
    {
        const int64_t signedValue = value.get<int64_t>();
        Require(
            signedValue >= 0,
            EditorErrorCode::InvalidPayload,
            context,
            std::string(field) + " must be an unsigned integer");
        converted = static_cast<uint64_t>(signedValue);
    }
    else
    {
        ThrowCodecError(
            EditorErrorCode::InvalidPayload,
            context,
            std::string(field) + " must be an unsigned integer");
    }

    Require(
        converted <= static_cast<uint64_t>(std::numeric_limits<T>::max()),
        EditorErrorCode::InvalidPayload,
        context,
        std::string(field) + " is out of range");
    return static_cast<T>(converted);
}

float ReadFiniteFloat(
    const Json& value,
    std::string_view context);

std::string ReadString(
    const Json& object,
    std::string_view field,
    std::string_view context,
    bool requireNonEmpty = true);

bool ReadBoolean(
    const Json& object,
    std::string_view field,
    std::string_view context);

std::string NormalizeRelativeAssetPath(
    std::string_view rawPath,
    std::string_view context,
    bool requireTextureAsset = false);

std::string ReadAssetPath(
    const Json& object,
    std::string_view field,
    std::string_view context,
    bool requireTextureAsset = false);

std::string_view RequireWireName(
    std::string_view name,
    std::string_view context);

EditorCommandSource DecodeSource(
    std::string_view name);

EditorCommandStatus DecodeStatus(
    std::string_view name);

EditorErrorCode DecodeErrorCode(
    std::string_view name);

EditorMaterialParameterType DecodeParameterType(
    std::string_view name);

EditorDirtyDocumentPolicy DecodeDirtyPolicy(
    std::string_view name);

EditorResetScope DecodeResetScope(
    std::string_view name);

EditorMaterialRenderStateField DecodeRenderStateField(
    std::string_view name,
    std::string_view context);

EditorMaterialRenderStateValue DecodeRenderStateValue(
    EditorMaterialRenderStateField field,
    std::string_view name,
    std::string_view context);

EditorCommandType DecodeCommandType(
    std::string_view name);

uint32_t DecodeSectionSelector(
    std::string_view selector,
    std::string_view context);

Json EncodeParameterValue(
    const EditorMaterialParameterValue& value,
    std::string_view context);

EditorMaterialParameterValue DecodeParameterValue(
    const Json& value,
    EditorMaterialParameterType type,
    std::string_view context);

Json EncodeNavigationOrigin(
    const EditorNavigationOrigin& origin);

EditorNavigationOrigin DecodeNavigationOrigin(
    const Json& wire);

Json EncodePayload(
    EditorCommandType type,
    const EditorCommandPayload& payload);

EditorCommandPayload DecodePayload(
    EditorCommandType type,
    const Json& wire);

bool IsBatchCommandType(
    EditorCommandType type) noexcept;

EditorCommandBatchItemPayload ToBatchPayload(
    EditorCommandType type,
    const EditorCommandPayload& payload);

Json EncodeBatchItemPayload(
    EditorCommandType type,
    const EditorCommandBatchItemPayload& payload);

Json EncodeBatchPayload(
    const ExecuteEditorCommandBatchPayload& batch);

EditorCommandBatchItem DecodeBatchItem(
    const Json& wire);

EditorCommandPayload DecodeBatchPayload(
    const Json& wire);

Json EncodePayload(
    EditorCommandType type,
    const EditorCommandPayload& payload);

EditorCommandPayload DecodePayload(
    EditorCommandType type,
    const Json& wire);

void ValidateEnvelopeForCodec(
    const EditorCommandEnvelope& command);

void ValidateResultForCodec(
    const EditorCommandResult& result);

Json EncodeResultPayload(
    const EditorCommandResultPayload& payload);

EditorCommandResultPayload DecodeResultPayload(
    const Json& wire);

} // namespace EditorCommandJsonDetail
} // namespace VL
