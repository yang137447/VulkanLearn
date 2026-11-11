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
using namespace EditorCommandJsonDetail;

std::string_view GetEditorCommandJsonSourceName(
    EditorCommandSource source) noexcept
{
    switch (source)
    {
    case EditorCommandSource::RmlUi: return "rmlui";
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
        return DecodeCommand(Editor::ParseStrictJson(text, 1024 * 1024));
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
