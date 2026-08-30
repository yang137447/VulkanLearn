#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "editor/editorFacade.h"
#include "material/materialDescriptorSchema.h"
#include "material/validation/materialAssetValidator.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"
#include "support/materialInstanceEditorContract.h"

namespace
{

using Json = nlohmann::json;
using namespace material_instance_editor_test;

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

Json BuildDefaultParameters(
    const Json& material)
{
    Json defaults = Json::object();
    for (const auto& [name, descriptor] : material.at("parameters").items())
    {
        defaults[name] = descriptor.at("default");
    }
    return defaults;
}

Json BuildDefaultTextures(
    const Json& material)
{
    Json defaults = Json::object();
    for (const auto& [name, descriptor] : material.at("textures").items())
    {
        defaults[name] = descriptor.at("default");
    }
    return defaults;
}

std::vector<std::string> CommandNames()
{
    return {
        "material.resolve_scene_reference",
        "material.list_assets",
        "material.open",
        "material.select",
        "material.close",
        "material.get_document",
        "material.get_reference_context",
        "material.set_parameter",
        "material.clear_parameter",
        "material.set_texture",
        "material.clear_texture",
        "material.set_render_state",
        "material.clear_render_state",
        "material.reset_overrides",
        "material.validate",
        "material.save",
        "material.revert",
        "material.reload",
        "material.preview.connect",
        "material.preview.apply",
        "material.preview.restore_baseline",
        "material.preview.disconnect",
        "editor.get_command_result",
        "editor.list_events",
        "editor.execute_batch"};
}

void TestCommandCodecAndSchema()
{
    const Json schema = LoadFixture("editor_command_schema_v1.json");
    Require(schema.at("protocolVersion") == 1, "unexpected editor protocol version");
    Require(
        schema.at("commands").size() == CommandNames().size(),
        "editor command schema has an unexpected command count");
    for (const std::string& command : CommandNames())
    {
        Require(
            schema.at("commands").contains(command),
            "editor command schema is missing " + command);
    }

    const Json parameterWire = LoadFixture("command_set_parameter.json");
    const CommandEnvelope parameterCommand = DecodeCommandEnvelope(
        parameterWire,
        schema);
    Require(
        parameterCommand.payload.at("assetPath") ==
            "materials/MI_editor_test.json",
        "command codec did not normalize MI path separators");
    Require(
        parameterCommand.payload.at("value").size() == 3,
        "command codec lost complete vec3 value");
    Require(
        EncodeCommandEnvelope(parameterCommand) ==
            Json({
                {"protocolVersion", 1},
                {"commandId", 1001},
                {"correlationId", 77},
                {"source", "ai"},
                {"command", "material.set_parameter"},
                {"expectedDocumentRevision", 7},
                {"payload", {
                    {"assetPath", "materials/MI_editor_test.json"},
                    {"parameter", "u_tint"},
                    {"type", "vec3"},
                    {"value", {0.4, 0.5, 0.6}}}}}),
        "command codec round-trip changed the wire contract");

    for (const Json& sourceValue : schema.at("sources"))
    {
        const std::string source = sourceValue.get<std::string>();
        Json producerWire = parameterWire;
        producerWire["source"] = source;
        const CommandEnvelope producerCommand = DecodeCommandEnvelope(
            producerWire,
            schema);
        Require(
            producerCommand.command == parameterCommand.command &&
                producerCommand.expectedDocumentRevision ==
                    parameterCommand.expectedDocumentRevision &&
                producerCommand.payload == parameterCommand.payload,
            "producer-specific source changed command semantics");
    }

    const CommandEnvelope textureCommand = DecodeCommandEnvelope(
        LoadFixture("command_set_texture.json"),
        schema);
    Require(
        textureCommand.payload.at("textureAssetPath") ==
            "textures/T_normal_override.json",
        "texture command path was not normalized");

    const CommandEnvelope renderStateCommand = DecodeCommandEnvelope(
        LoadFixture("command_set_render_state.json"),
        schema);
    Require(
        renderStateCommand.payload.at("field") == "cullMode" &&
            renderStateCommand.payload.at("value") == "None",
        "render state command did not decode its typed field and value");
    Require(
        EncodeCommandEnvelope(renderStateCommand) ==
            Json({
                {"protocolVersion", 1},
                {"commandId", 1005},
                {"source", "imgui"},
                {"command", "material.set_render_state"},
                {"expectedDocumentRevision", 8},
                {"payload", {
                    {"assetPath", "materials/MI_editor_test.json"},
                    {"field", "cullMode"},
                    {"value", "None"}}}}),
        "render state command round-trip changed the wire contract");

    const CommandEnvelope previewCommand = DecodeCommandEnvelope(
        LoadFixture("command_preview_apply.json"),
        schema);
    Require(
        previewCommand.payload.at("documentRevision") == 9,
        "preview command did not carry the draft revision");

    const CommandResult succeeded = DecodeCommandResult(
        LoadFixture("command_result_succeeded.json"),
        schema);
    Require(
        succeeded.commandId == 1001 &&
        succeeded.status == "Succeeded" &&
            succeeded.errorCode == "None" &&
            succeeded.documentRevision == 8,
        "succeeded command result did not decode correctly");

    const CommandResult rejected = DecodeCommandResult(
        LoadFixture("command_result_rejected.json"),
        schema);
    Require(
        rejected.status == "Rejected" &&
            rejected.errorCode == "StaleDocumentRevision",
        "rejected command result did not preserve its stable error code");

    RequireThrows(
        [&schema]() {
            DecodeCommandEnvelope(
                LoadFixture("command_invalid_vector_arity.json"),
                schema);
        },
        "codec accepted an incomplete vector value");
    RequireThrows(
        [&schema]() {
            DecodeCommandEnvelope(
                LoadFixture("command_invalid_texture_path.json"),
                schema);
        },
        "codec accepted a direct image texture path");
    RequireThrows(
        [&schema]() {
            DecodeCommandEnvelope(
                LoadFixture("command_unknown_field.json"),
                schema);
        },
        "codec accepted a non-serializable payload field");

    Json missingRevision = parameterWire;
    missingRevision.erase("expectedDocumentRevision");
    RequireThrows(
        [&schema, &missingRevision]() {
            DecodeCommandEnvelope(missingRevision, schema);
        },
        "write command without expected revision was accepted");
    RequireThrows(
        [&schema]() {
            Json invalid = LoadFixture("command_set_parameter.json");
            invalid["protocolVersion"] = 2;
            DecodeCommandEnvelope(invalid, schema);
        },
        "unsupported editor protocol version was accepted");
}

void TestMaterialSchemaAndAssetValidation()
{
    const Json material = LoadFixture("M_editor_test.json");
    const Json materialInstance = LoadFixture("MI_editor_test_original.json");
    MaterialAssetValidator::ValidateDefinition(
        material,
        "shader/glsl/M_editor_test.json");
    MaterialAssetValidator::ValidateInstanceHeader(
        materialInstance,
        "materials/MI_editor_test.json");
    MaterialAssetValidator::ValidateInstanceOverrides(
        material,
        materialInstance,
        "materials/MI_editor_test.json");

    const VL::MaterialDescriptorSchema schema = VL::MaterialDescriptorSchema::Build(
        material,
        "shader/glsl/M_editor_test.json");
    Require(schema.GetParameters().size() == 4, "schema parameter count mismatch");
    Require(schema.GetTextures().size() == 2, "schema texture count mismatch");
    Require(
        schema.GetParameters()[0].name == "u_packed" &&
            schema.GetParameters()[0].offset == 0 &&
            schema.GetParameters()[1].name == "u_tint" &&
            schema.GetParameters()[1].offset == 16 &&
            schema.GetParameters()[2].name == "u_uvScale" &&
            schema.GetParameters()[2].offset == 32 &&
            schema.GetParameters()[3].name == "u_scalar" &&
            schema.GetParameters()[3].offset == 40,
        "schema did not preserve deterministic std140 ordering");
    Require(
        schema.GetParameters()[1].channels.size() == 3 &&
            schema.GetParameters()[1].channels[0].name == "red" &&
            schema.GetParameters()[1].channels[2].max == 1.0f,
        "schema did not retain vector channel metadata");

    const Json effectiveParameters = BuildDefaultParameters(material);
    const Json effectiveTextures = BuildDefaultTextures(material);
    ShaderBinding activeNormalBinding;
    activeNormalBinding.set = 1;
    activeNormalBinding.binding = schema.GetTextures()[1].binding;
    activeNormalBinding.type = vk::DescriptorType::eCombinedImageSampler;
    activeNormalBinding.name = "normalMap";
    schema.ValidateInstanceValues(
        effectiveParameters,
        effectiveTextures,
        {activeNormalBinding},
        "materials/MI_editor_test.json");

    Json inactiveTextureSet = effectiveTextures;
    inactiveTextureSet.erase("albedoMap");
    schema.ValidateInstanceValues(
        effectiveParameters,
        inactiveTextureSet,
        {activeNormalBinding},
        "materials/MI_editor_test.json");
    Json missingActiveTexture = effectiveTextures;
    missingActiveTexture.erase("normalMap");
    RequireThrows(
        [&schema, &effectiveParameters, &missingActiveTexture, &activeNormalBinding]() {
            schema.ValidateInstanceValues(
                effectiveParameters,
                missingActiveTexture,
                {activeNormalBinding},
                "materials/MI_editor_test.json");
        },
        "schema accepted a missing active texture binding");

    RequireThrows(
        []() {
            MaterialAssetValidator::ValidateDefinition(
                LoadFixture("M_editor_test_invalid_channels.json"),
                "shader/glsl/M_editor_test_invalid_channels.json");
        },
        "material validator accepted incomplete channel metadata");
    RequireThrows(
        []() {
            const Json materialValue = LoadFixture("M_editor_test.json");
            MaterialAssetValidator::ValidateInstanceHeader(
                LoadFixture("MI_editor_test_unknown_parameter.json"),
                "materials/MI_editor_test_unknown_parameter.json");
            MaterialAssetValidator::ValidateInstanceOverrides(
                materialValue,
                LoadFixture("MI_editor_test_unknown_parameter.json"),
                "materials/MI_editor_test_unknown_parameter.json");
        },
        "material validator accepted an unknown parameter override");
    RequireThrows(
        []() {
            const Json materialValue = LoadFixture("M_editor_test.json");
            const Json instanceValue = LoadFixture("MI_editor_test_redundant_default.json");
            MaterialAssetValidator::ValidateInstanceOverrides(
                materialValue,
                instanceValue,
                "materials/MI_editor_test_redundant_default.json");
        },
        "material validator accepted a redundant default override");

    MaterialAssetValidator::ValidateRenderStateCombination(
        "Eye",
        "ForwardEyeInner",
        "materials/MI_editor_test.json");
    RequireThrows(
        []() {
            MaterialAssetValidator::ValidateRenderStateCombination(
                "DefaultLit",
                "ForwardEyeInner",
                "materials/MI_editor_test.json");
        },
        "material validator accepted a non-Eye forward render path");
    RequireThrows(
        []() {
            MaterialAssetValidator::ValidateRenderStateCombination(
                "ThinTranslucent",
                "TransparentAlphaBlend",
                "materials/MI_editor_test.json");
        },
        "material validator accepted an incompatible thin-translucent pairing");
}

void TestNavigationAndDocumentBasics()
{
    const Json navigation = LoadFixture("navigation_references.json");
    const NavigationReference meshReference = ResolveNavigationReference(
        navigation,
        "mesh-car",
        "0");
    Require(
        meshReference.objectType == "mesh" &&
            meshReference.assetPath == "models/SM_editor_car.json" &&
            meshReference.selector == "0" &&
            meshReference.materialInstancePath ==
                "materials/MI_editor_test.json",
        "mesh navigation did not produce the expected breadcrumb");

    const NavigationReference terrainReference = ResolveNavigationReference(
        navigation,
        "terrain-ground",
        "");
    Require(
        terrainReference.objectType == "terrain" &&
            terrainReference.selector == "surface" &&
            terrainReference.materialInstancePath ==
                "materials/MI_editor_test.json",
        "terrain navigation did not resolve its material slot");
    RequireThrows(
        [&navigation]() {
            ResolveNavigationReference(navigation, "mesh-car", "missing");
        },
        "navigation accepted a missing mesh section");
    RequireThrows(
        [&navigation]() {
            ResolveNavigationReference(navigation, "unknown-object", "");
        },
        "navigation accepted an unknown scene object");

    const Json documentFixture = LoadFixture("document_contract.json");
    const Json baseline = LoadFixture("MI_editor_test_original.json");
    DocumentContractRegistry registry;
    registry.Open(
        documentFixture.at("assetPath").get<std::string>(),
        documentFixture.at("origins").at(0).get<std::string>(),
        documentFixture.at("schemaDigest").get<std::string>(),
        baseline);
    registry.Open(
        "materials/MI_editor_test.json",
        documentFixture.at("origins").at(1).get<std::string>(),
        documentFixture.at("schemaDigest").get<std::string>(),
        baseline);
    Require(registry.Contains("materials/MI_editor_test.json"), "document was not opened");
    auto snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        snapshot.revision == 1 && !snapshot.dirty &&
            snapshot.state == "clean" && snapshot.originCount == 2,
        "same-path document singleton or origin retention failed");

    const Json working = LoadFixture("expected_sparse_candidate.json");
    Require(
        registry.SetWorking("materials/MI_editor_test.json", 1, working),
        "working document update was rejected at its current revision");
    snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        snapshot.revision == 2 && snapshot.dirty && snapshot.state == "dirty",
        "working document did not become dirty with a new revision");
    Require(
        !registry.SetWorking("materials/MI_editor_test.json", 1, baseline),
        "stale document revision was accepted");
    Require(
        registry.GetSnapshot("materials/MI_editor_test.json").revision == 2,
        "stale command changed document state");

    registry.ConnectPreview("materials/MI_editor_test.json");
    Require(
        registry.GetSnapshot("materials/MI_editor_test.json").previewConnected,
        "preview connection was not recorded");
    registry.WorldChanged(documentFixture.at("worldAfter").get<std::string>());
    snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        snapshot.previewConnected == false && snapshot.dirty &&
            registry.Contains("materials/MI_editor_test.json"),
        "World change closed the document or retained a stale preview connection");

    registry.MarkSchemaChanged("materials/MI_editor_test.json", "schema-v2");
    snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        snapshot.state == "source_changed" && snapshot.dirty,
        "schema change did not expose Source Changed state");
    Require(
        !registry.Save("materials/MI_editor_test.json", snapshot.revision, "schema-v2"),
        "save bypassed a Source Changed document state");

    Require(
        !registry.Revert("materials/MI_editor_test.json", snapshot.revision),
        "revert bypassed a Source Changed document state");
    Require(
        registry.Reload(
            "materials/MI_editor_test.json",
            snapshot.revision,
            "schema-v2",
            baseline),
        "explicit reload did not recover a Source Changed document");
    snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        snapshot.state == "clean" && !snapshot.dirty,
        "revert did not restore the clean baseline");
    Require(
        registry.SetWorking("materials/MI_editor_test.json", snapshot.revision, working),
        "second working update was rejected");
    snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        registry.Save("materials/MI_editor_test.json", snapshot.revision, "schema-v2"),
        "clean save did not update the document baseline");
    snapshot = registry.GetSnapshot("materials/MI_editor_test.json");
    Require(
        snapshot.state == "clean" && !snapshot.dirty &&
            snapshot.schemaDigest == "schema-v2",
        "save did not clear dirty state and update schema digest");

    Require(
        registry.Close("materials/MI_editor_test.json", ClosePolicy::Cancel),
        "clean document could not be closed");
    Require(!registry.Contains("materials/MI_editor_test.json"), "closed document remained open");

    registry.Open("materials/MI_editor_test.json", "browser:reopen", "schema-v2", baseline);
    Require(
        registry.SetWorking("materials/MI_editor_test.json", 1, working),
        "reopened document could not become dirty");
    Require(
        !registry.Close("materials/MI_editor_test.json", ClosePolicy::Cancel),
        "dirty document closed without an explicit discard policy");
    Require(
        registry.Close("materials/MI_editor_test.json", ClosePolicy::Discard),
        "dirty document could not be discarded explicitly");
}

void TestSparseCandidateAndTextureValidation()
{
    const Json material = LoadFixture("M_editor_test.json");
    const Json original = LoadFixture("MI_editor_test_original.json");
    const Json workingParameters = LoadFixture("working_parameters.json");
    const Json workingTextures = LoadFixture("working_textures.json");
    const Json expected = LoadFixture("expected_sparse_candidate.json");

    const Json candidate = BuildSparseCandidate(
        original,
        material,
        workingParameters,
        workingTextures);
    Require(candidate == expected, "sparse candidate did not match the fixture oracle");
    Require(
        original.at("parameters").contains("u_scalar") &&
            original.at("textures").contains("albedoMap"),
        "sparse candidate builder mutated the source document");
    Require(
        candidate.at("macros") == original.at("macros") &&
            candidate.at("renderStateOverrides") == original.at("renderStateOverrides") &&
            candidate.at("configHelp") == original.at("configHelp"),
        "sparse candidate dropped unmanaged MI fields");

    const Json emptyCandidate = BuildSparseCandidate(
        original,
        material,
        BuildDefaultParameters(material),
        BuildDefaultTextures(material));
    Require(
        emptyCandidate == LoadFixture("expected_empty_candidate.json"),
        "sparse save retained empty parameters or textures objects");
    Require(
        !emptyCandidate.contains("parameters") &&
            !emptyCandidate.contains("textures"),
        "sparse save emitted empty override objects");

    Json nearDefaultParameters = BuildDefaultParameters(material);
    nearDefaultParameters["u_scalar"] = 0.50000006;
    const Json nearDefaultCandidate = BuildSparseCandidate(
        original,
        material,
        nearDefaultParameters,
        BuildDefaultTextures(material));
    Require(
        nearDefaultCandidate.at("parameters").at("u_scalar") ==
            0.50000006,
        "sparse save silently applied an epsilon to a user value");

    MaterialAssetValidator::ValidateInstanceHeader(
        candidate,
        "materials/MI_editor_test.json");
    MaterialAssetValidator::ValidateInstanceOverrides(
        material,
        candidate,
        "materials/MI_editor_test.json");

    const Json extensionOriginal = LoadFixture("MI_editor_test_opaque_extension.json");
    const Json extensionCandidate = BuildSparseCandidate(
        extensionOriginal,
        material,
        BuildDefaultParameters(material),
        BuildDefaultTextures(material));
    Require(
        extensionCandidate.contains("editorExtension") &&
            extensionCandidate.at("editorExtension") ==
                extensionOriginal.at("editorExtension"),
        "sparse save did not preserve an unmanaged top-level extension");

    ValidateTextureAssetDocument(
        kFixtureRoot,
        "textures/T_albedo_default.json");
    ValidateTextureAssetDocument(
        kFixtureRoot,
        "textures\\T_normal_override.json");
    RequireThrows(
        []() {
            ValidateTextureAssetDocument(
                kFixtureRoot,
                "textures/T_missing_source.json");
        },
        "texture validator accepted a missing source image");
    RequireThrows(
        []() {
            ValidateTextureAssetDocument(
                kFixtureRoot,
                "textures/datas/albedo_default.ppm");
        },
        "texture validator accepted a raw image path as an asset reference");
    RequireThrows(
        []() {
            NormalizeRelativeAssetPath("../outside/MI_invalid.json");
        },
        "asset path normalization allowed traversal outside the resource root");
}

void TestConflictAndAtomicCandidateCommit()
{
    const std::filesystem::path temporaryRoot =
        std::filesystem::temp_directory_path() /
        ("vulkanlearn_material_instance_editor_contract_" +
         std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    const std::filesystem::path materialInstancePath =
        temporaryRoot / "materials" / "MI_editor_test.json";

    try
    {
        std::filesystem::create_directories(materialInstancePath.parent_path());
        const std::filesystem::path sourceFixture =
            kFixtureRoot / "MI_editor_test_original.json";
        std::filesystem::copy_file(
            sourceFixture,
            materialInstancePath,
            std::filesystem::copy_options::overwrite_existing);

        const VL::ContentDigest baselineDigest =
            VL::ContentHasher::HashFile(materialInstancePath);
        const std::string externallyChanged =
            ReadTextFile(materialInstancePath) + "\nexternal change\n";
        VL::WriteTextFileAtomically(materialInstancePath, externallyChanged);
        const std::string bytesBeforeRejectedCommit =
            ReadTextFile(materialInstancePath);
        const CommitResult rejected = TryCommitCandidate(
            materialInstancePath,
            baselineDigest,
            LoadFixture("expected_sparse_candidate.json"));
        Require(
            !rejected.succeeded && rejected.errorCode == "source_changed",
            "stale source digest did not reject a candidate save");
        Require(
            ReadTextFile(materialInstancePath) == bytesBeforeRejectedCommit,
            "rejected source conflict modified the on-disk asset");

        const VL::ContentDigest currentDigest =
            VL::ContentHasher::HashFile(materialInstancePath);
        const Json candidate = LoadFixture("expected_sparse_candidate.json");
        const CommitResult committed = TryCommitCandidate(
            materialInstancePath,
            currentDigest,
            candidate);
        Require(
            committed.succeeded && committed.errorCode == "none",
            "candidate with current source digest did not commit atomically");
        Json committedJson;
        std::ifstream committedInput(materialInstancePath);
        committedInput >> committedJson;
        Require(committedJson == candidate, "atomic commit wrote a different candidate");

        const CommitResult missing = TryCommitCandidate(
            temporaryRoot / "materials" / "MI_missing.json",
            currentDigest,
            candidate);
        Require(
            !missing.succeeded && missing.errorCode == "asset_not_found",
            "missing MI save did not return a stable asset-not-found error");
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

class ResultStoreContract
{
public:
    void Put(const Json& result)
    {
        const uint64_t commandId = result.at("commandId").get<uint64_t>();
        const std::string status = result.at("status").get<std::string>();
        const auto existing = results.find(commandId);
        if (existing != results.end() && IsFinal(existing->second))
        {
            return;
        }
        results[commandId] = result;
        if (IsFinal(result))
        {
            finalIds.insert(commandId);
        }
        (void)status;
    }

    const Json& Get(uint64_t commandId) const
    {
        const auto it = results.find(commandId);
        Require(it != results.end(), "command result was not stored");
        return it->second;
    }

private:
    static bool IsFinal(const Json& result)
    {
        const std::string status = result.at("status").get<std::string>();
        return status == "Succeeded" || status == "Rejected" || status == "Failed";
    }

    std::map<uint64_t, Json> results;
    std::set<uint64_t> finalIds;
};

void TestPreviewRequestAndResultQuery()
{
    const Json schema = LoadFixture("editor_command_schema_v1.json");
    const CommandEnvelope preview = DecodeCommandEnvelope(
        LoadFixture("command_preview_apply.json"),
        schema);
    Require(
        preview.command == "material.preview.apply" &&
            preview.payload.at("documentRevision") == 9,
        "preview request did not identify its document revision");

    ResultStoreContract resultStore;
    Json accepted = {
        {"protocolVersion", 1},
        {"commandId", preview.commandId},
        {"status", "Accepted"},
        {"errorCode", "None"},
        {"message", "preview queued"},
        {"payload", {{"documentRevision", 9}}}};
    Json running = accepted;
    running["status"] = "Running";
    running["message"] = "preview preparing";
    Json succeeded = accepted;
    succeeded["status"] = "Succeeded";
    succeeded["message"] = "preview applied";
    succeeded["documentRevision"] = 9;

    DecodeCommandResult(accepted, schema);
    DecodeCommandResult(running, schema);
    DecodeCommandResult(succeeded, schema);
    resultStore.Put(accepted);
    resultStore.Put(running);
    resultStore.Put(succeeded);
    const Json finalResult = resultStore.Get(preview.commandId);
    Require(
        finalResult.at("status") == "Succeeded" &&
            finalResult.at("documentRevision") == 9,
        "result query did not expose the final preview state");

    Json lateFailure = succeeded;
    lateFailure["status"] = "Failed";
    lateFailure["errorCode"] = "PreviewCommitFailed";
    resultStore.Put(lateFailure);
    Require(
        resultStore.Get(preview.commandId) == finalResult,
        "a terminal command result was overwritten by a later result");

    const CommandEnvelope resultQuery = DecodeCommandEnvelope(
        Json({
            {"protocolVersion", 1},
            {"commandId", 1005},
            {"source", "ai"},
            {"command", "editor.get_command_result"},
            {"payload", {{"commandId", preview.commandId}}}}),
        schema);
    Require(
        resultQuery.payload.at("commandId") == preview.commandId,
        "result query command did not preserve the target command id");
}

void TestEditorFacadeCommandFlow()
{
    VL::EditorFacade facade;
    VL::EditorCommandEnvelope command;
    command.payload = VL::MaterialInstanceAssetPathPayload{
        "materials/MI_editor_test.json"};
    const VL::EditorCommandSubmission submission = facade.Submit(command);
    Require(
        submission.admission == VL::EditorCommandAdmission::Queued &&
            submission.commandId != 0 &&
            facade.PendingCount() == 1,
        "editor facade did not queue a valid command");

    const std::vector<VL::EditorCommandEnvelope> drained = facade.Drain();
    Require(
        drained.size() == 1 &&
            drained.front().commandId == submission.commandId &&
            facade.PendingCount() == 0,
        "editor facade did not expose the queued command to the owner thread");

    VL::EditorCommandResult result;
    result.commandId = submission.commandId;
    result.status = VL::EditorCommandStatus::Succeeded;
    result.message = "command completed";
    Require(
        facade.PublishResult(std::move(result)),
        "editor facade did not publish the command result");
    const auto storedResult = facade.GetResult(submission.commandId);
    Require(
        storedResult.has_value() &&
            storedResult->status == VL::EditorCommandStatus::Succeeded,
        "editor facade did not retain the terminal command result");
}

} // namespace

int main()
{
    try
    {
        TestCommandCodecAndSchema();
        TestMaterialSchemaAndAssetValidation();
        TestNavigationAndDocumentBasics();
        TestSparseCandidateAndTextureValidation();
        TestConflictAndAtomicCandidateCommit();
        TestPreviewRequestAndResultQuery();
        TestEditorFacadeCommandFlow();
        std::cout << "Material instance editor contract tests passed." << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Material instance editor contract tests failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}
