#include "editor/command/editorCommand.h"

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <type_traits>

namespace VL
{
namespace
{
constexpr EditorCommandSpec kSpecs[] = {
    {EditorCommandType::ResolveSceneMaterialAsset, "material.resolve_scene_reference", 1, false, false, false},
    {EditorCommandType::ListMaterialInstanceAssets, "material.list_assets", 1, false, false, false},
    {EditorCommandType::OpenMaterialInstanceAsset, "material.open", 1, false, false, false},
    {EditorCommandType::OpenTextureAsset, "texture.open", 1, false, false, false},
    {EditorCommandType::SelectMaterialInstanceDocument, "material.select", 1, false, false, false},
    {EditorCommandType::CloseMaterialInstanceAsset, "material.close", 1, false, false, false},
    {EditorCommandType::GetMaterialInstanceDocument, "material.get_document", 1, false, false, false},
    {EditorCommandType::GetMaterialInstanceReferenceContext, "material.get_reference_context", 1, false, false, false},
    {EditorCommandType::SetMaterialParameterOverride, "material.set_parameter", 1, true, true, false},
    {EditorCommandType::ClearMaterialParameterOverride, "material.clear_parameter", 1, true, true, false},
    {EditorCommandType::SetMaterialTextureOverride, "material.set_texture", 1, true, true, false},
    {EditorCommandType::ClearMaterialTextureOverride, "material.clear_texture", 1, true, true, false},
    {EditorCommandType::SetMaterialRenderStateOverride, "material.set_render_state", 1, true, true, false},
    {EditorCommandType::ClearMaterialRenderStateOverride, "material.clear_render_state", 1, true, true, false},
    {EditorCommandType::ResetMaterialInstanceOverrides, "material.reset_overrides", 1, true, true, false},
    {EditorCommandType::RevertMaterialInstanceDocument, "material.revert", 1, true, true, false},
    {EditorCommandType::ReloadMaterialInstanceDocument, "material.reload", 1, true, true, true},
    {EditorCommandType::ValidateMaterialInstanceDocument, "material.validate", 1, false, false, true},
    {EditorCommandType::SaveMaterialInstanceDocument, "material.save", 1, true, true, true},
    {EditorCommandType::ConnectMaterialInstancePreview, "material.preview.connect", 1, false, false, true},
    {EditorCommandType::DisconnectMaterialInstancePreview, "material.preview.disconnect", 1, false, false, false},
    {EditorCommandType::ApplyMaterialInstancePreview, "material.preview.apply", 1, false, false, true},
    {EditorCommandType::RestoreMaterialInstancePreviewBaseline, "material.preview.restore_baseline", 1, false, false, true},
    {EditorCommandType::GetEditorCommandResult, "editor.get_command_result", 1, false, false, false},
    {EditorCommandType::ListEditorEvents, "editor.list_events", 1, false, false, false},
    {EditorCommandType::ExecuteEditorCommandBatch, "editor.execute_batch", 1, true, true, false}};

template <typename T>
bool FiniteArray(const T& values)
{
    if constexpr (std::is_same_v<T, float>)
    {
        return std::isfinite(values);
    }
    else
    {
        for (float value : values)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
        }
        return true;
    }
}

bool ValidBatchPayload(const EditorCommandBatchItem& item)
{
    switch (item.type)
    {
    case EditorCommandType::SetMaterialParameterOverride: return std::holds_alternative<SetMaterialParameterOverridePayload>(item.payload);
    case EditorCommandType::ClearMaterialParameterOverride: return std::holds_alternative<ClearMaterialParameterOverridePayload>(item.payload);
    case EditorCommandType::SetMaterialTextureOverride: return std::holds_alternative<SetMaterialTextureOverridePayload>(item.payload);
    case EditorCommandType::ClearMaterialTextureOverride: return std::holds_alternative<ClearMaterialTextureOverridePayload>(item.payload);
    case EditorCommandType::SetMaterialRenderStateOverride: return std::holds_alternative<SetMaterialRenderStateOverridePayload>(item.payload);
    case EditorCommandType::ClearMaterialRenderStateOverride: return std::holds_alternative<ClearMaterialRenderStateOverridePayload>(item.payload);
    case EditorCommandType::ResetMaterialInstanceOverrides: return std::holds_alternative<ResetMaterialInstanceOverridesPayload>(item.payload);
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline: return std::holds_alternative<MaterialInstanceAssetPathPayload>(item.payload);
    case EditorCommandType::ReloadMaterialInstanceDocument: return std::holds_alternative<ReloadMaterialInstanceDocumentPayload>(item.payload);
    case EditorCommandType::ApplyMaterialInstancePreview: return std::holds_alternative<ApplyMaterialInstancePreviewPayload>(item.payload);
    default: return false;
    }
}

std::string BatchPath(const EditorCommandBatchItem& item)
{
    if (const auto* value = std::get_if<SetMaterialParameterOverridePayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ClearMaterialParameterOverridePayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<SetMaterialTextureOverridePayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ClearMaterialTextureOverridePayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<SetMaterialRenderStateOverridePayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ClearMaterialRenderStateOverridePayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ResetMaterialInstanceOverridesPayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<MaterialInstanceAssetPathPayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ReloadMaterialInstanceDocumentPayload>(&item.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ApplyMaterialInstancePreviewPayload>(&item.payload)) return value->assetPath;
    return {};
}

std::string NormalizeBatchPathForComparison(std::string path)
{
    for (char& character : path)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }
    return std::filesystem::path(path).lexically_normal().generic_string();
}

bool IsValidEditorResetScope(EditorResetScope scope) noexcept
{
    switch (scope)
    {
    case EditorResetScope::Parameters:
    case EditorResetScope::Textures:
    case EditorResetScope::RenderStates:
    case EditorResetScope::All:
        return true;
    }
    return false;
}
}

const EditorCommandSpec* FindEditorCommandSpec(EditorCommandType type) noexcept
{
    for (const auto& spec : kSpecs)
        if (spec.type == type) return &spec;
    return nullptr;
}

const EditorCommandSpec* FindEditorCommandSpec(std::string_view name) noexcept
{
    for (const auto& spec : kSpecs)
        if (name == spec.name) return &spec;
    return nullptr;
}

std::string_view GetEditorCommandName(EditorCommandType type) noexcept
{
    const auto* spec = FindEditorCommandSpec(type);
    return spec == nullptr ? std::string_view{} : spec->name;
}

std::string_view GetEditorErrorCodeName(EditorErrorCode code) noexcept
{
    switch (code)
    {
    case EditorErrorCode::None: return "None";
    case EditorErrorCode::InvalidProtocolVersion: return "InvalidProtocolVersion";
    case EditorErrorCode::InvalidCommandId: return "InvalidCommandId";
    case EditorErrorCode::InvalidCommandType: return "InvalidCommandType";
    case EditorErrorCode::InvalidPayload: return "InvalidPayload";
    case EditorErrorCode::MissingExpectedDocumentRevision: return "MissingExpectedDocumentRevision";
    case EditorErrorCode::StaleDocumentRevision: return "StaleDocumentRevision";
    case EditorErrorCode::DuplicateCommandId: return "DuplicateCommandId";
    case EditorErrorCode::ResultStoreCapacityExceeded: return "ResultStoreCapacityExceeded";
    case EditorErrorCode::AssetNotFound: return "AssetNotFound";
    case EditorErrorCode::InvalidAssetType: return "InvalidAssetType";
    case EditorErrorCode::ReferenceResolutionFailed: return "ReferenceResolutionFailed";
    case EditorErrorCode::DocumentNotOpen: return "DocumentNotOpen";
    case EditorErrorCode::DocumentDirty: return "DocumentDirty";
    case EditorErrorCode::UnknownParameter: return "UnknownParameter";
    case EditorErrorCode::ParameterTypeMismatch: return "ParameterTypeMismatch";
    case EditorErrorCode::UnknownTextureSlot: return "UnknownTextureSlot";
    case EditorErrorCode::InvalidTextureAssetReference: return "InvalidTextureAssetReference";
    case EditorErrorCode::SourceChanged: return "SourceChanged";
    case EditorErrorCode::ValidationFailed: return "ValidationFailed";
    case EditorErrorCode::AtomicWriteFailed: return "AtomicWriteFailed";
    case EditorErrorCode::PreviewUnavailable: return "PreviewUnavailable";
    case EditorErrorCode::PreviewGenerationChanged: return "PreviewGenerationChanged";
    case EditorErrorCode::PreviewPrepareFailed: return "PreviewPrepareFailed";
    case EditorErrorCode::PreviewCommitFailed: return "PreviewCommitFailed";
    }
    return "Unknown";
}

std::string_view GetEditorCommandStatusName(EditorCommandStatus status) noexcept
{
    switch (status)
    {
    case EditorCommandStatus::Accepted: return "Accepted";
    case EditorCommandStatus::Running: return "Running";
    case EditorCommandStatus::Succeeded: return "Succeeded";
    case EditorCommandStatus::Rejected: return "Rejected";
    case EditorCommandStatus::Failed: return "Failed";
    }
    return "Unknown";
}

std::string_view GetEditorCommandSourceName(EditorCommandSource source) noexcept
{
    switch (source)
    {
    case EditorCommandSource::ImGui: return "imgui";
    case EditorCommandSource::Console: return "console";
    case EditorCommandSource::AI: return "ai";
    case EditorCommandSource::RuntimeTest: return "runtime_test";
    }
    return "unknown";
}

std::string_view GetEditorMaterialRenderStateFieldName(
    EditorMaterialRenderStateField field) noexcept
{
    switch (field)
    {
    case EditorMaterialRenderStateField::RenderMode: return "renderMode";
    case EditorMaterialRenderStateField::CullMode: return "cullMode";
    case EditorMaterialRenderStateField::ShadingModel: return "shadingModel";
    }
    return {};
}

std::string_view GetEditorMaterialRenderModeName(
    EditorMaterialRenderMode mode) noexcept
{
    switch (mode)
    {
    case EditorMaterialRenderMode::Opaque: return "Opaque";
    case EditorMaterialRenderMode::OpaqueClip: return "OpaqueClip";
    case EditorMaterialRenderMode::ForwardOpaque: return "ForwardOpaque";
    case EditorMaterialRenderMode::ForwardEyeInner: return "ForwardEyeInner";
    case EditorMaterialRenderMode::ForwardEyeCornea: return "ForwardEyeCornea";
    case EditorMaterialRenderMode::TransparentAlphaBlend: return "TransparentAlphaBlend";
    case EditorMaterialRenderMode::TransparentAlphaBlendWriteDepth: return "TransparentAlphaBlendWriteDepth";
    case EditorMaterialRenderMode::TransparentAdditive: return "TransparentAdditive";
    case EditorMaterialRenderMode::ThinTranslucent: return "ThinTranslucent";
    }
    return {};
}

std::string_view GetEditorMaterialCullModeName(
    EditorMaterialCullMode mode) noexcept
{
    switch (mode)
    {
    case EditorMaterialCullMode::Back: return "Back";
    case EditorMaterialCullMode::Front: return "Front";
    case EditorMaterialCullMode::None: return "None";
    }
    return {};
}

std::string_view GetEditorMaterialShadingModelName(
    EditorMaterialShadingModel model) noexcept
{
    switch (model)
    {
    case EditorMaterialShadingModel::DefaultLit: return "DefaultLit";
    case EditorMaterialShadingModel::Unlit: return "Unlit";
    case EditorMaterialShadingModel::Subsurface: return "Subsurface";
    case EditorMaterialShadingModel::PreintegratedSkin: return "PreintegratedSkin";
    case EditorMaterialShadingModel::ClearCoat: return "ClearCoat";
    case EditorMaterialShadingModel::SubsurfaceProfile: return "SubsurfaceProfile";
    case EditorMaterialShadingModel::TwoSidedFoliage: return "TwoSidedFoliage";
    case EditorMaterialShadingModel::Hair: return "Hair";
    case EditorMaterialShadingModel::Cloth: return "Cloth";
    case EditorMaterialShadingModel::Eye: return "Eye";
    case EditorMaterialShadingModel::ThinTranslucent: return "ThinTranslucent";
    }
    return {};
}

EditorMaterialRenderStateField ParseEditorMaterialRenderStateField(
    std::string_view name)
{
    if (name == "renderMode") return EditorMaterialRenderStateField::RenderMode;
    if (name == "cullMode") return EditorMaterialRenderStateField::CullMode;
    if (name == "shadingModel") return EditorMaterialRenderStateField::ShadingModel;
    throw std::invalid_argument("unknown material render state field: " + std::string(name));
}

EditorMaterialRenderMode ParseEditorMaterialRenderMode(std::string_view name)
{
    for (int index = 0; index <= static_cast<int>(EditorMaterialRenderMode::ThinTranslucent); ++index)
    {
        const auto mode = static_cast<EditorMaterialRenderMode>(index);
        if (GetEditorMaterialRenderModeName(mode) == name) return mode;
    }
    throw std::invalid_argument("unknown material render mode: " + std::string(name));
}

EditorMaterialCullMode ParseEditorMaterialCullMode(std::string_view name)
{
    for (int index = 0; index <= static_cast<int>(EditorMaterialCullMode::None); ++index)
    {
        const auto mode = static_cast<EditorMaterialCullMode>(index);
        if (GetEditorMaterialCullModeName(mode) == name) return mode;
    }
    throw std::invalid_argument("unknown material cull mode: " + std::string(name));
}

EditorMaterialShadingModel ParseEditorMaterialShadingModel(std::string_view name)
{
    for (int index = 0; index <= static_cast<int>(EditorMaterialShadingModel::ThinTranslucent); ++index)
    {
        const auto model = static_cast<EditorMaterialShadingModel>(index);
        if (GetEditorMaterialShadingModelName(model) == name) return model;
    }
    throw std::invalid_argument("unknown material shading model: " + std::string(name));
}

bool IsEditorMaterialRenderStateValueCompatible(
    EditorMaterialRenderStateField field,
    const EditorMaterialRenderStateValue& value) noexcept
{
    switch (field)
    {
    case EditorMaterialRenderStateField::RenderMode:
        return std::holds_alternative<EditorMaterialRenderMode>(value) &&
            !GetEditorMaterialRenderModeName(
                std::get<EditorMaterialRenderMode>(value)).empty();
    case EditorMaterialRenderStateField::CullMode:
        return std::holds_alternative<EditorMaterialCullMode>(value) &&
            !GetEditorMaterialCullModeName(
                std::get<EditorMaterialCullMode>(value)).empty();
    case EditorMaterialRenderStateField::ShadingModel:
        return std::holds_alternative<EditorMaterialShadingModel>(value) &&
            !GetEditorMaterialShadingModelName(
                std::get<EditorMaterialShadingModel>(value)).empty();
    }
    return false;
}

bool IsFinalEditorCommandStatus(EditorCommandStatus status) noexcept
{
    return status == EditorCommandStatus::Succeeded || status == EditorCommandStatus::Rejected || status == EditorCommandStatus::Failed;
}

EditorMaterialParameterType GetEditorMaterialParameterType(const EditorMaterialParameterValue& value) noexcept
{
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, float>) return EditorMaterialParameterType::Float;
        if constexpr (std::is_same_v<T, EditorVec2>) return EditorMaterialParameterType::Vec2;
        if constexpr (std::is_same_v<T, EditorVec3>) return EditorMaterialParameterType::Vec3;
        return EditorMaterialParameterType::Vec4;
    }, value);
}

bool IsFiniteEditorMaterialParameterValue(const EditorMaterialParameterValue& value) noexcept
{
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, float>)
            return std::isfinite(item);
        else
            return FiniteArray(item);
    }, value);
}

std::optional<std::string> GetEditorCommandAssetPath(const EditorCommandEnvelope& command)
{
    if (const auto* value = std::get_if<MaterialInstanceAssetPathPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<OpenMaterialInstanceAssetPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<OpenTextureAssetPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<CloseMaterialInstanceAssetPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<SetMaterialParameterOverridePayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ClearMaterialParameterOverridePayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<SetMaterialTextureOverridePayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ClearMaterialTextureOverridePayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<SetMaterialRenderStateOverridePayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ClearMaterialRenderStateOverridePayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ResetMaterialInstanceOverridesPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ReloadMaterialInstanceDocumentPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ApplyMaterialInstancePreviewPayload>(&command.payload)) return value->assetPath;
    if (const auto* value = std::get_if<ExecuteEditorCommandBatchPayload>(&command.payload))
    {
        if (!value->commands.empty()) return BatchPath(value->commands.front());
    }
    return std::nullopt;
}

bool IsEditorCommandPayloadCompatible(const EditorCommandEnvelope& command) noexcept
{
    switch (command.type)
    {
    case EditorCommandType::ResolveSceneMaterialAsset: return std::holds_alternative<ResolveSceneMaterialAssetPayload>(command.payload);
    case EditorCommandType::ListMaterialInstanceAssets: return std::holds_alternative<ListMaterialInstanceAssetsPayload>(command.payload);
    case EditorCommandType::OpenMaterialInstanceAsset: return std::holds_alternative<OpenMaterialInstanceAssetPayload>(command.payload);
    case EditorCommandType::OpenTextureAsset: return std::holds_alternative<OpenTextureAssetPayload>(command.payload);
    case EditorCommandType::SelectMaterialInstanceDocument:
    case EditorCommandType::GetMaterialInstanceDocument:
    case EditorCommandType::GetMaterialInstanceReferenceContext:
    case EditorCommandType::RevertMaterialInstanceDocument:
    case EditorCommandType::ValidateMaterialInstanceDocument:
    case EditorCommandType::SaveMaterialInstanceDocument:
    case EditorCommandType::ConnectMaterialInstancePreview:
    case EditorCommandType::DisconnectMaterialInstancePreview:
    case EditorCommandType::RestoreMaterialInstancePreviewBaseline: return std::holds_alternative<MaterialInstanceAssetPathPayload>(command.payload);
    case EditorCommandType::CloseMaterialInstanceAsset: return std::holds_alternative<CloseMaterialInstanceAssetPayload>(command.payload);
    case EditorCommandType::SetMaterialParameterOverride: return std::holds_alternative<SetMaterialParameterOverridePayload>(command.payload);
    case EditorCommandType::ClearMaterialParameterOverride: return std::holds_alternative<ClearMaterialParameterOverridePayload>(command.payload);
    case EditorCommandType::SetMaterialTextureOverride: return std::holds_alternative<SetMaterialTextureOverridePayload>(command.payload);
    case EditorCommandType::ClearMaterialTextureOverride: return std::holds_alternative<ClearMaterialTextureOverridePayload>(command.payload);
    case EditorCommandType::SetMaterialRenderStateOverride: return std::holds_alternative<SetMaterialRenderStateOverridePayload>(command.payload);
    case EditorCommandType::ClearMaterialRenderStateOverride: return std::holds_alternative<ClearMaterialRenderStateOverridePayload>(command.payload);
    case EditorCommandType::ResetMaterialInstanceOverrides: return std::holds_alternative<ResetMaterialInstanceOverridesPayload>(command.payload);
    case EditorCommandType::ReloadMaterialInstanceDocument: return std::holds_alternative<ReloadMaterialInstanceDocumentPayload>(command.payload);
    case EditorCommandType::ApplyMaterialInstancePreview: return std::holds_alternative<ApplyMaterialInstancePreviewPayload>(command.payload);
    case EditorCommandType::GetEditorCommandResult: return std::holds_alternative<GetEditorCommandResultPayload>(command.payload);
    case EditorCommandType::ListEditorEvents: return std::holds_alternative<ListEditorEventsPayload>(command.payload);
    case EditorCommandType::ExecuteEditorCommandBatch: return std::holds_alternative<ExecuteEditorCommandBatchPayload>(command.payload);
    }
    return false;
}

std::optional<EditorErrorCode> ValidateEditorCommandBatch(const ExecuteEditorCommandBatchPayload& batch)
{
    if (batch.commands.empty()) return EditorErrorCode::InvalidPayload;
    std::string path;
    bool saved = false;
    for (std::size_t index = 0; index < batch.commands.size(); ++index)
    {
        const auto& item = batch.commands[index];
        if (!ValidBatchPayload(item)) return EditorErrorCode::InvalidPayload;
        const std::string itemPath = NormalizeBatchPathForComparison(BatchPath(item));
        if (itemPath.empty() || (!path.empty() && path != itemPath)) return EditorErrorCode::InvalidPayload;
        path = itemPath;
        if (item.type == EditorCommandType::SaveMaterialInstanceDocument)
        {
            if (saved || index + 1 != batch.commands.size()) return EditorErrorCode::InvalidPayload;
            saved = true;
        }
        if (item.type == EditorCommandType::SetMaterialParameterOverride)
        {
            const auto& value = std::get<SetMaterialParameterOverridePayload>(item.payload);
            if (value.parameter.empty() || GetEditorMaterialParameterType(value.value) != value.parameterType || !IsFiniteEditorMaterialParameterValue(value.value))
                return EditorErrorCode::InvalidPayload;
        }
        if (item.type == EditorCommandType::SetMaterialRenderStateOverride)
        {
            const auto& value =
                std::get<SetMaterialRenderStateOverridePayload>(item.payload);
            if (value.assetPath.empty() ||
                !IsEditorMaterialRenderStateValueCompatible(
                    value.field,
                    value.value))
            {
                return EditorErrorCode::InvalidPayload;
            }
        }
        if (item.type == EditorCommandType::ClearMaterialRenderStateOverride)
        {
            const auto& value =
                std::get<ClearMaterialRenderStateOverridePayload>(item.payload);
            if (value.assetPath.empty() ||
                GetEditorMaterialRenderStateFieldName(value.field).empty())
            {
                return EditorErrorCode::InvalidPayload;
            }
        }
        if (item.type == EditorCommandType::ResetMaterialInstanceOverrides)
        {
            const auto& value =
                std::get<ResetMaterialInstanceOverridesPayload>(item.payload);
            if (value.assetPath.empty() || !IsValidEditorResetScope(value.scope))
            {
                return EditorErrorCode::InvalidPayload;
            }
        }
    }
    return std::nullopt;
}

std::optional<EditorErrorCode> ValidateEditorCommandEnvelope(const EditorCommandEnvelope& command)
{
    if (command.protocolVersion != kEditorCommandProtocolVersion) return EditorErrorCode::InvalidProtocolVersion;
    if (command.commandId == 0) return EditorErrorCode::InvalidCommandId;
    switch (command.source)
    {
    case EditorCommandSource::ImGui:
    case EditorCommandSource::Console:
    case EditorCommandSource::AI:
    case EditorCommandSource::RuntimeTest:
        break;
    default:
        return EditorErrorCode::InvalidPayload;
    }
    const auto* spec = FindEditorCommandSpec(command.type);
    if (spec == nullptr) return EditorErrorCode::InvalidCommandType;
    if (!IsEditorCommandPayloadCompatible(command)) return EditorErrorCode::InvalidPayload;
    if (spec->requiresExpectedRevision && !command.expectedDocumentRevision.has_value()) return EditorErrorCode::MissingExpectedDocumentRevision;
    if (command.type == EditorCommandType::ExecuteEditorCommandBatch)
        return ValidateEditorCommandBatch(std::get<ExecuteEditorCommandBatchPayload>(command.payload));
    if (command.type == EditorCommandType::SetMaterialParameterOverride)
    {
        const auto& value = std::get<SetMaterialParameterOverridePayload>(command.payload);
        if (value.assetPath.empty() || value.parameter.empty() || GetEditorMaterialParameterType(value.value) != value.parameterType || !IsFiniteEditorMaterialParameterValue(value.value))
            return EditorErrorCode::InvalidPayload;
    }
    if (command.type == EditorCommandType::SetMaterialRenderStateOverride)
    {
        const auto& value =
            std::get<SetMaterialRenderStateOverridePayload>(command.payload);
        if (value.assetPath.empty() ||
            !IsEditorMaterialRenderStateValueCompatible(
                value.field,
                value.value))
        {
            return EditorErrorCode::InvalidPayload;
        }
    }
    if (command.type == EditorCommandType::ClearMaterialRenderStateOverride)
    {
        const auto& value =
            std::get<ClearMaterialRenderStateOverridePayload>(command.payload);
        if (value.assetPath.empty() ||
            GetEditorMaterialRenderStateFieldName(value.field).empty())
        {
            return EditorErrorCode::InvalidPayload;
        }
    }
    if (command.type == EditorCommandType::ResetMaterialInstanceOverrides)
    {
        const auto& value =
            std::get<ResetMaterialInstanceOverridesPayload>(command.payload);
        if (value.assetPath.empty() || !IsValidEditorResetScope(value.scope))
        {
            return EditorErrorCode::InvalidPayload;
        }
    }
    return std::nullopt;
}

} // namespace VL
