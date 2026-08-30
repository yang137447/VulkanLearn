#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace VL::Editor::Preview
{

inline constexpr uint32_t kMaterialInstancePreviewProtocolVersion = 1;

using PreviewCommandId = uint64_t;
using PreviewOperationId = uint64_t;

enum class PreviewCommandSource
{
    Unknown,
    ImGui,
    Console,
    AI,
    RuntimeTest,
    Engine
};

enum class MaterialInstancePreviewCommandType
{
    ConnectMaterialInstancePreview,
    DisconnectMaterialInstancePreview,
    ApplyMaterialInstancePreview,
    RestoreMaterialInstancePreviewBaseline
};

enum class MaterialInstancePreviewCommandStatus
{
    Accepted,
    Running,
    Succeeded,
    Rejected,
    Failed
};

enum class MaterialInstancePreviewErrorCode
{
    None,
    InvalidProtocolVersion,
    InvalidCommand,
    InvalidMaterialInstancePath,
    PreviewUnavailable,
    PreviewGenerationChanged,
    StaleDocumentRevision,
    PreviewPrepareFailed,
    PreviewCommitFailed,
    PreviewNotConnected,
    PreviewOperationInProgress,
    PreviewOperationNotFound,
    PreviewOperationCanceled,
    PreviewAdapterUnavailable,
    PreviewAdapterDisconnected,
    CommandIdConflict,
    ResultStoreFull,
    ValidationFailed,
    InternalError
};

enum class MaterialInstancePreviewState
{
    Disconnected,
    Connecting,
    Connected,
    Applying,
    RestoringBaseline,
    Unavailable,
    Failed
};

enum class PreviewAdapterOperationStatus
{
    Completed,
    Pending,
    Unavailable,
    Failed
};

enum class PreviewAdapterFailureStage
{
    None,
    Prepare,
    Commit
};

// 只保存可复制的 World 元数据；不复用 WorldHandle，避免把 weak_ptr 带入
// 编辑器命令和异步结果。generation 是 World 换代身份，scenePath 是诊断和
// 物理场景身份的一部分。
struct MaterialInstancePreviewWorldIdentity
{
    uint64_t generation = 0;
    std::string scenePath;

    bool IsValid() const noexcept;
};

bool operator==(
    const MaterialInstancePreviewWorldIdentity& left,
    const MaterialInstancePreviewWorldIdentity& right) noexcept;
bool operator!=(
    const MaterialInstancePreviewWorldIdentity& left,
    const MaterialInstancePreviewWorldIdentity& right) noexcept;

// 这是 resourcePath 之下的逻辑 MI 身份，不是 filesystem::canonical 结果。
// canonical 化需要访问磁盘，且会把 Asset Document 身份错误地绑定到当前机器。
struct NormalizedMaterialInstancePath
{
    std::string value;

    bool IsValid() const noexcept;
};

bool operator==(
    const NormalizedMaterialInstancePath& left,
    const NormalizedMaterialInstancePath& right) noexcept;
bool operator!=(
    const NormalizedMaterialInstancePath& left,
    const NormalizedMaterialInstancePath& right) noexcept;

struct MaterialInstancePreviewPathNormalizationResult
{
    std::optional<NormalizedMaterialInstancePath> path;
    std::string errorMessage;

    bool Succeeded() const noexcept { return path.has_value(); }
};

MaterialInstancePreviewPathNormalizationResult
NormalizeMaterialInstancePath(std::string_view path);

MaterialInstancePreviewWorldIdentity NormalizeWorldIdentity(
    MaterialInstancePreviewWorldIdentity identity);

// working draft 必须是完整、值语义、可序列化的候选内容。Bridge 不解析或持有
// MaterialInstance*，renderer adapter 也只能在自己的 owner 线程重新解析这些字节。
struct MaterialInstancePreviewDraft
{
    std::string materialInstancePath;
    uint64_t documentRevision = 0;
    std::string serializedWorkingDraft;
};

bool operator==(
    const MaterialInstancePreviewDraft& left,
    const MaterialInstancePreviewDraft& right) noexcept;
bool operator!=(
    const MaterialInstancePreviewDraft& left,
    const MaterialInstancePreviewDraft& right) noexcept;

struct ConnectMaterialInstancePreviewPayload
{
    MaterialInstancePreviewWorldIdentity world;
    std::string materialInstancePath;
    std::optional<uint64_t> documentRevision;
};

struct DisconnectMaterialInstancePreviewPayload
{
};

struct ApplyMaterialInstancePreviewPayload
{
    MaterialInstancePreviewDraft workingDraft;
};

struct RestoreMaterialInstancePreviewBaselinePayload
{
    std::string materialInstancePath;
    uint64_t documentRevision = 0;
    MaterialInstancePreviewDraft baselineDraft;
};

bool operator==(
    const ConnectMaterialInstancePreviewPayload& left,
    const ConnectMaterialInstancePreviewPayload& right) noexcept;
bool operator==(
    const DisconnectMaterialInstancePreviewPayload& left,
    const DisconnectMaterialInstancePreviewPayload& right) noexcept;
bool operator==(
    const ApplyMaterialInstancePreviewPayload& left,
    const ApplyMaterialInstancePreviewPayload& right) noexcept;
bool operator==(
    const RestoreMaterialInstancePreviewBaselinePayload& left,
    const RestoreMaterialInstancePreviewBaselinePayload& right) noexcept;

using MaterialInstancePreviewCommandPayload = std::variant<
    ConnectMaterialInstancePreviewPayload,
    DisconnectMaterialInstancePreviewPayload,
    ApplyMaterialInstancePreviewPayload,
    RestoreMaterialInstancePreviewBaselinePayload>;

// 这个 envelope 与未来统一 EditorCommandEnvelope 保持同一组公共语义，但
// 暂时放在 preview 命名空间，避免 P0.0 尚未落地时产生跨目录耦合。
struct MaterialInstancePreviewCommand
{
    uint32_t protocolVersion =
        kMaterialInstancePreviewProtocolVersion;
    PreviewCommandId commandId = 0;
    std::optional<PreviewCommandId> correlationId;
    PreviewCommandSource source = PreviewCommandSource::Unknown;
    MaterialInstancePreviewCommandType type =
        MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview;
    std::optional<uint64_t> expectedDocumentRevision;
    std::optional<uint64_t> expectedPreviewGeneration;
    MaterialInstancePreviewCommandPayload payload =
        ConnectMaterialInstancePreviewPayload{};
};

bool operator==(
    const MaterialInstancePreviewCommand& left,
    const MaterialInstancePreviewCommand& right) noexcept;
bool operator!=(
    const MaterialInstancePreviewCommand& left,
    const MaterialInstancePreviewCommand& right) noexcept;

struct MaterialInstancePreviewStatus
{
    MaterialInstancePreviewState state =
        MaterialInstancePreviewState::Disconnected;
    std::optional<MaterialInstancePreviewWorldIdentity> world;
    std::optional<NormalizedMaterialInstancePath> materialInstancePath;
    // Bridge generation 是逻辑连接代际；每次断开、重连或 live replacement
    // 都会变化，用来拒绝迟到的异步结果。它不是 Vulkan handle 或指针。
    uint64_t liveGeneration = 0;
    uint64_t runtimeResourceGeneration = 0;
    std::optional<uint64_t> observedDocumentRevision;
    std::optional<uint64_t> appliedDocumentRevision;
    MaterialInstancePreviewErrorCode lastError =
        MaterialInstancePreviewErrorCode::None;
    std::string diagnosticMessage;
    uint64_t statusRevision = 0;

    bool IsConnected() const noexcept
    {
        return state == MaterialInstancePreviewState::Connected ||
               state == MaterialInstancePreviewState::Applying ||
               state == MaterialInstancePreviewState::RestoringBaseline;
    }
};

struct MaterialInstancePreviewResultPayload
{
    MaterialInstancePreviewStatus previewStatus;
    std::optional<uint64_t> appliedDocumentRevision;
    std::optional<PreviewOperationId> operationId;
    bool liveSwapCommitted = false;
};

struct MaterialInstancePreviewCommandResult
{
    uint32_t protocolVersion =
        kMaterialInstancePreviewProtocolVersion;
    PreviewCommandId commandId = 0;
    std::optional<PreviewCommandId> correlationId;
    MaterialInstancePreviewCommandStatus status =
        MaterialInstancePreviewCommandStatus::Rejected;
    MaterialInstancePreviewErrorCode errorCode =
        MaterialInstancePreviewErrorCode::None;
    std::string message;
    std::optional<uint64_t> documentRevision;
    MaterialInstancePreviewResultPayload payload;

    bool IsTerminal() const noexcept
    {
        return status == MaterialInstancePreviewCommandStatus::Succeeded ||
               status == MaterialInstancePreviewCommandStatus::Rejected ||
               status == MaterialInstancePreviewCommandStatus::Failed;
    }
};

// Adapter 命令只携带逻辑身份、代际和序列化 draft。具体 Vulkan 资源由未来
// renderer adapter 在 owner 侧重新解析，Bridge 永远不保存 handle 或资源指针。
struct MaterialInstancePreviewAdapterCommand
{
    MaterialInstancePreviewCommandType type =
        MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview;
    MaterialInstancePreviewWorldIdentity world;
    NormalizedMaterialInstancePath materialInstancePath;
    uint64_t bridgeLiveGeneration = 0;
    std::optional<MaterialInstancePreviewDraft> draft;
    std::optional<uint64_t> documentRevision;
};

struct MaterialInstancePreviewAdapterResult
{
    PreviewAdapterOperationStatus status =
        PreviewAdapterOperationStatus::Failed;
    PreviewOperationId operationId = 0;
    // 这是 renderer 侧可选的逻辑资源代际，用于诊断和后续 adapter 校验。
    // Bridge 不把它当作 Vulkan 对象，也不依赖它完成内存管理。
    uint64_t runtimeResourceGeneration = 0;
    bool liveSwapCommitted = false;
    bool replacesLiveResource = false;
    PreviewAdapterFailureStage failureStage =
        PreviewAdapterFailureStage::None;
    std::string message;
};

const char* ToString(PreviewCommandSource source) noexcept;
const char* ToString(MaterialInstancePreviewCommandType type) noexcept;
const char* ToString(MaterialInstancePreviewCommandStatus status) noexcept;
const char* ToString(MaterialInstancePreviewErrorCode errorCode) noexcept;
const char* ToString(MaterialInstancePreviewState state) noexcept;
const char* ToString(PreviewAdapterOperationStatus status) noexcept;
const char* ToString(PreviewAdapterFailureStage stage) noexcept;

} // namespace VL::Editor::Preview
