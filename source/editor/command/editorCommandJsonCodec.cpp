#include "editor/command/editorCommandJsonCodec.h"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <utility>

namespace VL
{
namespace
{

using Json = nlohmann::json;

[[noreturn]] void ThrowCodecError(
    EditorErrorCode errorCode,
    std::string_view context,
    std::string_view detail)
{
    std::string message;
    message.reserve(context.size() + detail.size() + 2);
    message.append(context);
    message.append(": ");
    message.append(detail);
    throw EditorCommandJsonCodecError(errorCode, message);
}

void Require(
    bool condition,
    EditorErrorCode errorCode,
    std::string_view context,
    std::string_view detail)
{
    if (!condition)
    {
        ThrowCodecError(errorCode, context, detail);
    }
}

void RequireObject(
    const Json& value,
    std::string_view context,
    EditorErrorCode errorCode = EditorErrorCode::InvalidPayload)
{
    Require(value.is_object(), errorCode, context, "must be an object");
}

bool ContainsField(
    const Json& object,
    std::string_view field)
{
    return object.find(std::string(field)) != object.end();
}

void RequireAllowedFields(
    const Json& object,
    std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional,
    std::string_view context)
{
    RequireObject(object, context);

    for (const std::string_view field : required)
    {
        Require(
            ContainsField(object, field),
            EditorErrorCode::InvalidPayload,
            context,
            std::string("missing field ") + std::string(field));
    }

    for (const auto& entry : object.items())
    {
        bool known = false;
        for (const std::string_view field : required)
        {
            if (entry.key() == field)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            for (const std::string_view field : optional)
            {
                if (entry.key() == field)
                {
                    known = true;
                    break;
                }
            }
        }
        Require(
            known,
            EditorErrorCode::InvalidPayload,
            context,
            std::string("contains unknown field ") + entry.key());
    }
}

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
    std::string_view context)
{
    Require(
        value.is_number(),
        EditorErrorCode::InvalidPayload,
        context,
        "must be a number");
    const double doubleValue = value.get<double>();
    const float floatValue = static_cast<float>(doubleValue);
    Require(
        std::isfinite(doubleValue) && std::isfinite(floatValue),
        EditorErrorCode::InvalidPayload,
        context,
        "must be finite and representable as float");
    return floatValue;
}

std::string ReadString(
    const Json& object,
    std::string_view field,
    std::string_view context,
    bool requireNonEmpty = true)
{
    RequireObject(object, context);
    const auto iterator = object.find(std::string(field));
    Require(
        iterator != object.end() && iterator->is_string(),
        EditorErrorCode::InvalidPayload,
        context,
        std::string(field) + " must be a string");
    const std::string value = iterator->get<std::string>();
    if (requireNonEmpty)
    {
        Require(
            !value.empty(),
            EditorErrorCode::InvalidPayload,
            context,
            std::string(field) + " must not be empty");
    }
    return value;
}

bool ReadBoolean(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    RequireObject(object, context);
    const auto iterator = object.find(std::string(field));
    Require(
        iterator != object.end() && iterator->is_boolean(),
        EditorErrorCode::InvalidPayload,
        context,
        std::string(field) + " must be a boolean");
    return iterator->get<bool>();
}

std::string NormalizeRelativeAssetPath(
    std::string_view rawPath,
    std::string_view context,
    bool requireTextureAsset = false)
{
    const EditorErrorCode pathErrorCode = requireTextureAsset
        ? EditorErrorCode::InvalidTextureAssetReference
        : EditorErrorCode::InvalidPayload;
    Require(
        !rawPath.empty(),
        pathErrorCode,
        context,
        "asset path must not be empty");

    std::string portablePath(rawPath);
    for (char& character : portablePath)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }

    const std::filesystem::path path(portablePath);
    Require(
        !path.empty() && !path.is_absolute() && path.root_name().empty() &&
            path.root_directory().empty(),
        pathErrorCode,
        context,
        "asset path must be relative");

    const std::filesystem::path normalizedPath = path.lexically_normal();
    Require(
        !normalizedPath.empty() && normalizedPath != "." &&
            normalizedPath != "..",
        pathErrorCode,
        context,
        "asset path must identify a file");

    for (const auto& component : normalizedPath)
    {
        Require(
            component != "..",
            pathErrorCode,
            context,
            "asset path must not traverse outside the resource root");
    }

    const std::string normalized = normalizedPath.generic_string();
    Require(
        !normalized.empty() && normalized.front() != '/',
        pathErrorCode,
        context,
        "asset path must remain relative");
    Require(
        normalizedPath.extension() == ".json",
        pathErrorCode,
        context,
        requireTextureAsset
            ? "texture binding must reference a T_*.json asset"
            : "asset path must use a JSON asset file");

    if (requireTextureAsset)
    {
        const std::string filename = normalizedPath.filename().string();
        Require(
            filename.rfind("T_", 0) == 0,
            EditorErrorCode::InvalidTextureAssetReference,
            context,
            "texture binding must reference a T_*.json asset");
    }
    return normalized;
}

std::string ReadAssetPath(
    const Json& object,
    std::string_view field,
    std::string_view context,
    bool requireTextureAsset = false)
{
    return NormalizeRelativeAssetPath(
        ReadString(object, field, context),
        context,
        requireTextureAsset);
}

std::string_view RequireWireName(
    std::string_view name,
    std::string_view context)
{
    Require(
        !name.empty(),
        EditorErrorCode::InvalidPayload,
        context,
        "enum has no stable wire name");
    return name;
}

EditorCommandSource DecodeSource(
    std::string_view name)
{
    if (name == "imgui") return EditorCommandSource::ImGui;
    if (name == "console") return EditorCommandSource::Console;
    if (name == "ai") return EditorCommandSource::AI;
    if (name == "runtime_test") return EditorCommandSource::RuntimeTest;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandEnvelope.source",
        std::string("unknown lower-case source name '") +
            std::string(name) + "'");
}

EditorCommandStatus DecodeStatus(
    std::string_view name)
{
    if (name == "accepted") return EditorCommandStatus::Accepted;
    if (name == "running") return EditorCommandStatus::Running;
    if (name == "succeeded") return EditorCommandStatus::Succeeded;
    if (name == "rejected") return EditorCommandStatus::Rejected;
    if (name == "failed") return EditorCommandStatus::Failed;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.status",
        std::string("unknown lower-case status name '") +
            std::string(name) + "'");
}

EditorErrorCode DecodeErrorCode(
    std::string_view name)
{
    if (name == "none") return EditorErrorCode::None;
    if (name == "invalid_protocol_version") return EditorErrorCode::InvalidProtocolVersion;
    if (name == "invalid_command_id") return EditorErrorCode::InvalidCommandId;
    if (name == "invalid_command_type") return EditorErrorCode::InvalidCommandType;
    if (name == "invalid_payload") return EditorErrorCode::InvalidPayload;
    if (name == "missing_expected_document_revision") return EditorErrorCode::MissingExpectedDocumentRevision;
    if (name == "stale_document_revision") return EditorErrorCode::StaleDocumentRevision;
    if (name == "duplicate_command_id") return EditorErrorCode::DuplicateCommandId;
    if (name == "result_store_capacity_exceeded") return EditorErrorCode::ResultStoreCapacityExceeded;
    if (name == "asset_not_found") return EditorErrorCode::AssetNotFound;
    if (name == "invalid_asset_type") return EditorErrorCode::InvalidAssetType;
    if (name == "reference_resolution_failed") return EditorErrorCode::ReferenceResolutionFailed;
    if (name == "document_not_open") return EditorErrorCode::DocumentNotOpen;
    if (name == "document_dirty") return EditorErrorCode::DocumentDirty;
    if (name == "unknown_parameter") return EditorErrorCode::UnknownParameter;
    if (name == "parameter_type_mismatch") return EditorErrorCode::ParameterTypeMismatch;
    if (name == "unknown_texture_slot") return EditorErrorCode::UnknownTextureSlot;
    if (name == "invalid_texture_asset_reference") return EditorErrorCode::InvalidTextureAssetReference;
    if (name == "source_changed") return EditorErrorCode::SourceChanged;
    if (name == "validation_failed") return EditorErrorCode::ValidationFailed;
    if (name == "atomic_write_failed") return EditorErrorCode::AtomicWriteFailed;
    if (name == "preview_unavailable") return EditorErrorCode::PreviewUnavailable;
    if (name == "preview_generation_changed") return EditorErrorCode::PreviewGenerationChanged;
    if (name == "preview_prepare_failed") return EditorErrorCode::PreviewPrepareFailed;
    if (name == "preview_commit_failed") return EditorErrorCode::PreviewCommitFailed;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.errorCode",
        std::string("unknown lower-case error name '") +
            std::string(name) + "'");
}

EditorMaterialParameterType DecodeParameterType(
    std::string_view name)
{
    if (name == "float") return EditorMaterialParameterType::Float;
    if (name == "vec2") return EditorMaterialParameterType::Vec2;
    if (name == "vec3") return EditorMaterialParameterType::Vec3;
    if (name == "vec4") return EditorMaterialParameterType::Vec4;
    if (name == "color") return EditorMaterialParameterType::Color;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "material.set_parameter.type",
        std::string("unknown lower-case parameter type '") +
            std::string(name) + "'");
}

EditorDirtyDocumentPolicy DecodeDirtyPolicy(
    std::string_view name)
{
    if (name == "require_clean") return EditorDirtyDocumentPolicy::RequireClean;
    if (name == "discard_changes") return EditorDirtyDocumentPolicy::DiscardChanges;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "dirtyPolicy",
        std::string("unknown lower-case dirty policy '") +
            std::string(name) + "'");
}

EditorResetScope DecodeResetScope(
    std::string_view name)
{
    if (name == "parameters") return EditorResetScope::Parameters;
    if (name == "textures") return EditorResetScope::Textures;
    if (name == "render_states") return EditorResetScope::RenderStates;
    if (name == "all") return EditorResetScope::All;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "material.reset_overrides.scope",
        std::string("unknown lower-case reset scope '") +
            std::string(name) + "'");
}

EditorMaterialRenderStateField DecodeRenderStateField(
    std::string_view name,
    std::string_view context)
{
    try
    {
        return ParseEditorMaterialRenderStateField(name);
    }
    catch (const std::exception&)
    {
        ThrowCodecError(
            EditorErrorCode::InvalidPayload,
            context,
            std::string("unknown render state field '") + std::string(name) + "'");
    }
}

EditorMaterialRenderStateValue DecodeRenderStateValue(
    EditorMaterialRenderStateField field,
    std::string_view name,
    std::string_view context)
{
    try
    {
        switch (field)
        {
        case EditorMaterialRenderStateField::RenderMode:
            return ParseEditorMaterialRenderMode(name);
        case EditorMaterialRenderStateField::CullMode:
            return ParseEditorMaterialCullMode(name);
        case EditorMaterialRenderStateField::ShadingModel:
            return ParseEditorMaterialShadingModel(name);
        }
    }
    catch (const std::exception&)
    {
    }
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        context,
        std::string("invalid value '") + std::string(name) + "' for render state field");
}

EditorCommandType DecodeCommandType(
    std::string_view name)
{
    const EditorCommandSpec* spec = FindEditorCommandSpec(name);
    Require(
        spec != nullptr,
        EditorErrorCode::InvalidCommandType,
        "EditorCommandEnvelope.command",
        std::string("unknown lower-case command name '") +
            std::string(name) + "'");
    return spec->type;
}

uint32_t DecodeSectionSelector(
    std::string_view selector,
    std::string_view context)
{
    Require(
        !selector.empty(),
        EditorErrorCode::InvalidPayload,
        context,
        "selector must not be empty");

    uint32_t section = 0;
    const auto conversion = std::from_chars(
        selector.data(),
        selector.data() + selector.size(),
        section,
        10);
    Require(
        conversion.ec == std::errc() && conversion.ptr == selector.data() + selector.size(),
        EditorErrorCode::InvalidPayload,
        context,
        "selector must be a decimal mesh section for this typed payload");
    return section;
}

Json EncodeParameterValue(
    const EditorMaterialParameterValue& value,
    std::string_view context)
{
    Json result = Json::array();
    std::visit(
        [&result, context](const auto& item) {
            using ValueType = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<ValueType, float>)
            {
                Require(
                    std::isfinite(item),
                    EditorErrorCode::InvalidPayload,
                    context,
                    "value must be finite");
                result.push_back(item);
            }
            else if constexpr (std::is_same_v<ValueType, EditorColor>)
            {
                for (const float component : item.components)
                {
                    Require(
                        std::isfinite(component),
                        EditorErrorCode::InvalidPayload,
                        context,
                        "value components must be finite");
                    result.push_back(component);
                }
            }
            else
            {
                for (const float component : item)
                {
                    Require(
                        std::isfinite(component),
                        EditorErrorCode::InvalidPayload,
                        context,
                        "value components must be finite");
                    result.push_back(component);
                }
            }
        },
        value);
    return result;
}

EditorMaterialParameterValue DecodeParameterValue(
    const Json& value,
    EditorMaterialParameterType type,
    std::string_view context)
{
    Require(
        value.is_array(),
        EditorErrorCode::InvalidPayload,
        context,
        "value must be an array");

    const std::size_t expectedCount =
        type == EditorMaterialParameterType::Float
            ? 1
            : type == EditorMaterialParameterType::Vec2
                ? 2
                : type == EditorMaterialParameterType::Vec3 ? 3 : 4;
    Require(
        value.size() == expectedCount,
        EditorErrorCode::InvalidPayload,
        context,
        "value array has the wrong vector arity");

    std::array<float, 4> components{};
    for (std::size_t index = 0; index < expectedCount; ++index)
    {
        components[index] = ReadFiniteFloat(
            value.at(index),
            std::string(context) + "[" + std::to_string(index) + "]");
    }

    switch (type)
    {
    case EditorMaterialParameterType::Float:
        return components[0];
    case EditorMaterialParameterType::Vec2:
        return EditorVec2{components[0], components[1]};
    case EditorMaterialParameterType::Vec3:
        return EditorVec3{components[0], components[1], components[2]};
    case EditorMaterialParameterType::Vec4:
        return EditorVec4{
            components[0],
            components[1],
            components[2],
            components[3]};
    case EditorMaterialParameterType::Color:
        return EditorColor{{
            components[0],
            components[1],
            components[2],
            components[3]}};
    }
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        context,
        "unsupported parameter type");
}

Json EncodeNavigationOrigin(
    const EditorNavigationOrigin& origin)
{
    Require(
        !origin.sceneIdentity.empty() && !origin.objectIdentity.empty(),
        EditorErrorCode::InvalidPayload,
        "navigationOrigin",
        "scene and object identities must not be empty");

    Json result = {
        {"scenePath", NormalizeRelativeAssetPath(
                           origin.sceneIdentity,
                           "navigationOrigin.scenePath")},
        {"objectId", origin.objectIdentity}};
    if (origin.section.has_value())
    {
        result["section"] = *origin.section;
    }
    if (!origin.slot.empty())
    {
        result["slot"] = origin.slot;
    }
    return result;
}

EditorNavigationOrigin DecodeNavigationOrigin(
    const Json& wire)
{
    RequireAllowedFields(
        wire,
        {"scenePath", "objectId"},
        {"section", "slot"},
        "navigationOrigin");

    EditorNavigationOrigin origin;
    origin.sceneIdentity = ReadAssetPath(
        wire,
        "scenePath",
        "navigationOrigin");
    origin.objectIdentity = ReadString(
        wire,
        "objectId",
        "navigationOrigin");
    if (ContainsField(wire, "section"))
    {
        origin.section = ReadUnsigned<uint32_t>(
            wire,
            "section",
            "navigationOrigin");
    }
    if (ContainsField(wire, "slot"))
    {
        origin.slot = ReadString(wire, "slot", "navigationOrigin");
    }
    return origin;
}

Json EncodePayload(
    EditorCommandType type,
    const EditorCommandPayload& payload);

EditorCommandPayload DecodePayload(
    EditorCommandType type,
    const Json& wire);

bool IsBatchCommandType(
    EditorCommandType type) noexcept
{
    switch (type)
    {
    case EditorCommandType::SetMaterialParameterOverride:
    case EditorCommandType::ClearMaterialParameterOverride:
    case EditorCommandType::SetMaterialTextureOverride:
    case EditorCommandType::ClearMaterialTextureOverride:
    case EditorCommandType::SetMaterialRenderStateOverride:
    case EditorCommandType::ClearMaterialRenderStateOverride:
    case EditorCommandType::ResetMaterialInstanceOverrides:
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
    case EditorCommandType::ReloadMaterialInstanceDocument:
    case EditorCommandType::ApplyMaterialInstancePreview:
        return true;
    default:
        return false;
    }
}

EditorCommandBatchItemPayload ToBatchPayload(
    EditorCommandType type,
    const EditorCommandPayload& payload)
{
    switch (type)
    {
    case EditorCommandType::SetMaterialParameterOverride:
        if (const auto* value = std::get_if<SetMaterialParameterOverridePayload>(&payload)) return *value;
        break;
    case EditorCommandType::ClearMaterialParameterOverride:
        if (const auto* value = std::get_if<ClearMaterialParameterOverridePayload>(&payload)) return *value;
        break;
    case EditorCommandType::SetMaterialTextureOverride:
        if (const auto* value = std::get_if<SetMaterialTextureOverridePayload>(&payload)) return *value;
        break;
    case EditorCommandType::ClearMaterialTextureOverride:
        if (const auto* value = std::get_if<ClearMaterialTextureOverridePayload>(&payload)) return *value;
        break;
    case EditorCommandType::SetMaterialRenderStateOverride:
        if (const auto* value = std::get_if<SetMaterialRenderStateOverridePayload>(&payload)) return *value;
        break;
    case EditorCommandType::ClearMaterialRenderStateOverride:
        if (const auto* value = std::get_if<ClearMaterialRenderStateOverridePayload>(&payload)) return *value;
        break;
    case EditorCommandType::ResetMaterialInstanceOverrides:
        if (const auto* value = std::get_if<ResetMaterialInstanceOverridesPayload>(&payload)) return *value;
        break;
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
        if (const auto* value = std::get_if<MaterialInstanceAssetPathPayload>(&payload)) return *value;
        break;
    case EditorCommandType::ReloadMaterialInstanceDocument:
        if (const auto* value = std::get_if<ReloadMaterialInstanceDocumentPayload>(&payload)) return *value;
        break;
    case EditorCommandType::ApplyMaterialInstancePreview:
        if (const auto* value = std::get_if<ApplyMaterialInstancePreviewPayload>(&payload)) return *value;
        break;
    default:
        break;
    }

    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "editor.execute_batch.payload",
        "payload type does not match batch command");
}

Json EncodeBatchItemPayload(
    EditorCommandType type,
    const EditorCommandBatchItemPayload& payload)
{
    switch (type)
    {
    case EditorCommandType::SetMaterialParameterOverride:
        if (const auto* value = std::get_if<SetMaterialParameterOverridePayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::ClearMaterialParameterOverride:
        if (const auto* value = std::get_if<ClearMaterialParameterOverridePayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::SetMaterialTextureOverride:
        if (const auto* value = std::get_if<SetMaterialTextureOverridePayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::ClearMaterialTextureOverride:
        if (const auto* value = std::get_if<ClearMaterialTextureOverridePayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::SetMaterialRenderStateOverride:
        if (const auto* value = std::get_if<SetMaterialRenderStateOverridePayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::ClearMaterialRenderStateOverride:
        if (const auto* value = std::get_if<ClearMaterialRenderStateOverridePayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::ResetMaterialInstanceOverrides:
        if (const auto* value = std::get_if<ResetMaterialInstanceOverridesPayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
        if (const auto* value = std::get_if<MaterialInstanceAssetPathPayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::ReloadMaterialInstanceDocument:
        if (const auto* value = std::get_if<ReloadMaterialInstanceDocumentPayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    case EditorCommandType::ApplyMaterialInstancePreview:
        if (const auto* value = std::get_if<ApplyMaterialInstancePreviewPayload>(&payload))
            return EncodePayload(type, EditorCommandPayload{*value});
        break;
    default:
        break;
    }

    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "editor.execute_batch.payload",
        "payload type does not match batch command");
}

Json EncodeBatchPayload(
    const ExecuteEditorCommandBatchPayload& batch)
{
    Require(
        !batch.commands.empty(),
        EditorErrorCode::InvalidPayload,
        "editor.execute_batch.commands",
        "batch must contain at least one command");

    Json commands = Json::array();
    for (const EditorCommandBatchItem& item : batch.commands)
    {
        Require(
            IsBatchCommandType(item.type),
            EditorErrorCode::InvalidPayload,
            "editor.execute_batch.commands",
            "contains a command that is not batch-compatible");
        commands.push_back({
            {"command", std::string(RequireWireName(
                             GetEditorCommandName(item.type),
                             "editor.execute_batch.command"))},
            {"payload", EncodeBatchItemPayload(item.type, item.payload)}});
    }
    return Json{{"commands", std::move(commands)}};
}

EditorCommandBatchItem DecodeBatchItem(
    const Json& wire)
{
    RequireAllowedFields(
        wire,
        {"command", "payload"},
        {},
        "editor.execute_batch.commands[]");
    const EditorCommandType type = DecodeCommandType(ReadString(
        wire,
        "command",
        "editor.execute_batch.commands[]"));
    Require(
        IsBatchCommandType(type),
        EditorErrorCode::InvalidPayload,
        "editor.execute_batch.commands[]",
        "command is not batch-compatible");
    const EditorCommandPayload payload = DecodePayload(
        type,
        wire.at("payload"));
    return EditorCommandBatchItem{type, ToBatchPayload(type, payload)};
}

EditorCommandPayload DecodeBatchPayload(
    const Json& wire)
{
    RequireAllowedFields(
        wire,
        {"commands"},
        {},
        "editor.execute_batch");
    const Json& commands = wire.at("commands");
    Require(
        commands.is_array() && !commands.empty(),
        EditorErrorCode::InvalidPayload,
        "editor.execute_batch.commands",
        "must be a non-empty array");

    ExecuteEditorCommandBatchPayload batch;
    batch.commands.reserve(commands.size());
    for (const Json& item : commands)
    {
        batch.commands.push_back(DecodeBatchItem(item));
    }
    return batch;
}

Json EncodePayload(
    EditorCommandType type,
    const EditorCommandPayload& payload)
{
    switch (type)
    {
    case EditorCommandType::ResolveSceneMaterialAsset:
    {
        const auto* value = std::get_if<ResolveSceneMaterialAssetPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.resolve_scene_reference", "payload type mismatch");
        Json result = {
            {"scenePath", NormalizeRelativeAssetPath(value->sceneIdentity, "material.resolve_scene_reference.scenePath")},
            {"objectId", value->objectIdentity}};
        Require(!value->objectIdentity.empty(), EditorErrorCode::InvalidPayload, "material.resolve_scene_reference", "object identity must not be empty");
        if (value->section.has_value()) result["selector"] = std::to_string(*value->section);
        return result;
    }
    case EditorCommandType::ListMaterialInstanceAssets:
    {
        const auto* value = std::get_if<ListMaterialInstanceAssetsPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.list_assets", "payload type mismatch");
        Require(value->pageSize > 0, EditorErrorCode::InvalidPayload, "material.list_assets", "pageSize must be greater than zero");
        return Json{
            {"search", value->searchText},
            {"page", value->pageIndex},
            {"pageSize", value->pageSize}};
    }
    case EditorCommandType::OpenMaterialInstanceAsset:
    {
        const auto* value = std::get_if<OpenMaterialInstanceAssetPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.open", "payload type mismatch");
        Json result = {{"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.open.assetPath")}};
        if (value->origin.has_value()) result["navigationOrigin"] = EncodeNavigationOrigin(*value->origin);
        return result;
    }
    case EditorCommandType::OpenTextureAsset:
    {
        const auto* value = std::get_if<OpenTextureAssetPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "texture.open", "payload type mismatch");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "texture.open.assetPath")},
            {"textureAssetPath", NormalizeRelativeAssetPath(value->textureAssetPath, "texture.open.textureAssetPath", true)}};
    }
    case EditorCommandType::SelectMaterialInstanceDocument:
    case EditorCommandType::GetMaterialInstanceDocument:
    case EditorCommandType::GetMaterialInstanceReferenceContext:
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
    {
        const auto* value = std::get_if<MaterialInstanceAssetPathPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "asset command", "payload type mismatch");
        return Json{{"assetPath", NormalizeRelativeAssetPath(value->assetPath, "asset command.assetPath")}};
    }
    case EditorCommandType::CloseMaterialInstanceAsset:
    {
        const auto* value = std::get_if<CloseMaterialInstanceAssetPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.close", "payload type mismatch");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.close.assetPath")},
            {"dirtyPolicy", std::string(RequireWireName(
                                 value->dirtyPolicy == EditorDirtyDocumentPolicy::RequireClean
                                     ? "require_clean"
                                     : value->dirtyPolicy == EditorDirtyDocumentPolicy::DiscardChanges
                                         ? "discard_changes"
                                         : "",
                                 "material.close.dirtyPolicy"))}};
    }
    case EditorCommandType::SetMaterialParameterOverride:
    {
        const auto* value = std::get_if<SetMaterialParameterOverridePayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.set_parameter", "payload type mismatch");
        Require(
            GetEditorMaterialParameterType(value->value) == value->parameterType,
            EditorErrorCode::InvalidPayload,
            "material.set_parameter",
            "parameter type does not match value variant");
        const std::string_view typeName =
            value->parameterType == EditorMaterialParameterType::Float
                ? "float"
                : value->parameterType == EditorMaterialParameterType::Vec2
                    ? "vec2"
                    : value->parameterType == EditorMaterialParameterType::Vec3 ? "vec3"
                    : value->parameterType == EditorMaterialParameterType::Vec4 ? "vec4" : "color";
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.set_parameter.assetPath")},
            {"parameter", value->parameter},
            {"type", std::string(typeName)},
            {"value", EncodeParameterValue(value->value, "material.set_parameter.value")}};
    }
    case EditorCommandType::ClearMaterialParameterOverride:
    {
        const auto* value = std::get_if<ClearMaterialParameterOverridePayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.clear_parameter", "payload type mismatch");
        Require(!value->parameter.empty(), EditorErrorCode::InvalidPayload, "material.clear_parameter", "parameter must not be empty");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.clear_parameter.assetPath")},
            {"parameter", value->parameter}};
    }
    case EditorCommandType::SetMaterialTextureOverride:
    {
        const auto* value = std::get_if<SetMaterialTextureOverridePayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.set_texture", "payload type mismatch");
        Require(!value->slot.empty(), EditorErrorCode::InvalidPayload, "material.set_texture", "slot must not be empty");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.set_texture.assetPath")},
            {"slot", value->slot},
            {"textureAssetPath", NormalizeRelativeAssetPath(value->textureAssetPath, "material.set_texture.textureAssetPath", true)}};
    }
    case EditorCommandType::ClearMaterialTextureOverride:
    {
        const auto* value = std::get_if<ClearMaterialTextureOverridePayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.clear_texture", "payload type mismatch");
        Require(!value->slot.empty(), EditorErrorCode::InvalidPayload, "material.clear_texture", "slot must not be empty");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.clear_texture.assetPath")},
            {"slot", value->slot}};
    }
    case EditorCommandType::SetMaterialRenderStateOverride:
    {
        const auto* value =
            std::get_if<SetMaterialRenderStateOverridePayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload,
            "material.set_render_state", "payload type mismatch");
        Require(
            IsEditorMaterialRenderStateValueCompatible(value->field, value->value),
            EditorErrorCode::InvalidPayload,
            "material.set_render_state",
            "value does not match field");
        const std::string fieldName(
            GetEditorMaterialRenderStateFieldName(value->field));
        const std::string valueName = std::visit(
            [](const auto& typedValue)
            {
                using ValueType = std::decay_t<decltype(typedValue)>;
                if constexpr (std::is_same_v<ValueType, EditorMaterialRenderMode>)
                    return std::string(GetEditorMaterialRenderModeName(typedValue));
                else if constexpr (std::is_same_v<ValueType, EditorMaterialCullMode>)
                    return std::string(GetEditorMaterialCullModeName(typedValue));
                else
                    return std::string(GetEditorMaterialShadingModelName(typedValue));
            },
            value->value);
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.set_render_state.assetPath")},
            {"field", fieldName},
            {"value", valueName}};
    }
    case EditorCommandType::ClearMaterialRenderStateOverride:
    {
        const auto* value =
            std::get_if<ClearMaterialRenderStateOverridePayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload,
            "material.clear_render_state", "payload type mismatch");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.clear_render_state.assetPath")},
            {"field", std::string(GetEditorMaterialRenderStateFieldName(value->field))}};
    }
    case EditorCommandType::ResetMaterialInstanceOverrides:
    {
        const auto* value = std::get_if<ResetMaterialInstanceOverridesPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.reset_overrides", "payload type mismatch");
        const std::string_view scopeName =
            value->scope == EditorResetScope::Parameters
                ? "parameters"
                : value->scope == EditorResetScope::Textures ? "textures"
                : value->scope == EditorResetScope::RenderStates ? "render_states"
                : value->scope == EditorResetScope::All ? "all" : "";
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.reset_overrides.assetPath")},
            {"scope", std::string(RequireWireName(scopeName, "material.reset_overrides.scope"))}};
    }
    case EditorCommandType::ReloadMaterialInstanceDocument:
    {
        const auto* value = std::get_if<ReloadMaterialInstanceDocumentPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.reload", "payload type mismatch");
        const std::string_view policyName =
            value->dirtyPolicy == EditorDirtyDocumentPolicy::RequireClean
                ? "require_clean"
                : value->dirtyPolicy == EditorDirtyDocumentPolicy::DiscardChanges ? "discard_changes" : "";
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.reload.assetPath")},
            {"dirtyPolicy", std::string(RequireWireName(policyName, "material.reload.dirtyPolicy"))}};
    }
    case EditorCommandType::ApplyMaterialInstancePreview:
    {
        const auto* value = std::get_if<ApplyMaterialInstancePreviewPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "material.preview.apply", "payload type mismatch");
        Require(value->documentRevision > 0, EditorErrorCode::InvalidPayload, "material.preview.apply", "documentRevision must be non-zero");
        return Json{
            {"assetPath", NormalizeRelativeAssetPath(value->assetPath, "material.preview.apply.assetPath")},
            {"documentRevision", value->documentRevision}};
    }
    case EditorCommandType::GetEditorCommandResult:
    {
        const auto* value = std::get_if<GetEditorCommandResultPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "editor.get_command_result", "payload type mismatch");
        Require(value->commandId > 0, EditorErrorCode::InvalidCommandId, "editor.get_command_result", "commandId must be non-zero");
        return Json{{"commandId", value->commandId}};
    }
    case EditorCommandType::ListEditorEvents:
    {
        const auto* value = std::get_if<ListEditorEventsPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "editor.list_events", "payload type mismatch");
        Require(value->limit > 0, EditorErrorCode::InvalidPayload, "editor.list_events", "limit must be greater than zero");
        return Json{
            {"afterEventId", value->afterEventId},
            {"limit", value->limit}};
    }
    case EditorCommandType::ExecuteEditorCommandBatch:
    {
        const auto* value = std::get_if<ExecuteEditorCommandBatchPayload>(&payload);
        Require(value != nullptr, EditorErrorCode::InvalidPayload, "editor.execute_batch", "payload type mismatch");
        return EncodeBatchPayload(*value);
    }
    }

    ThrowCodecError(
        EditorErrorCode::InvalidCommandType,
        "EditorCommandEnvelope.payload",
        "unsupported command type");
}

EditorCommandPayload DecodePayload(
    EditorCommandType type,
    const Json& wire)
{
    switch (type)
    {
    case EditorCommandType::ResolveSceneMaterialAsset:
    {
        RequireAllowedFields(
            wire,
            {"scenePath", "objectId"},
            {"selector"},
            "material.resolve_scene_reference.payload");
        ResolveSceneMaterialAssetPayload value;
        value.sceneIdentity = ReadAssetPath(wire, "scenePath", "material.resolve_scene_reference.payload");
        value.objectIdentity = ReadString(wire, "objectId", "material.resolve_scene_reference.payload");
        if (ContainsField(wire, "selector"))
        {
            value.section = DecodeSectionSelector(
                ReadString(wire, "selector", "material.resolve_scene_reference.payload"),
                "material.resolve_scene_reference.selector");
        }
        return value;
    }
    case EditorCommandType::ListMaterialInstanceAssets:
    {
        RequireAllowedFields(
            wire,
            {},
            {"search", "page", "pageSize"},
            "material.list_assets.payload");
        ListMaterialInstanceAssetsPayload value;
        if (ContainsField(wire, "search")) value.searchText = ReadString(wire, "search", "material.list_assets.payload", false);
        if (ContainsField(wire, "page")) value.pageIndex = ReadUnsigned<uint32_t>(wire, "page", "material.list_assets.payload");
        if (ContainsField(wire, "pageSize")) value.pageSize = ReadUnsigned<uint32_t>(wire, "pageSize", "material.list_assets.payload");
        Require(value.pageSize > 0, EditorErrorCode::InvalidPayload, "material.list_assets.payload", "pageSize must be greater than zero");
        return value;
    }
    case EditorCommandType::OpenMaterialInstanceAsset:
    {
        RequireAllowedFields(
            wire,
            {"assetPath"},
            {"navigationOrigin"},
            "material.open.payload");
        OpenMaterialInstanceAssetPayload value;
        value.assetPath = ReadAssetPath(wire, "assetPath", "material.open.payload");
        if (ContainsField(wire, "navigationOrigin"))
        {
            value.origin = DecodeNavigationOrigin(wire.at("navigationOrigin"));
        }
        return value;
    }
    case EditorCommandType::OpenTextureAsset:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "textureAssetPath"},
            {},
            "texture.open.payload");
        return OpenTextureAssetPayload{
            ReadAssetPath(wire, "assetPath", "texture.open.payload"),
            ReadAssetPath(wire, "textureAssetPath", "texture.open.payload", true)};
    }
    case EditorCommandType::SelectMaterialInstanceDocument:
    case EditorCommandType::GetMaterialInstanceDocument:
    case EditorCommandType::GetMaterialInstanceReferenceContext:
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
    {
        RequireAllowedFields(wire, {"assetPath"}, {}, "asset command.payload");
        return MaterialInstanceAssetPathPayload{
            ReadAssetPath(wire, "assetPath", "asset command.payload")};
    }
    case EditorCommandType::CloseMaterialInstanceAsset:
    {
        RequireAllowedFields(
            wire,
            {"assetPath"},
            {"dirtyPolicy"},
            "material.close.payload");
        CloseMaterialInstanceAssetPayload value;
        value.assetPath = ReadAssetPath(wire, "assetPath", "material.close.payload");
        if (ContainsField(wire, "dirtyPolicy"))
        {
            value.dirtyPolicy = DecodeDirtyPolicy(
                ReadString(wire, "dirtyPolicy", "material.close.payload"));
        }
        return value;
    }
    case EditorCommandType::SetMaterialParameterOverride:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "parameter", "type", "value"},
            {},
            "material.set_parameter.payload");
        const EditorMaterialParameterType parameterType = DecodeParameterType(
            ReadString(wire, "type", "material.set_parameter.payload"));
        SetMaterialParameterOverridePayload value;
        value.assetPath = ReadAssetPath(wire, "assetPath", "material.set_parameter.payload");
        value.parameter = ReadString(wire, "parameter", "material.set_parameter.payload");
        value.parameterType = parameterType;
        value.value = DecodeParameterValue(
            wire.at("value"),
            parameterType,
            "material.set_parameter.value");
        return value;
    }
    case EditorCommandType::ClearMaterialParameterOverride:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "parameter"},
            {},
            "material.clear_parameter.payload");
        return ClearMaterialParameterOverridePayload{
            ReadAssetPath(wire, "assetPath", "material.clear_parameter.payload"),
            ReadString(wire, "parameter", "material.clear_parameter.payload")};
    }
    case EditorCommandType::SetMaterialTextureOverride:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "slot", "textureAssetPath"},
            {},
            "material.set_texture.payload");
        return SetMaterialTextureOverridePayload{
            ReadAssetPath(wire, "assetPath", "material.set_texture.payload"),
            ReadString(wire, "slot", "material.set_texture.payload"),
            ReadAssetPath(wire, "textureAssetPath", "material.set_texture.payload", true)};
    }
    case EditorCommandType::ClearMaterialTextureOverride:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "slot"},
            {},
            "material.clear_texture.payload");
        return ClearMaterialTextureOverridePayload{
            ReadAssetPath(wire, "assetPath", "material.clear_texture.payload"),
            ReadString(wire, "slot", "material.clear_texture.payload")};
    }
    case EditorCommandType::SetMaterialRenderStateOverride:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "field", "value"},
            {},
            "material.set_render_state.payload");
        const std::string context = "material.set_render_state.payload";
        const EditorMaterialRenderStateField field = DecodeRenderStateField(
            ReadString(wire, "field", context),
            context);
        const std::string valueName = ReadString(wire, "value", context);
        return SetMaterialRenderStateOverridePayload{
            ReadAssetPath(wire, "assetPath", context),
            field,
            DecodeRenderStateValue(field, valueName, context)};
    }
    case EditorCommandType::ClearMaterialRenderStateOverride:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "field"},
            {},
            "material.clear_render_state.payload");
        const std::string context = "material.clear_render_state.payload";
        return ClearMaterialRenderStateOverridePayload{
            ReadAssetPath(wire, "assetPath", context),
            DecodeRenderStateField(ReadString(wire, "field", context), context)};
    }
    case EditorCommandType::ResetMaterialInstanceOverrides:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "scope"},
            {},
            "material.reset_overrides.payload");
        return ResetMaterialInstanceOverridesPayload{
            ReadAssetPath(wire, "assetPath", "material.reset_overrides.payload"),
            DecodeResetScope(ReadString(wire, "scope", "material.reset_overrides.payload"))};
    }
    case EditorCommandType::ReloadMaterialInstanceDocument:
    {
        RequireAllowedFields(
            wire,
            {"assetPath"},
            {"dirtyPolicy"},
            "material.reload.payload");
        ReloadMaterialInstanceDocumentPayload value;
        value.assetPath = ReadAssetPath(wire, "assetPath", "material.reload.payload");
        if (ContainsField(wire, "dirtyPolicy"))
        {
            value.dirtyPolicy = DecodeDirtyPolicy(
                ReadString(wire, "dirtyPolicy", "material.reload.payload"));
        }
        return value;
    }
    case EditorCommandType::ApplyMaterialInstancePreview:
    {
        RequireAllowedFields(
            wire,
            {"assetPath", "documentRevision"},
            {},
            "material.preview.apply.payload");
        const EditorDocumentRevision revision = ReadUnsigned<uint64_t>(
            wire,
            "documentRevision",
            "material.preview.apply.payload");
        Require(
            revision > 0,
            EditorErrorCode::InvalidPayload,
            "material.preview.apply.payload",
            "documentRevision must be non-zero");
        return ApplyMaterialInstancePreviewPayload{
            ReadAssetPath(wire, "assetPath", "material.preview.apply.payload"),
            revision};
    }
    case EditorCommandType::GetEditorCommandResult:
    {
        RequireAllowedFields(
            wire,
            {"commandId"},
            {},
            "editor.get_command_result.payload");
        return GetEditorCommandResultPayload{
            ReadUnsigned<uint64_t>(wire, "commandId", "editor.get_command_result.payload")};
    }
    case EditorCommandType::ListEditorEvents:
    {
        RequireAllowedFields(
            wire,
            {},
            {"afterEventId", "limit"},
            "editor.list_events.payload");
        ListEditorEventsPayload value;
        if (ContainsField(wire, "afterEventId")) value.afterEventId = ReadUnsigned<uint64_t>(wire, "afterEventId", "editor.list_events.payload");
        if (ContainsField(wire, "limit")) value.limit = ReadUnsigned<uint32_t>(wire, "limit", "editor.list_events.payload");
        Require(value.limit > 0, EditorErrorCode::InvalidPayload, "editor.list_events.payload", "limit must be greater than zero");
        return value;
    }
    case EditorCommandType::ExecuteEditorCommandBatch:
        return DecodeBatchPayload(wire);
    }

    ThrowCodecError(
        EditorErrorCode::InvalidCommandType,
        "EditorCommandEnvelope.payload",
        "unsupported command type");
}

void ValidateEnvelopeForCodec(
    const EditorCommandEnvelope& command)
{
    Require(
        command.protocolVersion == kEditorCommandProtocolVersion,
        EditorErrorCode::InvalidProtocolVersion,
        "EditorCommandEnvelope.protocolVersion",
        "unsupported protocol version");
    Require(
        command.commandId != 0,
        EditorErrorCode::InvalidCommandId,
        "EditorCommandEnvelope.commandId",
        "commandId must be non-zero");
    Require(
        !GetEditorCommandJsonSourceName(command.source).empty(),
        EditorErrorCode::InvalidPayload,
        "EditorCommandEnvelope.source",
        "source has no stable lower-case wire name");
    Require(
        FindEditorCommandSpec(command.type) != nullptr,
        EditorErrorCode::InvalidCommandType,
        "EditorCommandEnvelope.command",
        "command type is not registered");

    const auto compatibilityError = ValidateEditorCommandEnvelope(command);
    if (compatibilityError.has_value())
    {
        ThrowCodecError(
            *compatibilityError,
            "EditorCommandEnvelope",
            std::string("typed command failed command contract validation: ") +
                std::string(GetEditorCommandJsonErrorName(*compatibilityError)));
    }
}

void ValidateResultForCodec(
    const EditorCommandResult& result)
{
    Require(
        result.protocolVersion == kEditorCommandProtocolVersion,
        EditorErrorCode::InvalidProtocolVersion,
        "EditorCommandResult.protocolVersion",
        "unsupported protocol version");
    Require(
        result.commandId != 0,
        EditorErrorCode::InvalidCommandId,
        "EditorCommandResult.commandId",
        "commandId must be non-zero");
    Require(
        !GetEditorCommandJsonStatusName(result.status).empty(),
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.status",
        "status has no stable lower-case wire name");
    Require(
        !GetEditorCommandJsonErrorName(result.errorCode).empty(),
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.errorCode",
        "error code has no stable lower-case wire name");
    if (result.status == EditorCommandStatus::Succeeded)
    {
        Require(
            result.errorCode == EditorErrorCode::None,
            EditorErrorCode::InvalidPayload,
            "EditorCommandResult",
            "succeeded result must use error code none");
    }

    if (const auto* document =
            std::get_if<EditorDocumentResultPayload>(&result.payload))
    {
        if (result.documentRevision.has_value())
        {
            Require(
                *result.documentRevision == document->documentRevision,
                EditorErrorCode::InvalidPayload,
                "EditorCommandResult",
                "top-level and payload document revisions differ");
        }
    }
    if (const auto* batch =
            std::get_if<EditorBatchResultPayload>(&result.payload))
    {
        if (result.documentRevision.has_value())
        {
            Require(
                *result.documentRevision == batch->documentRevision,
                EditorErrorCode::InvalidPayload,
                "EditorCommandResult",
                "top-level and payload document revisions differ");
        }
    }
}

Json EncodeResultPayload(
    const EditorCommandResultPayload& payload)
{
    if (std::holds_alternative<EditorNoPayload>(payload))
    {
        return Json::object();
    }
    if (const auto* document = std::get_if<EditorDocumentResultPayload>(&payload))
    {
        return Json{
            {"documentRevision", document->documentRevision},
            {"dirty", document->dirty}};
    }
    if (const auto* batch = std::get_if<EditorBatchResultPayload>(&payload))
    {
        return Json{
            {"commandCount", batch->commandCount},
            {"documentRevision", batch->documentRevision}};
    }
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.payload",
        "unsupported result payload type");
}

EditorCommandResultPayload DecodeResultPayload(
    const Json& wire)
{
    RequireObject(wire, "EditorCommandResult.payload");
    if (wire.empty())
    {
        return EditorNoPayload{};
    }

    if (ContainsField(wire, "dirty"))
    {
        RequireAllowedFields(
            wire,
            {"documentRevision", "dirty"},
            {},
            "EditorCommandResult.payload");
        return EditorDocumentResultPayload{
            ReadUnsigned<uint64_t>(
                wire,
                "documentRevision",
                "EditorCommandResult.payload"),
            ReadBoolean(wire, "dirty", "EditorCommandResult.payload")};
    }

    if (ContainsField(wire, "commandCount"))
    {
        RequireAllowedFields(
            wire,
            {"commandCount", "documentRevision"},
            {},
            "EditorCommandResult.payload");
        return EditorBatchResultPayload{
            ReadUnsigned<uint32_t>(
                wire,
                "commandCount",
                "EditorCommandResult.payload"),
            ReadUnsigned<uint64_t>(
                wire,
                "documentRevision",
                "EditorCommandResult.payload")};
    }

    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.payload",
        "contains unknown or incomplete result fields");
}

} // namespace

std::string_view GetEditorCommandJsonSourceName(
    EditorCommandSource source) noexcept
{
    switch (source)
    {
    case EditorCommandSource::ImGui: return "imgui";
    case EditorCommandSource::Console: return "console";
    case EditorCommandSource::AI: return "ai";
    case EditorCommandSource::RuntimeTest: return "runtime_test";
    }
    return {};
}

std::string_view GetEditorCommandJsonStatusName(
    EditorCommandStatus status) noexcept
{
    switch (status)
    {
    case EditorCommandStatus::Accepted: return "accepted";
    case EditorCommandStatus::Running: return "running";
    case EditorCommandStatus::Succeeded: return "succeeded";
    case EditorCommandStatus::Rejected: return "rejected";
    case EditorCommandStatus::Failed: return "failed";
    }
    return {};
}

std::string_view GetEditorCommandJsonErrorName(
    EditorErrorCode errorCode) noexcept
{
    switch (errorCode)
    {
    case EditorErrorCode::None: return "none";
    case EditorErrorCode::InvalidProtocolVersion: return "invalid_protocol_version";
    case EditorErrorCode::InvalidCommandId: return "invalid_command_id";
    case EditorErrorCode::InvalidCommandType: return "invalid_command_type";
    case EditorErrorCode::InvalidPayload: return "invalid_payload";
    case EditorErrorCode::MissingExpectedDocumentRevision: return "missing_expected_document_revision";
    case EditorErrorCode::StaleDocumentRevision: return "stale_document_revision";
    case EditorErrorCode::DuplicateCommandId: return "duplicate_command_id";
    case EditorErrorCode::ResultStoreCapacityExceeded: return "result_store_capacity_exceeded";
    case EditorErrorCode::AssetNotFound: return "asset_not_found";
    case EditorErrorCode::InvalidAssetType: return "invalid_asset_type";
    case EditorErrorCode::ReferenceResolutionFailed: return "reference_resolution_failed";
    case EditorErrorCode::DocumentNotOpen: return "document_not_open";
    case EditorErrorCode::DocumentDirty: return "document_dirty";
    case EditorErrorCode::UnknownParameter: return "unknown_parameter";
    case EditorErrorCode::ParameterTypeMismatch: return "parameter_type_mismatch";
    case EditorErrorCode::UnknownTextureSlot: return "unknown_texture_slot";
    case EditorErrorCode::InvalidTextureAssetReference: return "invalid_texture_asset_reference";
    case EditorErrorCode::SourceChanged: return "source_changed";
    case EditorErrorCode::ValidationFailed: return "validation_failed";
    case EditorErrorCode::AtomicWriteFailed: return "atomic_write_failed";
    case EditorErrorCode::PreviewUnavailable: return "preview_unavailable";
    case EditorErrorCode::PreviewGenerationChanged: return "preview_generation_changed";
    case EditorErrorCode::PreviewPrepareFailed: return "preview_prepare_failed";
    case EditorErrorCode::PreviewCommitFailed: return "preview_commit_failed";
    }
    return {};
}

EditorCommandJsonCodec::Json EditorCommandJsonCodec::Encode(
    const EditorCommandEnvelope& command)
{
    ValidateEnvelopeForCodec(command);

    Json wire = {
        {"protocolVersion", command.protocolVersion},
        {"commandId", command.commandId},
        {"source", std::string(RequireWireName(
                        GetEditorCommandJsonSourceName(command.source),
                        "EditorCommandEnvelope.source"))},
        {"command", std::string(RequireWireName(
                         GetEditorCommandName(command.type),
                         "EditorCommandEnvelope.command"))},
        {"payload", EncodePayload(command.type, command.payload)}};
    if (command.correlationId.has_value())
    {
        Require(
            *command.correlationId != 0,
            EditorErrorCode::InvalidPayload,
            "EditorCommandEnvelope.correlationId",
            "correlationId must be non-zero when present");
        wire["correlationId"] = *command.correlationId;
    }
    if (command.expectedDocumentRevision.has_value())
    {
        wire["expectedDocumentRevision"] = *command.expectedDocumentRevision;
    }
    return wire;
}

EditorCommandEnvelope EditorCommandJsonCodec::DecodeCommand(
    const Json& wire)
{
    RequireAllowedFields(
        wire,
        {"protocolVersion", "commandId", "source", "command", "payload"},
        {"correlationId", "expectedDocumentRevision"},
        "EditorCommandEnvelope");

    const uint32_t protocolVersion = ReadUnsigned<uint32_t>(
        wire,
        "protocolVersion",
        "EditorCommandEnvelope");
    Require(
        protocolVersion == kEditorCommandProtocolVersion,
        EditorErrorCode::InvalidProtocolVersion,
        "EditorCommandEnvelope.protocolVersion",
        "unsupported protocol version");

    EditorCommandEnvelope command;
    command.protocolVersion = protocolVersion;
    command.commandId = ReadUnsigned<uint64_t>(
        wire,
        "commandId",
        "EditorCommandEnvelope");
    Require(
        command.commandId != 0,
        EditorErrorCode::InvalidCommandId,
        "EditorCommandEnvelope.commandId",
        "commandId must be non-zero");
    command.source = DecodeSource(ReadString(
        wire,
        "source",
        "EditorCommandEnvelope"));
    command.type = DecodeCommandType(ReadString(
        wire,
        "command",
        "EditorCommandEnvelope"));

    if (ContainsField(wire, "correlationId"))
    {
        const EditorCorrelationId correlationId = ReadUnsigned<uint64_t>(
            wire,
            "correlationId",
            "EditorCommandEnvelope");
        Require(
            correlationId != 0,
            EditorErrorCode::InvalidPayload,
            "EditorCommandEnvelope.correlationId",
            "correlationId must be non-zero when present");
        command.correlationId = correlationId;
    }
    if (ContainsField(wire, "expectedDocumentRevision"))
    {
        command.expectedDocumentRevision = ReadUnsigned<uint64_t>(
            wire,
            "expectedDocumentRevision",
            "EditorCommandEnvelope");
    }

    const auto payloadIterator = wire.find("payload");
    Require(
        payloadIterator != wire.end(),
        EditorErrorCode::InvalidPayload,
        "EditorCommandEnvelope",
        "missing payload");
    RequireObject(*payloadIterator, "EditorCommandEnvelope.payload");
    command.payload = DecodePayload(command.type, *payloadIterator);
    ValidateEnvelopeForCodec(command);
    return command;
}

EditorCommandJsonCodec::Json EditorCommandJsonCodec::EncodeResult(
    const EditorCommandResult& result)
{
    ValidateResultForCodec(result);
    Json wire = {
        {"protocolVersion", result.protocolVersion},
        {"commandId", result.commandId},
        {"status", std::string(RequireWireName(
                        GetEditorCommandJsonStatusName(result.status),
                        "EditorCommandResult.status"))},
        {"errorCode", std::string(RequireWireName(
                           GetEditorCommandJsonErrorName(result.errorCode),
                           "EditorCommandResult.errorCode"))},
        {"message", result.message},
        {"payload", EncodeResultPayload(result.payload)}};
    if (result.documentRevision.has_value())
    {
        wire["documentRevision"] = *result.documentRevision;
    }
    return wire;
}

EditorCommandResult EditorCommandJsonCodec::DecodeResult(
    const Json& wire)
{
    RequireAllowedFields(
        wire,
        {"protocolVersion", "commandId", "status", "errorCode", "message", "payload"},
        {"documentRevision"},
        "EditorCommandResult");

    EditorCommandResult result;
    result.protocolVersion = ReadUnsigned<uint32_t>(
        wire,
        "protocolVersion",
        "EditorCommandResult");
    Require(
        result.protocolVersion == kEditorCommandProtocolVersion,
        EditorErrorCode::InvalidProtocolVersion,
        "EditorCommandResult.protocolVersion",
        "unsupported protocol version");
    result.commandId = ReadUnsigned<uint64_t>(
        wire,
        "commandId",
        "EditorCommandResult");
    Require(
        result.commandId != 0,
        EditorErrorCode::InvalidCommandId,
        "EditorCommandResult.commandId",
        "commandId must be non-zero");
    result.status = DecodeStatus(ReadString(
        wire,
        "status",
        "EditorCommandResult"));
    result.errorCode = DecodeErrorCode(ReadString(
        wire,
        "errorCode",
        "EditorCommandResult"));
    result.message = ReadString(
        wire,
        "message",
        "EditorCommandResult",
        false);
    result.payload = DecodeResultPayload(wire.at("payload"));
    if (ContainsField(wire, "documentRevision"))
    {
        result.documentRevision = ReadUnsigned<uint64_t>(
            wire,
            "documentRevision",
            "EditorCommandResult");
    }

    if (const auto* document = std::get_if<EditorDocumentResultPayload>(&result.payload))
    {
        if (result.documentRevision.has_value())
        {
            Require(
                *result.documentRevision == document->documentRevision,
                EditorErrorCode::InvalidPayload,
                "EditorCommandResult",
                "top-level and payload document revisions differ");
        }
    }
    if (const auto* batch = std::get_if<EditorBatchResultPayload>(&result.payload))
    {
        if (result.documentRevision.has_value())
        {
            Require(
                *result.documentRevision == batch->documentRevision,
                EditorErrorCode::InvalidPayload,
                "EditorCommandResult",
                "top-level and payload document revisions differ");
        }
    }

    ValidateResultForCodec(result);
    return result;
}

std::string EditorCommandJsonCodec::EncodeText(
    const EditorCommandEnvelope& command)
{
    return Encode(command).dump();
}

EditorCommandEnvelope EditorCommandJsonCodec::DecodeCommandText(
    std::string_view text)
{
    try
    {
        return DecodeCommand(Json::parse(text.begin(), text.end()));
    }
    catch (const EditorCommandJsonCodecError&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        ThrowCodecError(
            EditorErrorCode::InvalidPayload,
            "EditorCommandEnvelope",
            std::string("invalid JSON: ") + exception.what());
    }
}

std::string EditorCommandJsonCodec::EncodeResultText(
    const EditorCommandResult& result)
{
    return EncodeResult(result).dump();
}

EditorCommandResult EditorCommandJsonCodec::DecodeResultText(
    std::string_view text)
{
    try
    {
        return DecodeResult(Json::parse(text.begin(), text.end()));
    }
    catch (const EditorCommandJsonCodecError&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        ThrowCodecError(
            EditorErrorCode::InvalidPayload,
            "EditorCommandResult",
            std::string("invalid JSON: ") + exception.what());
    }
}

EditorCommandJson EncodeEditorCommandEnvelope(
    const EditorCommandEnvelope& command)
{
    return EditorCommandJsonCodec::Encode(command);
}

EditorCommandEnvelope DecodeEditorCommandEnvelope(
    const EditorCommandJson& wire)
{
    return EditorCommandJsonCodec::DecodeCommand(wire);
}

EditorCommandJson EncodeEditorCommandResult(
    const EditorCommandResult& result)
{
    return EditorCommandJsonCodec::EncodeResult(result);
}

EditorCommandResult DecodeEditorCommandResult(
    const EditorCommandJson& wire)
{
    return EditorCommandJsonCodec::DecodeResult(wire);
}

} // namespace VL
