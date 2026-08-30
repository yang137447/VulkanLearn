#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace VL
{

inline constexpr uint32_t kEditorCommandProtocolVersion = 1;
using EditorCommandId = uint64_t;
using EditorCorrelationId = uint64_t;
using EditorDocumentRevision = uint64_t;

enum class EditorCommandSource
{
    ImGui,
    Console,
    AI,
    RuntimeTest
};

enum class EditorCommandType
{
    ResolveSceneMaterialAsset,
    ListMaterialInstanceAssets,
    OpenMaterialInstanceAsset,
    OpenTextureAsset,
    SelectMaterialInstanceDocument,
    CloseMaterialInstanceAsset,
    GetMaterialInstanceDocument,
    GetMaterialInstanceReferenceContext,
    SetMaterialParameterOverride,
    ClearMaterialParameterOverride,
    SetMaterialTextureOverride,
    ClearMaterialTextureOverride,
    SetMaterialRenderStateOverride,
    ClearMaterialRenderStateOverride,
    ResetMaterialInstanceOverrides,
    RevertMaterialInstanceDocument,
    ReloadMaterialInstanceDocument,
    ValidateMaterialInstanceDocument,
    SaveMaterialInstanceDocument,
    ConnectMaterialInstancePreview,
    DisconnectMaterialInstancePreview,
    ApplyMaterialInstancePreview,
    RestoreMaterialInstancePreviewBaseline,
    GetEditorCommandResult,
    ListEditorEvents,
    ExecuteEditorCommandBatch
};

enum class EditorCommandStatus
{
    Accepted,
    Running,
    Succeeded,
    Rejected,
    Failed
};

enum class EditorErrorCode
{
    None,
    InvalidProtocolVersion,
    InvalidCommandId,
    InvalidCommandType,
    InvalidPayload,
    MissingExpectedDocumentRevision,
    StaleDocumentRevision,
    DuplicateCommandId,
    ResultStoreCapacityExceeded,
    AssetNotFound,
    InvalidAssetType,
    ReferenceResolutionFailed,
    DocumentNotOpen,
    DocumentDirty,
    UnknownParameter,
    ParameterTypeMismatch,
    UnknownTextureSlot,
    InvalidTextureAssetReference,
    SourceChanged,
    ValidationFailed,
    AtomicWriteFailed,
    PreviewUnavailable,
    PreviewGenerationChanged,
    PreviewPrepareFailed,
    PreviewCommitFailed
};

enum class EditorDirtyDocumentPolicy
{
    RequireClean,
    DiscardChanges
};

enum class EditorResetScope
{
    Parameters,
    Textures,
    RenderStates,
    All
};

enum class EditorMaterialRenderStateField
{
    RenderMode,
    CullMode,
    ShadingModel
};

enum class EditorMaterialRenderMode
{
    Opaque,
    OpaqueClip,
    ForwardOpaque,
    ForwardEyeInner,
    ForwardEyeCornea,
    TransparentAlphaBlend,
    TransparentAlphaBlendWriteDepth,
    TransparentAdditive,
    ThinTranslucent
};

enum class EditorMaterialCullMode
{
    Back,
    Front,
    None
};

enum class EditorMaterialShadingModel
{
    DefaultLit,
    Unlit,
    Subsurface,
    PreintegratedSkin,
    ClearCoat,
    SubsurfaceProfile,
    TwoSidedFoliage,
    Hair,
    Cloth,
    Eye,
    ThinTranslucent
};

using EditorMaterialRenderStateValue = std::variant<
    EditorMaterialRenderMode,
    EditorMaterialCullMode,
    EditorMaterialShadingModel>;

enum class EditorMaterialParameterType
{
    Float,
    Vec2,
    Vec3,
    Vec4
};

using EditorVec2 = std::array<float, 2>;
using EditorVec3 = std::array<float, 3>;
using EditorVec4 = std::array<float, 4>;
using EditorMaterialParameterValue =
    std::variant<float, EditorVec2, EditorVec3, EditorVec4>;

struct EditorNoPayload
{
};

struct EditorNavigationOrigin
{
    std::string sceneIdentity;
    std::string objectIdentity;
    std::optional<uint32_t> section;
    std::string slot;
};

struct ResolveSceneMaterialAssetPayload
{
    std::string sceneIdentity;
    std::string objectIdentity;
    std::optional<uint32_t> section;
};

struct ListMaterialInstanceAssetsPayload
{
    std::string searchText;
    uint32_t pageIndex = 0;
    uint32_t pageSize = 50;
};

struct OpenMaterialInstanceAssetPayload
{
    std::string assetPath;
    std::optional<EditorNavigationOrigin> origin;
};

struct OpenTextureAssetPayload
{
    std::string assetPath;
    std::string textureAssetPath;
};

struct MaterialInstanceAssetPathPayload
{
    std::string assetPath;
};

struct CloseMaterialInstanceAssetPayload
{
    std::string assetPath;
    EditorDirtyDocumentPolicy dirtyPolicy =
        EditorDirtyDocumentPolicy::RequireClean;
};

struct SetMaterialParameterOverridePayload
{
    std::string assetPath;
    std::string parameter;
    EditorMaterialParameterType parameterType =
        EditorMaterialParameterType::Float;
    EditorMaterialParameterValue value = 0.0f;
};

struct ClearMaterialParameterOverridePayload
{
    std::string assetPath;
    std::string parameter;
};

struct SetMaterialTextureOverridePayload
{
    std::string assetPath;
    std::string slot;
    std::string textureAssetPath;
};

struct ClearMaterialTextureOverridePayload
{
    std::string assetPath;
    std::string slot;
};

struct SetMaterialRenderStateOverridePayload
{
    std::string assetPath;
    EditorMaterialRenderStateField field =
        EditorMaterialRenderStateField::RenderMode;
    EditorMaterialRenderStateValue value =
        EditorMaterialRenderMode::Opaque;
};

struct ClearMaterialRenderStateOverridePayload
{
    std::string assetPath;
    EditorMaterialRenderStateField field =
        EditorMaterialRenderStateField::RenderMode;
};

struct ResetMaterialInstanceOverridesPayload
{
    std::string assetPath;
    EditorResetScope scope = EditorResetScope::All;
};

struct ReloadMaterialInstanceDocumentPayload
{
    std::string assetPath;
    EditorDirtyDocumentPolicy dirtyPolicy =
        EditorDirtyDocumentPolicy::RequireClean;
};

struct ApplyMaterialInstancePreviewPayload
{
    std::string assetPath;
    EditorDocumentRevision documentRevision = 0;
};

struct GetEditorCommandResultPayload
{
    EditorCommandId commandId = 0;
};

struct ListEditorEventsPayload
{
    uint64_t afterEventId = 0;
    uint32_t limit = 100;
};

using EditorCommandBatchItemPayload = std::variant<
    SetMaterialParameterOverridePayload,
    ClearMaterialParameterOverridePayload,
    SetMaterialTextureOverridePayload,
    ClearMaterialTextureOverridePayload,
    SetMaterialRenderStateOverridePayload,
    ClearMaterialRenderStateOverridePayload,
    ResetMaterialInstanceOverridesPayload,
    MaterialInstanceAssetPathPayload,
    ReloadMaterialInstanceDocumentPayload,
    ApplyMaterialInstancePreviewPayload>;

struct EditorCommandBatchItem
{
    EditorCommandType type = EditorCommandType::SetMaterialParameterOverride;
    EditorCommandBatchItemPayload payload =
        SetMaterialParameterOverridePayload{};
};

struct ExecuteEditorCommandBatchPayload
{
    std::vector<EditorCommandBatchItem> commands;
};

using EditorCommandPayload = std::variant<
    EditorNoPayload,
    ResolveSceneMaterialAssetPayload,
    ListMaterialInstanceAssetsPayload,
    OpenMaterialInstanceAssetPayload,
    OpenTextureAssetPayload,
    MaterialInstanceAssetPathPayload,
    CloseMaterialInstanceAssetPayload,
    SetMaterialParameterOverridePayload,
    ClearMaterialParameterOverridePayload,
    SetMaterialTextureOverridePayload,
    ClearMaterialTextureOverridePayload,
    SetMaterialRenderStateOverridePayload,
    ClearMaterialRenderStateOverridePayload,
    ResetMaterialInstanceOverridesPayload,
    ReloadMaterialInstanceDocumentPayload,
    ApplyMaterialInstancePreviewPayload,
    GetEditorCommandResultPayload,
    ListEditorEventsPayload,
    ExecuteEditorCommandBatchPayload>;

// 可跨 ImGui、Console、AI 和测试传递的稳定命令包；不携带指针、callback 或 Vulkan 对象。
struct EditorCommandEnvelope
{
    uint32_t protocolVersion = kEditorCommandProtocolVersion;
    EditorCommandId commandId = 0;
    std::optional<EditorCorrelationId> correlationId;
    EditorCommandSource source = EditorCommandSource::ImGui;
    EditorCommandType type = EditorCommandType::GetMaterialInstanceDocument;
    std::optional<EditorDocumentRevision> expectedDocumentRevision;
    EditorCommandPayload payload = EditorNoPayload{};
};

struct EditorDocumentResultPayload
{
    EditorDocumentRevision documentRevision = 0;
    bool dirty = false;
};

struct EditorBatchResultPayload
{
    uint32_t commandCount = 0;
    EditorDocumentRevision documentRevision = 0;
};

using EditorCommandResultPayload = std::variant<
    EditorNoPayload,
    EditorDocumentResultPayload,
    EditorBatchResultPayload>;

// 结果是所有 producer 共同消费的状态，不表达 executor 的内部对象。
struct EditorCommandResult
{
    uint32_t protocolVersion = kEditorCommandProtocolVersion;
    EditorCommandId commandId = 0;
    EditorCommandStatus status = EditorCommandStatus::Accepted;
    EditorErrorCode errorCode = EditorErrorCode::None;
    std::string message;
    std::optional<EditorDocumentRevision> documentRevision;
    EditorCommandResultPayload payload = EditorNoPayload{};
};

struct EditorCommandSpec
{
    EditorCommandType type;
    const char* name;
    uint32_t schemaVersion;
    bool mutatesDocument;
    bool requiresExpectedRevision;
    bool asynchronous;
};

const EditorCommandSpec* FindEditorCommandSpec(EditorCommandType type) noexcept;
const EditorCommandSpec* FindEditorCommandSpec(std::string_view name) noexcept;
std::string_view GetEditorCommandName(EditorCommandType type) noexcept;
std::string_view GetEditorErrorCodeName(EditorErrorCode code) noexcept;
std::string_view GetEditorCommandStatusName(EditorCommandStatus status) noexcept;
std::string_view GetEditorCommandSourceName(EditorCommandSource source) noexcept;
std::string_view GetEditorMaterialRenderStateFieldName(
    EditorMaterialRenderStateField field) noexcept;
std::string_view GetEditorMaterialRenderModeName(
    EditorMaterialRenderMode mode) noexcept;
std::string_view GetEditorMaterialCullModeName(
    EditorMaterialCullMode mode) noexcept;
std::string_view GetEditorMaterialShadingModelName(
    EditorMaterialShadingModel model) noexcept;
EditorMaterialRenderStateField ParseEditorMaterialRenderStateField(
    std::string_view name);
EditorMaterialRenderMode ParseEditorMaterialRenderMode(std::string_view name);
EditorMaterialCullMode ParseEditorMaterialCullMode(std::string_view name);
EditorMaterialShadingModel ParseEditorMaterialShadingModel(std::string_view name);
bool IsEditorMaterialRenderStateValueCompatible(
    EditorMaterialRenderStateField field,
    const EditorMaterialRenderStateValue& value) noexcept;
bool IsFinalEditorCommandStatus(EditorCommandStatus status) noexcept;
EditorMaterialParameterType GetEditorMaterialParameterType(
    const EditorMaterialParameterValue& value) noexcept;
bool IsFiniteEditorMaterialParameterValue(
    const EditorMaterialParameterValue& value) noexcept;
std::optional<std::string> GetEditorCommandAssetPath(
    const EditorCommandEnvelope& command);
bool IsEditorCommandPayloadCompatible(
    const EditorCommandEnvelope& command) noexcept;
std::optional<EditorErrorCode> ValidateEditorCommandEnvelope(
    const EditorCommandEnvelope& command);
std::optional<EditorErrorCode> ValidateEditorCommandBatch(
    const ExecuteEditorCommandBatchPayload& batch);

} // namespace VL
