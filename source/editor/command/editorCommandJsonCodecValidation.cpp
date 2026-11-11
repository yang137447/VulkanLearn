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
    if (const auto* asset = std::get_if<EditorAssetDocumentResultPayload>(&payload))
    {
        Json result{{"assetPath", asset->assetPath}, {"documentRevision", asset->documentRevision},
            {"dirty", asset->dirty}, {"sourceDigest", asset->sourceDigest}, {"document", asset->document},
            {"canUndo", asset->canUndo}, {"canRedo", asset->canRedo},
            {"capabilities", asset->capabilities}, {"changedFields", asset->changedFields},
            {"diagnostics", asset->diagnostics},
            {"application", asset->application}, {"validationLevel", asset->validationLevel}};
        if (!asset->heightmapPreviewPath.empty())
            result["heightmapPreviewPath"] = asset->heightmapPreviewPath;
        return result;
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

    if (ContainsField(wire, "assetPath"))
    {
        RequireAllowedFields(wire, {"assetPath", "documentRevision", "dirty", "sourceDigest",
            "document", "capabilities", "changedFields", "application", "validationLevel"},
            {"diagnostics", "canUndo", "canRedo", "heightmapPreviewPath"},
            "EditorAssetDocumentResultPayload");
        EditorAssetDocumentResultPayload result;
        result.assetPath = ReadAssetPath(wire, "assetPath", "asset result");
        result.documentRevision = ReadUnsigned<uint64_t>(wire, "documentRevision", "asset result");
        result.dirty = ReadBoolean(wire, "dirty", "asset result");
        if (wire.contains("canUndo")) result.canUndo = ReadBoolean(wire, "canUndo", "asset result");
        if (wire.contains("canRedo")) result.canRedo = ReadBoolean(wire, "canRedo", "asset result");
        result.sourceDigest = ReadString(wire, "sourceDigest", "asset result");
        RequireObject(wire.at("document"), "asset result.document");
        RequireObject(wire.at("capabilities"), "asset result.capabilities");
        Require(wire.at("changedFields").is_array(), EditorErrorCode::InvalidPayload, "asset result", "changedFields must be array");
        result.document = wire.at("document");
        result.capabilities = wire.at("capabilities");
        result.changedFields = wire.at("changedFields");
        if (wire.contains("diagnostics"))
        {
            Require(wire.at("diagnostics").is_array(), EditorErrorCode::InvalidPayload, "asset result", "diagnostics must be array");
            result.diagnostics = wire.at("diagnostics");
        }
        result.application = ReadString(wire, "application", "asset result");
        result.validationLevel = ReadString(wire, "validationLevel", "asset result");
        if (wire.contains("heightmapPreviewPath"))
            result.heightmapPreviewPath = ReadString(wire, "heightmapPreviewPath", "asset result");
        return result;
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

} // namespace EditorCommandJsonDetail
} // namespace VL
