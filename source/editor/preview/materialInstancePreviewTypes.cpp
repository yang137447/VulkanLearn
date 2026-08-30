#include "editor/preview/materialInstancePreviewTypes.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <utility>

namespace VL::Editor::Preview
{
namespace
{

std::string NormalizeSeparators(std::string_view path)
{
    std::string normalized(path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

bool StartsWithParentComponent(const std::string& path)
{
    return path == ".." || path.rfind("../", 0) == 0 ||
           path.rfind("/../", 0) != std::string::npos ||
           (path.size() >= 3 &&
            path.compare(path.size() - 3, 3, "/..") == 0);
}

bool HasValidMaterialInstanceFileName(const std::string& path)
{
    const std::filesystem::path filePath(path);
    const std::string fileName = filePath.filename().generic_string();
    const std::string extension = filePath.extension().generic_string();
    return fileName.size() > 7 && fileName.rfind("MI_", 0) == 0 &&
           extension == ".json";
}

const char* UnknownString() noexcept
{
    return "Unknown";
}

} // namespace

bool MaterialInstancePreviewWorldIdentity::IsValid() const noexcept
{
    return generation != 0 && !scenePath.empty();
}

bool operator==(
    const MaterialInstancePreviewWorldIdentity& left,
    const MaterialInstancePreviewWorldIdentity& right) noexcept
{
    return left.generation == right.generation &&
           left.scenePath == right.scenePath;
}

bool operator!=(
    const MaterialInstancePreviewWorldIdentity& left,
    const MaterialInstancePreviewWorldIdentity& right) noexcept
{
    return !(left == right);
}

bool NormalizedMaterialInstancePath::IsValid() const noexcept
{
    return !value.empty();
}

bool operator==(
    const NormalizedMaterialInstancePath& left,
    const NormalizedMaterialInstancePath& right) noexcept
{
    return left.value == right.value;
}

bool operator!=(
    const NormalizedMaterialInstancePath& left,
    const NormalizedMaterialInstancePath& right) noexcept
{
    return !(left == right);
}

MaterialInstancePreviewPathNormalizationResult
NormalizeMaterialInstancePath(std::string_view path)
{
    MaterialInstancePreviewPathNormalizationResult result;
    if (path.empty())
    {
        result.errorMessage = "Material instance path is empty.";
        return result;
    }

    if (path.find('\0') != std::string_view::npos)
    {
        result.errorMessage = "Material instance path contains a NUL byte.";
        return result;
    }

    try
    {
        const std::filesystem::path inputPath(
            NormalizeSeparators(path));
        if (inputPath.has_root_name() || inputPath.has_root_directory() ||
            inputPath.is_absolute())
        {
            result.errorMessage =
                "Material instance path must be resource-relative.";
            return result;
        }

        const std::string normalized =
            inputPath.lexically_normal().generic_string();
        if (normalized.empty() || normalized == "." ||
            StartsWithParentComponent(normalized))
        {
            result.errorMessage =
                "Material instance path escapes the resource root.";
            return result;
        }

        if (!HasValidMaterialInstanceFileName(normalized))
        {
            result.errorMessage =
                "Material instance path must name an MI_*.json asset.";
            return result;
        }

        NormalizedMaterialInstancePath normalizedPath;
        normalizedPath.value = normalized;
        result.path = std::move(normalizedPath);
        return result;
    }
    catch (const std::exception& exception)
    {
        result.errorMessage =
            std::string("Material instance path normalization failed: ") +
            exception.what();
        return result;
    }
}

MaterialInstancePreviewWorldIdentity NormalizeWorldIdentity(
    MaterialInstancePreviewWorldIdentity identity)
{
    std::replace(identity.scenePath.begin(), identity.scenePath.end(), '\\', '/');
    try
    {
        identity.scenePath =
            std::filesystem::path(identity.scenePath)
                .lexically_normal()
                .generic_string();
    }
    catch (const std::exception&)
    {
        // 仅做逻辑规范化；如果平台路径解析失败，保留原始值供诊断。
    }
    return identity;
}

bool operator==(
    const MaterialInstancePreviewDraft& left,
    const MaterialInstancePreviewDraft& right) noexcept
{
    return left.materialInstancePath == right.materialInstancePath &&
           left.documentRevision == right.documentRevision &&
           left.serializedWorkingDraft == right.serializedWorkingDraft;
}

bool operator!=(
    const MaterialInstancePreviewDraft& left,
    const MaterialInstancePreviewDraft& right) noexcept
{
    return !(left == right);
}

bool operator==(
    const ConnectMaterialInstancePreviewPayload& left,
    const ConnectMaterialInstancePreviewPayload& right) noexcept
{
    return left.world == right.world &&
           left.materialInstancePath == right.materialInstancePath &&
           left.documentRevision == right.documentRevision;
}

bool operator==(
    const DisconnectMaterialInstancePreviewPayload&,
    const DisconnectMaterialInstancePreviewPayload&) noexcept
{
    return true;
}

bool operator==(
    const ApplyMaterialInstancePreviewPayload& left,
    const ApplyMaterialInstancePreviewPayload& right) noexcept
{
    return left.workingDraft == right.workingDraft;
}

bool operator==(
    const RestoreMaterialInstancePreviewBaselinePayload& left,
    const RestoreMaterialInstancePreviewBaselinePayload& right) noexcept
{
    return left.materialInstancePath == right.materialInstancePath &&
           left.documentRevision == right.documentRevision &&
           left.baselineDraft == right.baselineDraft;
}

bool operator==(
    const MaterialInstancePreviewCommand& left,
    const MaterialInstancePreviewCommand& right) noexcept
{
    return left.protocolVersion == right.protocolVersion &&
           left.commandId == right.commandId &&
           left.correlationId == right.correlationId &&
           left.source == right.source && left.type == right.type &&
           left.expectedDocumentRevision == right.expectedDocumentRevision &&
           left.expectedPreviewGeneration == right.expectedPreviewGeneration &&
           left.payload == right.payload;
}

bool operator!=(
    const MaterialInstancePreviewCommand& left,
    const MaterialInstancePreviewCommand& right) noexcept
{
    return !(left == right);
}

const char* ToString(PreviewCommandSource source) noexcept
{
    switch (source)
    {
    case PreviewCommandSource::ImGui:
        return "imgui";
    case PreviewCommandSource::Console:
        return "console";
    case PreviewCommandSource::AI:
        return "ai";
    case PreviewCommandSource::RuntimeTest:
        return "runtime_test";
    case PreviewCommandSource::Engine:
        return "engine";
    case PreviewCommandSource::Unknown:
        return "unknown";
    }
    return UnknownString();
}

const char* ToString(MaterialInstancePreviewCommandType type) noexcept
{
    switch (type)
    {
    case MaterialInstancePreviewCommandType::
        ConnectMaterialInstancePreview:
        return "material.preview.connect";
    case MaterialInstancePreviewCommandType::
        DisconnectMaterialInstancePreview:
        return "material.preview.disconnect";
    case MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview:
        return "material.preview.apply";
    case MaterialInstancePreviewCommandType::
        RestoreMaterialInstancePreviewBaseline:
        return "material.preview.restore_baseline";
    }
    return UnknownString();
}

const char* ToString(MaterialInstancePreviewCommandStatus status) noexcept
{
    switch (status)
    {
    case MaterialInstancePreviewCommandStatus::Accepted:
        return "Accepted";
    case MaterialInstancePreviewCommandStatus::Running:
        return "Running";
    case MaterialInstancePreviewCommandStatus::Succeeded:
        return "Succeeded";
    case MaterialInstancePreviewCommandStatus::Rejected:
        return "Rejected";
    case MaterialInstancePreviewCommandStatus::Failed:
        return "Failed";
    }
    return UnknownString();
}

const char* ToString(MaterialInstancePreviewErrorCode errorCode) noexcept
{
    switch (errorCode)
    {
    case MaterialInstancePreviewErrorCode::None:
        return "None";
    case MaterialInstancePreviewErrorCode::InvalidProtocolVersion:
        return "InvalidProtocolVersion";
    case MaterialInstancePreviewErrorCode::InvalidCommand:
        return "InvalidCommand";
    case MaterialInstancePreviewErrorCode::InvalidMaterialInstancePath:
        return "InvalidMaterialInstancePath";
    case MaterialInstancePreviewErrorCode::PreviewUnavailable:
        return "PreviewUnavailable";
    case MaterialInstancePreviewErrorCode::PreviewGenerationChanged:
        return "PreviewGenerationChanged";
    case MaterialInstancePreviewErrorCode::StaleDocumentRevision:
        return "StaleDocumentRevision";
    case MaterialInstancePreviewErrorCode::PreviewPrepareFailed:
        return "PreviewPrepareFailed";
    case MaterialInstancePreviewErrorCode::PreviewCommitFailed:
        return "PreviewCommitFailed";
    case MaterialInstancePreviewErrorCode::PreviewNotConnected:
        return "PreviewNotConnected";
    case MaterialInstancePreviewErrorCode::PreviewOperationInProgress:
        return "PreviewOperationInProgress";
    case MaterialInstancePreviewErrorCode::PreviewOperationNotFound:
        return "PreviewOperationNotFound";
    case MaterialInstancePreviewErrorCode::PreviewOperationCanceled:
        return "PreviewOperationCanceled";
    case MaterialInstancePreviewErrorCode::PreviewAdapterUnavailable:
        return "PreviewAdapterUnavailable";
    case MaterialInstancePreviewErrorCode::PreviewAdapterDisconnected:
        return "PreviewAdapterDisconnected";
    case MaterialInstancePreviewErrorCode::CommandIdConflict:
        return "CommandIdConflict";
    case MaterialInstancePreviewErrorCode::ResultStoreFull:
        return "ResultStoreFull";
    case MaterialInstancePreviewErrorCode::ValidationFailed:
        return "ValidationFailed";
    case MaterialInstancePreviewErrorCode::InternalError:
        return "InternalError";
    }
    return UnknownString();
}

const char* ToString(MaterialInstancePreviewState state) noexcept
{
    switch (state)
    {
    case MaterialInstancePreviewState::Disconnected:
        return "Disconnected";
    case MaterialInstancePreviewState::Connecting:
        return "Connecting";
    case MaterialInstancePreviewState::Connected:
        return "Connected";
    case MaterialInstancePreviewState::Applying:
        return "Applying";
    case MaterialInstancePreviewState::RestoringBaseline:
        return "RestoringBaseline";
    case MaterialInstancePreviewState::Unavailable:
        return "Unavailable";
    case MaterialInstancePreviewState::Failed:
        return "Failed";
    }
    return UnknownString();
}

const char* ToString(PreviewAdapterOperationStatus status) noexcept
{
    switch (status)
    {
    case PreviewAdapterOperationStatus::Completed:
        return "Completed";
    case PreviewAdapterOperationStatus::Pending:
        return "Pending";
    case PreviewAdapterOperationStatus::Unavailable:
        return "Unavailable";
    case PreviewAdapterOperationStatus::Failed:
        return "Failed";
    }
    return UnknownString();
}

const char* ToString(PreviewAdapterFailureStage stage) noexcept
{
    switch (stage)
    {
    case PreviewAdapterFailureStage::None:
        return "None";
    case PreviewAdapterFailureStage::Prepare:
        return "Prepare";
    case PreviewAdapterFailureStage::Commit:
        return "Commit";
    }
    return UnknownString();
}

} // namespace VL::Editor::Preview
