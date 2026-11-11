#include "editor/command/editorCommandJsonCodecInternal.h"
#include "editor/command/assetDocumentFields.h"
#include "editor/command/strictJson.h"

#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <utility>


namespace VL
{
namespace EditorCommandJsonDetail
{

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
    if (const auto operation = GetEditorAssetOperation(type))
    {
        EditorCommandEnvelope command;
        command.payload = payload;
        const auto assetPath = GetEditorCommandAssetPath(command);
        Require(assetPath.has_value(), EditorErrorCode::InvalidPayload, "asset command", "missing path");
        Json result{{"assetPath", NormalizeRelativeAssetPath(*assetPath, "asset command.assetPath")}};
        if (*operation == EditorAssetOperation::Modify)
        {
            try
            {
                result["changes"] = Editor::EncodeAssetChanges(
                    std::get<ModifyAssetDocumentPayload>(payload).changes, *GetEditorAssetDocumentKind(type));
            }
            catch (const std::exception& error)
            {
                ThrowCodecError(EditorErrorCode::InvalidPayload, "asset.modify", error.what());
            }
        }
        if (*operation == EditorAssetOperation::Reload)
            result["dirtyPolicy"] = std::get<ReloadMaterialInstanceDocumentPayload>(payload).dirtyPolicy ==
                EditorDirtyDocumentPolicy::DiscardChanges ? "discard_changes" : "require_clean";
        return result;
    }
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
    case EditorCommandType::SaveSceneDocument:
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
    if (const auto operation = GetEditorAssetOperation(type))
    {
        if (*operation == EditorAssetOperation::Modify)
        {
            RequireAllowedFields(wire, {"assetPath", "changes"}, {}, "asset.modify");
            try
            {
                return ModifyAssetDocumentPayload{ReadAssetPath(wire, "assetPath", "asset.modify"),
                    Editor::DecodeAssetChanges(wire.at("changes"), *GetEditorAssetDocumentKind(type))};
            }
            catch (const EditorCommandJsonCodecError&) { throw; }
            catch (const std::exception& error)
            {
                ThrowCodecError(EditorErrorCode::InvalidPayload, "asset.modify", error.what());
            }
        }
        if (*operation == EditorAssetOperation::Reload)
        {
            RequireAllowedFields(wire, {"assetPath"}, {"dirtyPolicy"}, "asset.reload");
            return ReloadMaterialInstanceDocumentPayload{
                ReadAssetPath(wire, "assetPath", "asset.reload"),
                ContainsField(wire, "dirtyPolicy") ? DecodeDirtyPolicy(ReadString(wire, "dirtyPolicy", "asset.reload"))
                    : EditorDirtyDocumentPolicy::RequireClean};
        }
        RequireAllowedFields(wire, {"assetPath"}, {}, "asset command");
        return MaterialInstanceAssetPathPayload{ReadAssetPath(wire, "assetPath", "asset command")};
    }
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
    case EditorCommandType::SaveSceneDocument:
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

} // namespace EditorCommandJsonDetail
} // namespace VL
