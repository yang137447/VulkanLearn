#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "editor/command/editorCommand.h"
#include "editor/command/editorCommandBus.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/persistence/materialInstancePersistence.h"
#include "editor/persistence/materialInstanceSparseCandidate.h"
#include "editor/preview/materialInstancePreviewController.h"
#include "editor/preview/materialInstancePreviewTypes.h"
#include "material/validation/materialAssetValidator.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"
#include "support/fakeMaterialInstancePreviewAdapter.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;
using namespace VL::Editor::Persistence;
using namespace VL::Editor::Preview;

#ifndef VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR
#define VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR "."
#endif

const std::filesystem::path kFixtureRoot =
    std::filesystem::path(VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR);

void Require(
    bool condition,
    std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void RequireThrows(
    const std::function<void()>& callback,
    std::string_view message)
{
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(std::string(message));
}

Json LoadFixture(
    std::string_view relativePath)
{
    const std::filesystem::path path = kFixtureRoot / relativePath;
    std::ifstream input(path);
    Require(input.is_open(), "failed to open fixture " + path.string());
    Json value;
    input >> value;
    return value;
}

std::string ReadTextFile(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "failed to read " + path.string());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<std::pair<EditorCommandType, std::string>> CommandSpecs()
{
    return {
        {EditorCommandType::ResolveSceneMaterialAsset,
         "material.resolve_scene_reference"},
        {EditorCommandType::ListMaterialInstanceAssets, "material.list_assets"},
        {EditorCommandType::OpenMaterialInstanceAsset, "material.open"},
        {EditorCommandType::SelectMaterialInstanceDocument, "material.select"},
        {EditorCommandType::CloseMaterialInstanceAsset, "material.close"},
        {EditorCommandType::GetMaterialInstanceDocument, "material.get_document"},
        {EditorCommandType::GetMaterialInstanceReferenceContext,
         "material.get_reference_context"},
        {EditorCommandType::SetMaterialParameterOverride,
         "material.set_parameter"},
        {EditorCommandType::ClearMaterialParameterOverride,
         "material.clear_parameter"},
        {EditorCommandType::SetMaterialTextureOverride,
         "material.set_texture"},
        {EditorCommandType::ClearMaterialTextureOverride,
         "material.clear_texture"},
        {EditorCommandType::ResetMaterialInstanceOverrides,
         "material.reset_overrides"},
        {EditorCommandType::RevertMaterialInstanceDocument, "material.revert"},
        {EditorCommandType::ReloadMaterialInstanceDocument, "material.reload"},
        {EditorCommandType::ValidateMaterialInstanceDocument,
         "material.validate"},
        {EditorCommandType::SaveMaterialInstanceDocument, "material.save"},
        {EditorCommandType::ConnectMaterialInstancePreview,
         "material.preview.connect"},
        {EditorCommandType::DisconnectMaterialInstancePreview,
         "material.preview.disconnect"},
        {EditorCommandType::ApplyMaterialInstancePreview,
         "material.preview.apply"},
        {EditorCommandType::RestoreMaterialInstancePreviewBaseline,
         "material.preview.restore_baseline"},
        {EditorCommandType::GetEditorCommandResult,
         "editor.get_command_result"},
        {EditorCommandType::ListEditorEvents, "editor.list_events"},
        {EditorCommandType::ExecuteEditorCommandBatch, "editor.execute_batch"}};
}

EditorCommandEnvelope BuildSetParameterCommand(
    EditorCommandSource source,
    EditorCommandId commandId,
    EditorDocumentRevision revision)
{
    EditorCommandEnvelope command;
    command.commandId = commandId;
    command.source = source;
    command.type = EditorCommandType::SetMaterialParameterOverride;
    command.expectedDocumentRevision = revision;
    command.payload = SetMaterialParameterOverridePayload{
        "materials/MI_editor_test.json",
        "u_tint",
        EditorMaterialParameterType::Vec3,
        EditorVec3{0.4f, 0.5f, 0.6f}};
    return command;
}

void RequireCodecRejects(
    const Json& wire,
    EditorErrorCode expectedCode,
    std::string_view message)
{
    try
    {
        EditorCommandJsonCodec::DecodeCommand(wire);
    }
    catch (const EditorCommandJsonCodecError& exception)
    {
        Require(
            exception.Code() == expectedCode,
            std::string(message) + " (unexpected error code)");
        return;
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(std::string(message) + " (unexpected exception type)");
    }
    throw std::runtime_error(std::string(message) + " (input was accepted)");
}

void RequireResultCodecRejects(
    const Json& wire,
    EditorErrorCode expectedCode,
    std::string_view message)
{
    try
    {
        EditorCommandJsonCodec::DecodeResult(wire);
    }
    catch (const EditorCommandJsonCodecError& exception)
    {
        Require(
            exception.Code() == expectedCode,
            std::string(message) + " (unexpected error code)");
        return;
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(std::string(message) + " (unexpected exception type)");
    }
    throw std::runtime_error(std::string(message) + " (input was accepted)");
}

void AssertCommandRoundTrip(
    const EditorCommandEnvelope& command,
    std::string_view context)
{
    const Json wire = EditorCommandJsonCodec::Encode(command);
    const auto decoded = EditorCommandJsonCodec::DecodeCommand(wire);
    Require(
        EditorCommandJsonCodec::Encode(decoded) == wire,
        std::string(context) + " changed during JSON command round-trip");
    Require(
        EditorCommandJsonCodec::DecodeCommandText(
            EditorCommandJsonCodec::EncodeText(command)).type == command.type,
        std::string(context) + " text transport lost command type");
    Require(
        wire.at("command").get<std::string>() == GetEditorCommandName(command.type),
        std::string(context) + " did not emit the stable lower-case command name");
    Require(
        wire.at("source").get<std::string>() ==
            GetEditorCommandJsonSourceName(command.source),
        std::string(context) + " did not emit the stable lower-case source name");
}

EditorCommandEnvelope BuildPathCommand(
    EditorCommandType type,
    EditorCommandId commandId,
    bool expectedRevision = false)
{
    EditorCommandEnvelope command;
    command.commandId = commandId;
    command.source = EditorCommandSource::RuntimeTest;
    command.type = type;
    if (expectedRevision)
    {
        command.expectedDocumentRevision = 7;
    }
    command.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    return command;
}

void TestLandedJsonCommandCodec()
{
    // wire token 使用小写，避免 producer 之间出现大小写分叉。
    Require(
        GetEditorCommandJsonSourceName(EditorCommandSource::ImGui) == "imgui" &&
            GetEditorCommandJsonSourceName(EditorCommandSource::Console) == "console" &&
            GetEditorCommandJsonSourceName(EditorCommandSource::AI) == "ai" &&
            GetEditorCommandJsonSourceName(EditorCommandSource::RuntimeTest) == "runtime_test",
        "JSON source token mapping is not stable");
    Require(
        GetEditorCommandJsonStatusName(EditorCommandStatus::Accepted) == "accepted" &&
            GetEditorCommandJsonStatusName(EditorCommandStatus::Running) == "running" &&
            GetEditorCommandJsonStatusName(EditorCommandStatus::Succeeded) == "succeeded" &&
            GetEditorCommandJsonStatusName(EditorCommandStatus::Rejected) == "rejected" &&
            GetEditorCommandJsonStatusName(EditorCommandStatus::Failed) == "failed",
        "JSON status token mapping is not lower-case");

    for (const EditorErrorCode errorCode : {
             EditorErrorCode::None,
             EditorErrorCode::InvalidProtocolVersion,
             EditorErrorCode::InvalidCommandId,
             EditorErrorCode::InvalidCommandType,
             EditorErrorCode::InvalidPayload,
             EditorErrorCode::MissingExpectedDocumentRevision,
             EditorErrorCode::StaleDocumentRevision,
             EditorErrorCode::DuplicateCommandId,
             EditorErrorCode::ResultStoreCapacityExceeded,
             EditorErrorCode::AssetNotFound,
             EditorErrorCode::InvalidAssetType,
             EditorErrorCode::ReferenceResolutionFailed,
             EditorErrorCode::DocumentNotOpen,
             EditorErrorCode::DocumentDirty,
             EditorErrorCode::UnknownParameter,
             EditorErrorCode::ParameterTypeMismatch,
             EditorErrorCode::UnknownTextureSlot,
             EditorErrorCode::InvalidTextureAssetReference,
             EditorErrorCode::SourceChanged,
             EditorErrorCode::ValidationFailed,
             EditorErrorCode::AtomicWriteFailed,
             EditorErrorCode::PreviewUnavailable,
             EditorErrorCode::PreviewGenerationChanged,
             EditorErrorCode::PreviewPrepareFailed,
             EditorErrorCode::PreviewCommitFailed})
    {
        const std::string name(GetEditorCommandJsonErrorName(errorCode));
        Require(!name.empty(), "JSON error token mapping is incomplete");
        for (const char character : name)
        {
            Require(
                character < 'A' || character > 'Z',
                "JSON error token contains an upper-case character");
        }
    }

    EditorCommandEnvelope resolve;
    resolve.commandId = 100;
    resolve.source = EditorCommandSource::AI;
    resolve.type = EditorCommandType::ResolveSceneMaterialAsset;
    resolve.payload = ResolveSceneMaterialAssetPayload{
        "scenes/scene_material_editor_fixture.json",
        "mesh-car",
        1};
    AssertCommandRoundTrip(resolve, "resolve command");

    EditorCommandEnvelope list;
    list.commandId = 101;
    list.source = EditorCommandSource::Console;
    list.type = EditorCommandType::ListMaterialInstanceAssets;
    list.payload = ListMaterialInstanceAssetsPayload{"editor", 2, 25};
    AssertCommandRoundTrip(list, "list command");

    EditorCommandEnvelope open;
    open.commandId = 102;
    open.source = EditorCommandSource::ImGui;
    open.type = EditorCommandType::OpenMaterialInstanceAsset;
    open.payload = OpenMaterialInstanceAssetPayload{
        "materials\\MI_editor_test.json",
        EditorNavigationOrigin{
            "scenes\\scene_material_editor_fixture.json",
            "mesh-car",
            1,
            "surface"}};
    AssertCommandRoundTrip(open, "open command");

    EditorCommandEnvelope openTexture;
    openTexture.commandId = 103;
    openTexture.source = EditorCommandSource::ImGui;
    openTexture.type = EditorCommandType::OpenTextureAsset;
    openTexture.payload = OpenTextureAssetPayload{
        "materials/MI_editor_test.json",
        "textures\\T_normal_override.json"};
    AssertCommandRoundTrip(openTexture, "open texture command");

    AssertCommandRoundTrip(
        BuildPathCommand(
            EditorCommandType::SelectMaterialInstanceDocument,
            104),
        "select command");
    AssertCommandRoundTrip(
        BuildPathCommand(
            EditorCommandType::GetMaterialInstanceDocument,
            105),
        "get document command");
    AssertCommandRoundTrip(
        BuildPathCommand(
            EditorCommandType::GetMaterialInstanceReferenceContext,
            106),
        "get reference command");

    EditorCommandEnvelope close = BuildPathCommand(
        EditorCommandType::CloseMaterialInstanceAsset,
        107,
        true);
    close.payload = CloseMaterialInstanceAssetPayload{
        "materials/MI_editor_test.json",
        EditorDirtyDocumentPolicy::DiscardChanges};
    AssertCommandRoundTrip(close, "close command");

    for (const auto& parameter : {
             std::pair<EditorMaterialParameterType, EditorMaterialParameterValue>{
                 EditorMaterialParameterType::Float,
                 0.25f},
             std::pair<EditorMaterialParameterType, EditorMaterialParameterValue>{
                 EditorMaterialParameterType::Vec2,
                 EditorVec2{0.1f, 0.2f}},
             std::pair<EditorMaterialParameterType, EditorMaterialParameterValue>{
                 EditorMaterialParameterType::Vec3,
                 EditorVec3{0.3f, 0.4f, 0.5f}},
             std::pair<EditorMaterialParameterType, EditorMaterialParameterValue>{
                 EditorMaterialParameterType::Vec4,
                 EditorVec4{0.6f, 0.7f, 0.8f, 0.9f}}})
    {
        EditorCommandEnvelope setParameter;
        setParameter.commandId = 108 + static_cast<EditorCommandId>(
            parameter.first == EditorMaterialParameterType::Float
                ? 0
                : parameter.first == EditorMaterialParameterType::Vec2
                    ? 1
                    : parameter.first == EditorMaterialParameterType::Vec3 ? 2 : 3);
        setParameter.source = EditorCommandSource::RuntimeTest;
        setParameter.type = EditorCommandType::SetMaterialParameterOverride;
        setParameter.expectedDocumentRevision = 7;
        setParameter.payload = SetMaterialParameterOverridePayload{
            "materials/MI_editor_test.json",
            "u_tint",
            parameter.first,
            parameter.second};
        AssertCommandRoundTrip(setParameter, "set parameter command");
    }

    EditorCommandEnvelope clearParameter;
    clearParameter.commandId = 112;
    clearParameter.source = EditorCommandSource::Console;
    clearParameter.type = EditorCommandType::ClearMaterialParameterOverride;
    clearParameter.expectedDocumentRevision = 7;
    clearParameter.payload = ClearMaterialParameterOverridePayload{
        "materials/MI_editor_test.json",
        "u_tint"};
    AssertCommandRoundTrip(clearParameter, "clear parameter command");

    EditorCommandEnvelope setTexture;
    setTexture.commandId = 113;
    setTexture.source = EditorCommandSource::ImGui;
    setTexture.type = EditorCommandType::SetMaterialTextureOverride;
    setTexture.expectedDocumentRevision = 7;
    setTexture.payload = SetMaterialTextureOverridePayload{
        "materials/MI_editor_test.json",
        "normalMap",
        "textures/T_normal_override.json"};
    AssertCommandRoundTrip(setTexture, "set texture command");

    EditorCommandEnvelope clearTexture;
    clearTexture.commandId = 114;
    clearTexture.source = EditorCommandSource::ImGui;
    clearTexture.type = EditorCommandType::ClearMaterialTextureOverride;
    clearTexture.expectedDocumentRevision = 7;
    clearTexture.payload = ClearMaterialTextureOverridePayload{
        "materials/MI_editor_test.json",
        "normalMap"};
    AssertCommandRoundTrip(clearTexture, "clear texture command");

    EditorCommandId nextCommandId = 115;
    for (const EditorResetScope scope : {
             EditorResetScope::Parameters,
             EditorResetScope::Textures,
             EditorResetScope::All})
    {
        EditorCommandEnvelope reset;
        reset.commandId = nextCommandId++;
        reset.source = EditorCommandSource::AI;
        reset.type = EditorCommandType::ResetMaterialInstanceOverrides;
        reset.expectedDocumentRevision = 7;
        reset.payload = ResetMaterialInstanceOverridesPayload{
            "materials/MI_editor_test.json",
            scope};
        AssertCommandRoundTrip(reset, "reset command");
    }

    AssertCommandRoundTrip(
        BuildPathCommand(EditorCommandType::RevertMaterialInstanceDocument, 118, true),
        "revert command");
    EditorCommandEnvelope reload = BuildPathCommand(
        EditorCommandType::ReloadMaterialInstanceDocument,
        119,
        true);
    reload.payload = ReloadMaterialInstanceDocumentPayload{
        "materials/MI_editor_test.json",
        EditorDirtyDocumentPolicy::DiscardChanges};
    AssertCommandRoundTrip(reload, "reload command");
    AssertCommandRoundTrip(
        BuildPathCommand(EditorCommandType::ValidateMaterialInstanceDocument, 120),
        "validate command");
    AssertCommandRoundTrip(
        BuildPathCommand(EditorCommandType::SaveMaterialInstanceDocument, 121, true),
        "save command");
    AssertCommandRoundTrip(
        BuildPathCommand(EditorCommandType::ConnectMaterialInstancePreview, 122),
        "connect preview command");
    AssertCommandRoundTrip(
        BuildPathCommand(EditorCommandType::DisconnectMaterialInstancePreview, 123),
        "disconnect preview command");

    EditorCommandEnvelope applyPreview;
    applyPreview.commandId = 124;
    applyPreview.source = EditorCommandSource::RuntimeTest;
    applyPreview.type = EditorCommandType::ApplyMaterialInstancePreview;
    applyPreview.payload = VL::ApplyMaterialInstancePreviewPayload{
        "materials/MI_editor_test.json",
        9};
    AssertCommandRoundTrip(applyPreview, "apply preview command");
    AssertCommandRoundTrip(
        BuildPathCommand(
            EditorCommandType::RestoreMaterialInstancePreviewBaseline,
            125),
        "restore preview command");

    EditorCommandEnvelope getResult;
    getResult.commandId = 126;
    getResult.source = EditorCommandSource::AI;
    getResult.type = EditorCommandType::GetEditorCommandResult;
    getResult.payload = GetEditorCommandResultPayload{124};
    AssertCommandRoundTrip(getResult, "get result command");

    EditorCommandEnvelope listEvents;
    listEvents.commandId = 127;
    listEvents.source = EditorCommandSource::Console;
    listEvents.type = EditorCommandType::ListEditorEvents;
    listEvents.payload = ListEditorEventsPayload{88, 20};
    AssertCommandRoundTrip(listEvents, "list events command");

    EditorCommandBatchItem batchSet;
    batchSet.type = EditorCommandType::SetMaterialParameterOverride;
    batchSet.payload = SetMaterialParameterOverridePayload{
        "materials/MI_editor_test.json",
        "u_scalar",
        EditorMaterialParameterType::Float,
        0.75f};
    EditorCommandBatchItem batchClearParameter;
    batchClearParameter.type = EditorCommandType::ClearMaterialParameterOverride;
    batchClearParameter.payload = ClearMaterialParameterOverridePayload{
        "materials/MI_editor_test.json",
        "u_tint"};
    EditorCommandBatchItem batchSetTexture;
    batchSetTexture.type = EditorCommandType::SetMaterialTextureOverride;
    batchSetTexture.payload = SetMaterialTextureOverridePayload{
        "materials/MI_editor_test.json",
        "normalMap",
        "textures/T_normal_override.json"};
    EditorCommandBatchItem batchClearTexture;
    batchClearTexture.type = EditorCommandType::ClearMaterialTextureOverride;
    batchClearTexture.payload = ClearMaterialTextureOverridePayload{
        "materials/MI_editor_test.json",
        "normalMap"};
    EditorCommandBatchItem batchReset;
    batchReset.type = EditorCommandType::ResetMaterialInstanceOverrides;
    batchReset.payload = ResetMaterialInstanceOverridesPayload{
        "materials/MI_editor_test.json",
        EditorResetScope::All};
    EditorCommandBatchItem batchRevert;
    batchRevert.type = EditorCommandType::RevertMaterialInstanceDocument;
    batchRevert.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    EditorCommandBatchItem batchValidate;
    batchValidate.type = EditorCommandType::ValidateMaterialInstanceDocument;
    batchValidate.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    EditorCommandBatchItem batchConnect;
    batchConnect.type = EditorCommandType::ConnectMaterialInstancePreview;
    batchConnect.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    EditorCommandBatchItem batchDisconnect;
    batchDisconnect.type = EditorCommandType::DisconnectMaterialInstancePreview;
    batchDisconnect.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    EditorCommandBatchItem batchRestore;
    batchRestore.type = EditorCommandType::RestoreMaterialInstancePreviewBaseline;
    batchRestore.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    EditorCommandBatchItem batchReload;
    batchReload.type = EditorCommandType::ReloadMaterialInstanceDocument;
    batchReload.payload = ReloadMaterialInstanceDocumentPayload{
        "materials/MI_editor_test.json",
        EditorDirtyDocumentPolicy::RequireClean};
    EditorCommandBatchItem batchApply;
    batchApply.type = EditorCommandType::ApplyMaterialInstancePreview;
    batchApply.payload = VL::ApplyMaterialInstancePreviewPayload{
        "materials/MI_editor_test.json",
        9};
    EditorCommandBatchItem batchSave;
    batchSave.type = EditorCommandType::SaveMaterialInstanceDocument;
    batchSave.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};

    EditorCommandEnvelope batch;
    batch.commandId = 128;
    batch.source = EditorCommandSource::AI;
    batch.type = EditorCommandType::ExecuteEditorCommandBatch;
    batch.expectedDocumentRevision = 7;
    batch.payload = ExecuteEditorCommandBatchPayload{{
        batchSet,
        batchClearParameter,
        batchSetTexture,
        batchClearTexture,
        batchReset,
        batchRevert,
        batchValidate,
        batchConnect,
        batchDisconnect,
        batchRestore,
        batchReload,
        batchApply,
        batchSave}};
    AssertCommandRoundTrip(batch, "batch command");

    const Json validSetWire = EditorCommandJsonCodec::Encode(
        BuildSetParameterCommand(EditorCommandSource::AI, 200, 7));
    Json invalid = validSetWire;
    invalid["unexpected"] = true;
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted an unknown envelope field");
    invalid = validSetWire;
    invalid["protocolVersion"] = 2;
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidProtocolVersion,
        "codec accepted an unsupported command protocol");
    invalid = validSetWire;
    invalid["source"] = "AI";
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted an upper-case source token");
    invalid = validSetWire;
    invalid["command"] = "Material.set_parameter";
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidCommandType,
        "codec accepted an upper-case command token");
    invalid = validSetWire;
    invalid["payload"]["unknown"] = true;
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted an unknown payload field");
    invalid = validSetWire;
    invalid["payload"]["value"] = Json::array({0.1, 0.2});
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted a short vector value");
    invalid = validSetWire;
    invalid["payload"]["value"] = Json::array({0.1, 0.2, 0.3, 0.4});
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted a long vector value");
    invalid = validSetWire;
    invalid["payload"]["type"] = "vec4";
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted a type/arity mismatch");
    invalid = validSetWire;
    invalid["payload"]["value"] = Json::array({
        std::numeric_limits<double>::quiet_NaN(),
        0.2,
        0.3});
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted a non-finite vector component");
    invalid = validSetWire;
    invalid["payload"].erase("parameter");
    RequireCodecRejects(
        invalid,
        EditorErrorCode::InvalidPayload,
        "codec accepted a missing required payload field");

    Json invalidTexture = EditorCommandJsonCodec::Encode(setTexture);
    invalidTexture["payload"]["textureAssetPath"] = "textures/editor.png";
    RequireCodecRejects(
        invalidTexture,
        EditorErrorCode::InvalidTextureAssetReference,
        "codec accepted a raw image texture path");

    Json invalidBatch = EditorCommandJsonCodec::Encode(batch);
    invalidBatch["payload"]["commands"][0]["extra"] = true;
    RequireCodecRejects(
        invalidBatch,
        EditorErrorCode::InvalidPayload,
        "codec accepted an unknown batch-item field");
    invalidBatch = EditorCommandJsonCodec::Encode(batch);
    invalidBatch["payload"]["commands"][0]["command"] = "material.open";
    RequireCodecRejects(
        invalidBatch,
        EditorErrorCode::InvalidPayload,
        "codec accepted a non-batch command");

    EditorCommandResult accepted;
    accepted.commandId = 300;
    accepted.status = EditorCommandStatus::Accepted;
    accepted.errorCode = EditorErrorCode::None;
    accepted.message = "queued";
    const Json acceptedWire = EditorCommandJsonCodec::EncodeResult(accepted);
    Require(
        acceptedWire.at("status") == "accepted" &&
            acceptedWire.at("errorCode") == "none" &&
            EditorCommandJsonCodec::DecodeResult(acceptedWire).payload.index() == 0,
        "accepted result did not use lower-case tokens or empty payload");
    Require(
        EditorCommandJsonCodec::EncodeResult(
            EditorCommandJsonCodec::DecodeResultText(
                EditorCommandJsonCodec::EncodeResultText(accepted))) == acceptedWire,
        "accepted result text round-trip changed the wire");

    EditorCommandResult documentResult;
    documentResult.commandId = 301;
    documentResult.status = EditorCommandStatus::Succeeded;
    documentResult.errorCode = EditorErrorCode::None;
    documentResult.message = "saved";
    documentResult.documentRevision = 8;
    documentResult.payload = EditorDocumentResultPayload{8, true};
    const Json documentResultWire = EditorCommandJsonCodec::EncodeResult(documentResult);
    Require(
        EditorCommandJsonCodec::EncodeResult(
            EditorCommandJsonCodec::DecodeResult(documentResultWire)) ==
            documentResultWire,
        "document result payload did not round-trip");

    EditorCommandResult batchResult;
    batchResult.commandId = 302;
    batchResult.status = EditorCommandStatus::Succeeded;
    batchResult.errorCode = EditorErrorCode::None;
    batchResult.message = "batch saved";
    batchResult.documentRevision = 9;
    batchResult.payload = EditorBatchResultPayload{13, 9};
    const Json batchResultWire = EditorCommandJsonCodec::EncodeResult(batchResult);
    Require(
        EditorCommandJsonCodec::EncodeResult(
            EditorCommandJsonCodec::DecodeResult(batchResultWire)) ==
            batchResultWire,
        "batch result payload did not round-trip");

    EditorCommandResult rejected;
    rejected.commandId = 303;
    rejected.status = EditorCommandStatus::Rejected;
    rejected.errorCode = EditorErrorCode::StaleDocumentRevision;
    rejected.message = "stale";
    const Json rejectedWire = EditorCommandJsonCodec::EncodeResult(rejected);
    Require(
        rejectedWire.at("status") == "rejected" &&
            rejectedWire.at("errorCode") == "stale_document_revision",
        "rejected result did not emit lower-case error token");

    Json invalidResult = acceptedWire;
    invalidResult["status"] = "Succeeded";
    RequireResultCodecRejects(
        invalidResult,
        EditorErrorCode::InvalidPayload,
        "codec accepted an upper-case result status");
    invalidResult = acceptedWire;
    invalidResult["errorCode"] = "None";
    RequireResultCodecRejects(
        invalidResult,
        EditorErrorCode::InvalidPayload,
        "codec accepted an upper-case result error");
    invalidResult = documentResultWire;
    invalidResult["payload"]["unknown"] = true;
    RequireResultCodecRejects(
        invalidResult,
        EditorErrorCode::InvalidPayload,
        "codec accepted an unknown result payload field");
    invalidResult = documentResultWire;
    invalidResult["documentRevision"] = 99;
    RequireResultCodecRejects(
        invalidResult,
        EditorErrorCode::InvalidPayload,
        "codec accepted mismatched result revisions");
    invalidResult = acceptedWire;
    invalidResult["protocolVersion"] = 2;
    RequireResultCodecRejects(
        invalidResult,
        EditorErrorCode::InvalidProtocolVersion,
        "codec accepted an unsupported result protocol");
}

void TestLandedCommandSchema()
{
    for (const auto& [type, name] : CommandSpecs())
    {
        const EditorCommandSpec* spec = FindEditorCommandSpec(type);
        Require(spec != nullptr, "landed command spec is missing");
        Require(spec->schemaVersion == 1, "landed command schema version changed");
        Require(
            GetEditorCommandName(type) == name &&
                FindEditorCommandSpec(name) == spec,
            "landed command name mapping is not stable: " + name);
    }

    Require(
        GetEditorCommandSourceName(EditorCommandSource::RuntimeTest) ==
            "runtime_test",
        "runtime-test source name is not stable");
    Require(
        GetEditorCommandStatusName(EditorCommandStatus::Succeeded) ==
            "Succeeded" &&
            IsFinalEditorCommandStatus(EditorCommandStatus::Failed) &&
            !IsFinalEditorCommandStatus(EditorCommandStatus::Running),
        "landed command status contract is inconsistent");

    const EditorCommandEnvelope valid = BuildSetParameterCommand(
        EditorCommandSource::AI,
        41,
        7);
    Require(
        !ValidateEditorCommandEnvelope(valid).has_value(),
        "valid typed set-parameter command was rejected");
    Require(
        GetEditorCommandAssetPath(valid).value() ==
            "materials/MI_editor_test.json",
        "command asset-path extraction changed the normalized identity");

    for (const EditorCommandSource source : {
             EditorCommandSource::ImGui,
             EditorCommandSource::Console,
             EditorCommandSource::AI,
             EditorCommandSource::RuntimeTest})
    {
        const EditorCommandEnvelope producerCommand =
            BuildSetParameterCommand(source, 42, 7);
        Require(
            !ValidateEditorCommandEnvelope(producerCommand).has_value() &&
                GetEditorCommandName(producerCommand.type) ==
                    GetEditorCommandName(valid.type),
            "producer source changed typed command behavior");
    }

    EditorCommandEnvelope invalid = valid;
    invalid.protocolVersion = 2;
    Require(
        ValidateEditorCommandEnvelope(invalid) ==
            EditorErrorCode::InvalidProtocolVersion,
        "invalid command protocol version was accepted");
    invalid = valid;
    invalid.commandId = 0;
    Require(
        ValidateEditorCommandEnvelope(invalid) == EditorErrorCode::InvalidCommandId,
        "zero command id was accepted");
    invalid = valid;
    invalid.expectedDocumentRevision.reset();
    Require(
        ValidateEditorCommandEnvelope(invalid) ==
            EditorErrorCode::MissingExpectedDocumentRevision,
        "write command without revision was accepted");
    invalid = valid;
    auto& invalidPayload = std::get<SetMaterialParameterOverridePayload>(
        invalid.payload);
    invalidPayload.parameterType = EditorMaterialParameterType::Float;
    Require(
        ValidateEditorCommandEnvelope(invalid) == EditorErrorCode::InvalidPayload,
        "typed parameter mismatch was accepted");
    invalid = valid;
    auto& nonFinitePayload = std::get<SetMaterialParameterOverridePayload>(
        invalid.payload);
    nonFinitePayload.value = EditorVec3{
        std::numeric_limits<float>::quiet_NaN(),
        0.5f,
        0.6f};
    Require(
        ValidateEditorCommandEnvelope(invalid) == EditorErrorCode::InvalidPayload,
        "non-finite parameter value was accepted");

    EditorCommandBatchItem setItem;
    setItem.type = EditorCommandType::SetMaterialParameterOverride;
    setItem.payload = std::get<SetMaterialParameterOverridePayload>(valid.payload);
    EditorCommandBatchItem textureItem;
    textureItem.type = EditorCommandType::SetMaterialTextureOverride;
    textureItem.payload = SetMaterialTextureOverridePayload{
        "materials/MI_editor_test.json",
        "normalMap",
        "textures/T_normal_override.json"};
    EditorCommandBatchItem saveItem;
    saveItem.type = EditorCommandType::SaveMaterialInstanceDocument;
    saveItem.payload = MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};

    EditorCommandEnvelope batch;
    batch.commandId = 43;
    batch.source = EditorCommandSource::RuntimeTest;
    batch.type = EditorCommandType::ExecuteEditorCommandBatch;
    batch.expectedDocumentRevision = 7;
    batch.payload = ExecuteEditorCommandBatchPayload{
        {setItem, textureItem, saveItem}};
    Require(
        !ValidateEditorCommandEnvelope(batch).has_value(),
        "valid editor command batch was rejected");

    auto& invalidBatch = std::get<ExecuteEditorCommandBatchPayload>(batch.payload);
    std::get<SetMaterialTextureOverridePayload>(invalidBatch.commands[1].payload)
        .assetPath = "materials/MI_other.json";
    Require(
        ValidateEditorCommandEnvelope(batch) == EditorErrorCode::InvalidPayload,
        "batch with mixed asset paths was accepted");
}

void TestLandedCommandBus()
{
    EditorCommandBus bus(2);
    const EditorCommandEnvelope command = BuildSetParameterCommand(
        EditorCommandSource::AI,
        100,
        7);
    const EditorCommandSubmission queued = bus.Submit(command);
    Require(
        queued.admission == EditorCommandAdmission::Queued &&
            queued.result.status == EditorCommandStatus::Accepted &&
            bus.PendingCount() == 1,
        "command bus did not queue a valid command");

    const EditorCommandSubmission duplicate = bus.Submit(command);
    Require(
        duplicate.admission == EditorCommandAdmission::Duplicate &&
            duplicate.result.commandId == queued.commandId,
        "command bus did not deduplicate command ids");

    bus.SetDocumentRevision("materials/MI_editor_test.json", 7);
    EditorCommandEnvelope stale = command;
    stale.commandId = 101;
    stale.expectedDocumentRevision = 6;
    const EditorCommandSubmission rejected = bus.Submit(stale);
    Require(
        rejected.admission == EditorCommandAdmission::Rejected &&
            rejected.result.errorCode == EditorErrorCode::StaleDocumentRevision &&
            rejected.result.documentRevision == 7,
        "command bus accepted a stale document revision");

    const std::vector<EditorCommandEnvelope> drained = bus.Drain();
    Require(
        drained.size() == 1 && drained.front().commandId == 100 &&
            bus.GetResult(100)->status == EditorCommandStatus::Running,
        "command bus did not transition a drained command to Running");

    EditorCommandResult succeeded;
    succeeded.commandId = 100;
    succeeded.status = EditorCommandStatus::Succeeded;
    succeeded.errorCode = EditorErrorCode::None;
    succeeded.documentRevision = 8;
    succeeded.message = "completed";
    Require(bus.PublishResult(succeeded), "command bus rejected a terminal result");
    const auto stored = bus.GetEditorCommandResult(100);
    Require(
        stored.has_value() && stored->status == EditorCommandStatus::Succeeded &&
            stored->documentRevision == 8,
        "command bus did not retain the terminal result");
    Require(
        !bus.PublishResult(succeeded),
        "command bus allowed a terminal result to be overwritten");

    EditorCommandBus boundedBus(1);
    EditorCommandEnvelope first = BuildSetParameterCommand(
        EditorCommandSource::RuntimeTest,
        200,
        1);
    EditorCommandEnvelope second = first;
    second.commandId = 201;
    Require(
        boundedBus.Submit(first).admission == EditorCommandAdmission::Queued &&
            boundedBus.Submit(second).admission == EditorCommandAdmission::Queued,
        "bounded bus did not accept independent commands");
    boundedBus.Drain();
    EditorCommandResult firstResult;
    firstResult.commandId = 200;
    firstResult.status = EditorCommandStatus::Succeeded;
    firstResult.errorCode = EditorErrorCode::None;
    Require(boundedBus.PublishResult(firstResult), "first bounded result failed");
    boundedBus.Drain();
    EditorCommandResult secondResult = firstResult;
    secondResult.commandId = 201;
    Require(boundedBus.PublishResult(secondResult), "second bounded result failed");
    Require(
        !boundedBus.GetResult(200).has_value() &&
            boundedBus.GetResult(201).has_value(),
        "bounded result store did not prune its oldest terminal result");
}

MaterialInstanceSparseOverrides BuildWorkingOverrides(
    const MaterialInstanceDefaults& defaults)
{
    MaterialInstanceSparseOverrides working;
    for (const auto& [name, value] : defaults.parameters)
    {
        working.parameters.emplace(name, value);
    }
    for (const auto& [slot, value] : defaults.textures)
    {
        if (value.has_value())
        {
            working.textures.emplace(slot, *value);
        }
    }
    working.parameters.at("u_tint") = std::array<float, 3>{0.4f, 0.5f, 0.6f};
    working.parameters.at("u_packed") =
        std::array<float, 4>{0.1f, 0.25f, 0.3f, 0.4f};
    working.textures.at("normalMap") = "textures/T_normal_override.json";
    return working;
}

void RequireSparseCandidateShape(
    const Json& candidate)
{
    Require(
        candidate.at("parameters").size() == 2 &&
            candidate.at("parameters").contains("u_tint") &&
            candidate.at("parameters").contains("u_packed") &&
            !candidate.at("parameters").contains("u_scalar") &&
            !candidate.at("parameters").contains("u_uvScale"),
        "landed sparse candidate retained the wrong parameter overrides");
    Require(
        candidate.at("textures").size() == 1 &&
            candidate.at("textures").at("normalMap") ==
                "textures/T_normal_override.json" &&
            !candidate.at("textures").contains("albedoMap"),
        "landed sparse candidate retained the wrong texture overrides");
    Require(
        candidate.at("macros") ==
                Json({{"USE_DETAIL", 2}}) &&
            candidate.at("renderStateOverrides") ==
                Json({{"cullMode", "None"}}) &&
            candidate.at("configHelp").is_object(),
        "landed sparse candidate dropped unmanaged MI fields");
}

void TestLandedSparseCandidate()
{
    const Json material = LoadFixture("M_editor_test.json");
    const Json original = LoadFixture("MI_editor_test_original.json");
    const MaterialInstanceDefaults defaults =
        ParseMaterialInstanceDefaults(material);
    const MaterialInstanceSparseOverrides parsed =
        ParseMaterialInstanceSparseOverrides(original, defaults);
    Require(
        parsed.parameters.size() == 3 && parsed.textures.size() == 1,
        "landed sparse parser did not read the fixture overrides");

    const MaterialInstanceSparseOverrides working =
        BuildWorkingOverrides(defaults);
    const MaterialInstanceSparseCandidate candidate =
        BuildMaterialInstanceSparseCandidate(original, defaults, working);
    const Json serialized = SerializeMaterialInstanceSparseCandidate(candidate);
    Require(serialized.at("name") == "MI_editor_test", "sparse save changed MI identity");
    RequireSparseCandidateShape(serialized);
    Require(
        candidate.sourceJson == original,
        "sparse candidate did not retain its source snapshot");
    Require(
        SerializeMaterialInstanceSparseCandidateText(candidate).back() == '\n',
        "sparse candidate text did not end with the required newline");
    MaterialAssetValidator::ValidateInstanceHeader(
        serialized,
        "materials/MI_editor_test.json");
    MaterialAssetValidator::ValidateInstanceOverrides(
        material,
        serialized,
        "materials/MI_editor_test.json");

    MaterialInstanceSparseOverrides defaultsOnly;
    for (const auto& [name, value] : defaults.parameters)
    {
        defaultsOnly.parameters.emplace(name, value);
    }
    for (const auto& [slot, value] : defaults.textures)
    {
        if (value.has_value())
        {
            defaultsOnly.textures.emplace(slot, *value);
        }
    }
    const Json emptySerialized = SerializeMaterialInstanceSparseCandidate(
        BuildMaterialInstanceSparseCandidate(original, defaults, defaultsOnly));
    Require(
        !emptySerialized.contains("parameters") &&
            !emptySerialized.contains("textures"),
        "landed sparse save emitted empty override objects");

    MaterialInstanceSparseOverrides nearDefault = defaultsOnly;
    nearDefault.parameters.at("u_scalar") = 0.5000001f;
    const Json nearDefaultSerialized = SerializeMaterialInstanceSparseCandidate(
        BuildMaterialInstanceSparseCandidate(original, defaults, nearDefault));
    Require(
        nearDefaultSerialized.at("parameters").contains("u_scalar"),
        "landed sparse save applied an implicit epsilon");

    Require(
        IsMaterialInstanceTextureAssetPath("textures\\T_normal_override.json") &&
            !IsMaterialInstanceTextureAssetPath("textures/normal.png"),
        "landed texture asset path gate is not strict");
    RequireThrows(
        []() {
            ParseMaterialInstanceNumericValue(
                MaterialInstanceNumericType::Vec3,
                Json({0.1, 0.2}),
                "u_tint");
        },
        "landed sparse parser accepted an incomplete vector");
    RequireThrows(
        []() {
            ParseMaterialInstanceNumericValue(
                MaterialInstanceNumericType::Float,
                Json(std::numeric_limits<double>::quiet_NaN()),
                "u_scalar");
        },
        "landed sparse parser accepted a non-finite scalar");
    RequireThrows(
        [&defaults]() {
            MaterialInstanceSparseOverrides invalid;
            invalid.parameters.emplace("u_missing", 1.0f);
            BuildMaterialInstanceSparseCandidate(
                LoadFixture("MI_editor_test_original.json"),
                defaults,
                invalid);
        },
        "landed sparse candidate accepted an unknown parameter");
    RequireThrows(
        [&defaults]() {
            MaterialInstanceSparseOverrides invalid;
            invalid.textures.emplace("normalMap", "textures/normal.png");
            BuildMaterialInstanceSparseCandidate(
                LoadFixture("MI_editor_test_original.json"),
                defaults,
                invalid);
        },
        "landed sparse candidate accepted a raw image texture path");

    const Json extension = LoadFixture("MI_editor_test_opaque_extension.json");
    const Json extensionSerialized = SerializeMaterialInstanceSparseCandidate(
        BuildMaterialInstanceSparseCandidate(
            extension,
            defaults,
            defaultsOnly));
    Require(
        extensionSerialized.at("editorExtension") == extension.at("editorExtension"),
        "landed sparse candidate did not preserve an unmanaged extension");
}

void TestLandedPersistenceConflict()
{
    const std::filesystem::path temporaryRoot =
        std::filesystem::temp_directory_path() /
        ("vulkanlearn_material_instance_editor_landed_" +
         std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    const std::filesystem::path materialInstancePath =
        temporaryRoot / "materials" / "MI_editor_test.json";

    try
    {
        std::filesystem::create_directories(materialInstancePath.parent_path());
        std::filesystem::copy_file(
            kFixtureRoot / "MI_editor_test_original.json",
            materialInstancePath,
            std::filesystem::copy_options::overwrite_existing);

        const auto snapshot = MaterialInstancePersistence::ReadSnapshot(
            materialInstancePath);
        Require(
            snapshot.status == MaterialInstancePersistenceStatus::SnapshotReady &&
                snapshot.Succeeded(),
            "landed persistence did not read an MI source snapshot");
        const Json candidate = LoadFixture("expected_sparse_candidate.json");
        const std::string candidateText = candidate.dump(2) + "\n";
        const auto saved = MaterialInstancePersistence::SaveTextIfUnchanged(
            materialInstancePath,
            snapshot.digest,
            candidateText);
        Require(
            saved.status == MaterialInstancePersistenceStatus::Saved &&
                saved.Succeeded() &&
                saved.newDigest == VL::ContentHasher::HashString(candidateText),
            "landed persistence did not atomically save a current candidate");

        Json savedJson;
        {
            std::ifstream savedInput(materialInstancePath);
            savedInput >> savedJson;
        }
        Require(savedJson == candidate, "landed persistence wrote the wrong JSON candidate");

        VL::WriteTextFileAtomically(
            materialInstancePath,
            ReadTextFile(materialInstancePath) + "\nexternal edit\n");
        const std::string bytesBeforeConflict = ReadTextFile(materialInstancePath);
        const auto conflict = MaterialInstancePersistence::SaveTextIfUnchanged(
            materialInstancePath,
            snapshot.digest,
            "must not overwrite");
        Require(
            conflict.status == MaterialInstancePersistenceStatus::SourceChanged &&
                conflict.HasSourceConflict(),
            "landed persistence did not report a source conflict");
        Require(
            ReadTextFile(materialInstancePath) == bytesBeforeConflict,
            "landed persistence changed the file after a source conflict");

        const auto missing = MaterialInstancePersistence::ReadSnapshot(
            temporaryRoot / "materials" / "MI_missing.json");
        Require(
            missing.status == MaterialInstancePersistenceStatus::Missing,
            "landed persistence did not report a missing MI");
        const auto invalidPath = MaterialInstancePersistence::ReadSnapshot({});
        Require(
            invalidPath.status == MaterialInstancePersistenceStatus::InvalidPath,
            "landed persistence accepted an empty path");
        const auto directory = MaterialInstancePersistence::ReadSnapshot(
            materialInstancePath.parent_path());
        Require(
            directory.status == MaterialInstancePersistenceStatus::NotRegularFile,
            "landed persistence accepted a directory as an MI file");
        const auto nullWrite = MaterialInstancePersistence::SaveBytesIfUnchanged(
            materialInstancePath,
            VL::ContentHasher::HashFile(materialInstancePath),
            nullptr,
            1);
        Require(
            nullWrite.status == MaterialInstancePersistenceStatus::WriteFailed,
            "landed persistence accepted a null byte range");
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove_all(temporaryRoot, ignored);
        throw;
    }

    std::error_code ignored;
    std::filesystem::remove_all(temporaryRoot, ignored);
}

void TestLandedPreviewTypes()
{
    const auto normalized = NormalizeMaterialInstancePath(
        "materials\\MI_editor_test.json");
    Require(
        normalized.Succeeded() && normalized.path->value ==
            "materials/MI_editor_test.json",
        "preview path normalization did not normalize separators");
    for (const std::string path : {
             "../materials/MI_editor_test.json",
             "materials/MI_editor_test.txt",
             "materials/T_not_an_mi.json",
             "C:/outside/MI_editor_test.json"})
    {
        Require(
            !NormalizeMaterialInstancePath(path).Succeeded(),
            "preview path normalization accepted an invalid MI path");
    }

    MaterialInstancePreviewWorldIdentity world{3, "scenes\\scene_editor.json"};
    Require(world.IsValid(), "valid preview World identity was rejected");
    world = NormalizeWorldIdentity(world);
    Require(
        world.scenePath == "scenes/scene_editor.json",
        "preview World identity did not normalize its scene path");

    MaterialInstancePreviewDraft draft{
        "materials/MI_editor_test.json",
        9,
        LoadFixture("expected_sparse_candidate.json").dump()};
    MaterialInstancePreviewDraft sameDraft = draft;
    Require(draft == sameDraft && !(draft != sameDraft), "preview draft equality is unstable");
    sameDraft.documentRevision = 10;
    Require(draft != sameDraft, "preview draft revision was ignored");

    MaterialInstancePreviewCommand command;
    command.commandId = 500;
    command.correlationId = 77;
    command.source = PreviewCommandSource::RuntimeTest;
    command.type = MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview;
    command.expectedDocumentRevision = 9;
    command.expectedPreviewGeneration = 3;
    command.payload =
        VL::Editor::Preview::ApplyMaterialInstancePreviewPayload{draft};
    Require(
        ToString(command.type) == std::string("material.preview.apply") &&
            std::get<VL::Editor::Preview::ApplyMaterialInstancePreviewPayload>(
                command.payload)
                    .workingDraft == draft,
        "preview apply command did not retain its typed draft");

    MaterialInstancePreviewStatus status;
    status.state = MaterialInstancePreviewState::Connected;
    status.world = world;
    status.materialInstancePath = normalized.path;
    status.liveGeneration = 3;
    status.observedDocumentRevision = 9;
    Require(status.IsConnected(), "connected preview status was not connected");
    status.state = MaterialInstancePreviewState::Failed;
    status.lastError = MaterialInstancePreviewErrorCode::PreviewPrepareFailed;
    Require(!status.IsConnected(), "failed preview status remained connected");

    MaterialInstancePreviewCommandResult result;
    result.commandId = command.commandId;
    result.status = MaterialInstancePreviewCommandStatus::Running;
    Require(!result.IsTerminal(), "running preview result was marked terminal");
    result.status = MaterialInstancePreviewCommandStatus::Succeeded;
    result.errorCode = MaterialInstancePreviewErrorCode::None;
    result.payload.previewStatus = status;
    result.payload.appliedDocumentRevision = 9;
    result.payload.liveSwapCommitted = true;
    Require(
        result.IsTerminal() &&
            ToString(result.errorCode) == std::string("None") &&
            result.payload.appliedDocumentRevision == 9,
        "terminal preview result contract is incomplete");
}

void TestLandedPreviewProtocolBoundaries()
{
    using namespace VL::Editor::Preview;
    using material_instance_editor_test::FakeMaterialInstancePreviewAdapter;

    FakeMaterialInstancePreviewAdapter adapter;
    MaterialInstancePreviewController controller(adapter);
    const MaterialInstancePreviewWorldIdentity world{
        4,
        "scenes/scene_editor.json"};

    MaterialInstancePreviewCommand command;
    command.commandId = 600;
    command.source = PreviewCommandSource::RuntimeTest;
    command.type = MaterialInstancePreviewCommandType::
        ConnectMaterialInstancePreview;
    command.payload = ConnectMaterialInstancePreviewPayload{
        world,
        "materials/MI_editor_test.json",
        9};

    MaterialInstancePreviewCommandResult result = controller.Execute(command);
    Require(
        result.status == MaterialInstancePreviewCommandStatus::Rejected &&
            result.errorCode == MaterialInstancePreviewErrorCode::PreviewUnavailable,
        "preview connect without an active World crossed the protocol boundary");

    controller.NotifyActiveWorldChanged(world);

    command.protocolVersion = kMaterialInstancePreviewProtocolVersion + 1;
    result = controller.Execute(command);
    Require(
        result.errorCode == MaterialInstancePreviewErrorCode::InvalidProtocolVersion,
        "preview protocol version boundary was not rejected");

    command.protocolVersion = kMaterialInstancePreviewProtocolVersion;
    command.commandId = 0;
    result = controller.Execute(command);
    Require(
        result.errorCode == MaterialInstancePreviewErrorCode::InvalidCommand,
        "zero preview command id crossed the protocol boundary");

    command.commandId = 601;
    command.expectedPreviewGeneration =
        controller.GetStatus().liveGeneration + 1;
    result = controller.Execute(command);
    Require(
        result.errorCode == MaterialInstancePreviewErrorCode::PreviewGenerationChanged,
        "stale preview generation crossed the protocol boundary");

    command.expectedPreviewGeneration.reset();
    command.payload = DisconnectMaterialInstancePreviewPayload{};
    result = controller.Execute(command);
    Require(
        result.errorCode == MaterialInstancePreviewErrorCode::InvalidCommand,
        "mismatched preview payload crossed the protocol boundary");

    MaterialInstancePreviewCommand apply;
    apply.commandId = 602;
    apply.source = PreviewCommandSource::RuntimeTest;
    apply.type = MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview;
    apply.payload = VL::Editor::Preview::ApplyMaterialInstancePreviewPayload{
        MaterialInstancePreviewDraft{
            "materials/MI_editor_test.json",
            9,
            "{\"parameters\":{}}"}};
    result = controller.Execute(apply);
    Require(
        result.errorCode == MaterialInstancePreviewErrorCode::PreviewNotConnected,
        "preview apply without a connection crossed the protocol boundary");

    adapter.QueueExecuteResult(MaterialInstancePreviewAdapterResult{
        PreviewAdapterOperationStatus::Pending,
        701,
        0,
        false,
        false,
        PreviewAdapterFailureStage::None,
        "fake apply pending"});
    command.commandId = 603;
    command.type = MaterialInstancePreviewCommandType::ConnectMaterialInstancePreview;
    command.payload = ConnectMaterialInstancePreviewPayload{
        world,
        "materials/MI_editor_test.json",
        9};
    result = controller.Execute(command);
    Require(
        result.status == MaterialInstancePreviewCommandStatus::Running &&
            controller.HasPendingOperation(),
        "pending preview operation was not retained");

    MaterialInstancePreviewCommand blocked = apply;
    blocked.commandId = 604;
    result = controller.Execute(blocked);
    Require(
        result.errorCode == MaterialInstancePreviewErrorCode::PreviewOperationInProgress,
        "second preview operation crossed the in-progress boundary");

    controller.NotifyActiveWorldChanged(
        MaterialInstancePreviewWorldIdentity{5, "scenes/scene_other.json"});
    Require(
        !adapter.canceledOperationIds.empty() &&
            adapter.canceledOperationIds.back() == 701 &&
            !controller.HasPendingOperation(),
        "World generation change did not cancel the pending preview operation");
}

} // namespace

int main()
{
    try
    {
        TestLandedCommandSchema();
        TestLandedJsonCommandCodec();
        TestLandedCommandBus();
        TestLandedSparseCandidate();
        TestLandedPersistenceConflict();
        TestLandedPreviewTypes();
        TestLandedPreviewProtocolBoundaries();
        std::cout << "Material instance editor landed-interface tests passed."
                  << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Material instance editor landed-interface tests failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}
