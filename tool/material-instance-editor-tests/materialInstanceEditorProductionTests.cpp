#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/editorCommand.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/service/materialInstanceDocumentService.h"
#include "support/fakeMaterialInstancePreviewAdapter.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;
using namespace VL::Editor;

#ifndef VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR
#define VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR "."
#endif

const std::filesystem::path kFixtureRoot =
    std::filesystem::path(VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR);
const std::string kMaterialInstancePath = "materials/MI_editor_test.json";

void Require(
    bool condition,
    std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void CopyFixtureFile(
    std::string_view relativePath,
    const std::filesystem::path& destination)
{
    const std::filesystem::path source = kFixtureRoot / relativePath;
    Require(
        std::filesystem::is_regular_file(source),
        "missing production test fixture: " + source.string());
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    Require(!error, "failed to create production fixture directory");
    std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    Require(!error, "failed to copy production test fixture: " + source.string());
}

void CopyFixtureDirectory(
    std::string_view relativePath,
    const std::filesystem::path& destination)
{
    const std::filesystem::path source = kFixtureRoot / relativePath;
    Require(
        std::filesystem::is_directory(source),
        "missing production test fixture directory: " + source.string());
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    Require(!error, "failed to create production fixture parent directory");
    std::filesystem::copy(
        source,
        destination,
        std::filesystem::copy_options::recursive,
        error);
    Require(!error, "failed to copy production test fixture directory");
}

void WriteJson(
    const std::filesystem::path& path,
    const Json& value)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    Require(!error, "failed to create generated fixture directory");
    std::ofstream output(path);
    Require(output.is_open(), "failed to write generated fixture");
    output << value.dump(2) << '\n';
    Require(output.good(), "failed to flush generated fixture");
}

Json ReadJson(
    const std::filesystem::path& path)
{
    std::ifstream input(path);
    Require(input.is_open(), "failed to read production test fixture");
    Json value;
    input >> value;
    return value;
}

class ProductionFixture
{
public:
    ProductionFixture()
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root = std::filesystem::temp_directory_path() /
            ("vulkanlearn_material_editor_production_" +
             std::to_string(stamp));
        resourceRoot = root / "resources";
        projectRoot = root / "project";

        CopyFixtureFile(
            "MI_editor_test_original.json",
            resourceRoot / kMaterialInstancePath);
        CopyFixtureFile(
            "M_editor_test.json",
            projectRoot / "shader/glsl/M_editor_test.json");
        CopyFixtureDirectory("textures", resourceRoot / "textures");

        WriteJson(
            resourceRoot / "scenes/scene_editor.json",
            Json{
                {"objects",
                 Json::array({
                     Json{
                         {"id", "mesh-car"},
                         {"type", "mesh"},
                         {"modelPath", "models/SM_editor_car.json"}},
                     Json{
                         {"id", "terrain-ground"},
                         {"type", "terrain"},
                         {"terrainPath", "terrains/TR_editor_ground.json"}}})}});
        WriteJson(
            resourceRoot / "models/SM_editor_car.json",
            Json{
                {"materialSlots",
                 Json::array({
                     Json{
                         {"slot", "body"},
                         {"materialInstancePath", kMaterialInstancePath}},
                     Json{
                         {"slot", "detail"},
                         {"materialInstancePath", kMaterialInstancePath}}})}});
        WriteJson(
            resourceRoot / "terrains/TR_editor_ground.json",
            Json{
                {"materialSlots",
                 Json::array({Json{
                     {"slot", "surface"},
                     {"materialInstancePath", kMaterialInstancePath}}})}});
    }

    ~ProductionFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    ProductionFixture(const ProductionFixture&) = delete;
    ProductionFixture& operator=(const ProductionFixture&) = delete;

    std::filesystem::path root;
    std::filesystem::path resourceRoot;
    std::filesystem::path projectRoot;
};

const MaterialEditorParameterSnapshot* FindParameter(
    const MaterialEditorDocumentSnapshot& document,
    std::string_view name)
{
    const auto iterator = std::find_if(
        document.parameters.begin(),
        document.parameters.end(),
        [name](const MaterialEditorParameterSnapshot& parameter)
        {
            return parameter.name == name;
        });
    return iterator == document.parameters.end() ? nullptr : &*iterator;
}

const MaterialEditorTextureBindingSnapshot* FindTexture(
    const MaterialEditorDocumentSnapshot& document,
    std::string_view slotName)
{
    const auto iterator = std::find_if(
        document.textures.begin(),
        document.textures.end(),
        [slotName](const MaterialEditorTextureBindingSnapshot& texture)
        {
            return texture.slotName == slotName;
        });
    return iterator == document.textures.end() ? nullptr : &*iterator;
}

const MaterialEditorRenderStateSnapshot* FindRenderState(
    const MaterialEditorDocumentSnapshot& document,
    std::string_view name)
{
    const auto iterator = std::find_if(
        document.renderStates.begin(),
        document.renderStates.end(),
        [name](const MaterialEditorRenderStateSnapshot& state)
        {
            return state.name == name;
        });
    return iterator == document.renderStates.end() ? nullptr : &*iterator;
}

void RequireRenderState(
    const MaterialEditorDocumentSnapshot& document,
    std::string_view name,
    std::string_view defaultValue,
    std::string_view effectiveValue,
    std::optional<std::string_view> overrideValue)
{
    const MaterialEditorRenderStateSnapshot* state =
        FindRenderState(document, name);
    Require(state != nullptr, "production service omitted a render state snapshot");
    Require(
        state->defaultValue == defaultValue &&
            state->effectiveValue == effectiveValue &&
            state->overrideValue.has_value() == overrideValue.has_value(),
        "production service returned an unexpected render state snapshot");
    if (overrideValue.has_value())
    {
        Require(
            state->overrideValue.has_value() &&
                state->overrideValue.value() == overrideValue.value(),
            "production service returned an unexpected render state override");
    }
}

const EditorUi::EditorParameterSnapshot* FindUiParameter(
    const EditorUi::MaterialInstanceDocumentSnapshot& document,
    std::string_view name)
{
    const auto iterator = std::find_if(
        document.parameters.begin(),
        document.parameters.end(),
        [name](const EditorUi::EditorParameterSnapshot& parameter)
        {
            return parameter.name == name;
        });
    return iterator == document.parameters.end() ? nullptr : &*iterator;
}

const EditorUi::MaterialInstanceDocumentSnapshot& RequireUiDocument(
    const MaterialInstanceEditorRuntime& runtime)
{
    Require(
        runtime.GetSnapshot().activeDocument.has_value(),
        "production runtime did not publish an active document");
    return runtime.GetSnapshot().activeDocument.value();
}

void SubmitRuntimeCommand(
    MaterialInstanceEditorRuntime& runtime,
    EditorCommandId commandId,
    EditorCommandType type,
    EditorCommandPayload payload,
    std::optional<EditorDocumentRevision> expectedRevision = std::nullopt)
{
    EditorCommandEnvelope command;
    command.commandId = commandId;
    command.source = EditorCommandSource::RuntimeTest;
    command.type = type;
    command.expectedDocumentRevision = expectedRevision;
    command.payload = std::move(payload);
    runtime.Submit(std::move(command));
    Require(
        runtime.PendingCommandCount() == 1,
        "production runtime did not queue command #" +
            std::to_string(commandId) + " (" +
            std::string(GetEditorCommandName(type)) + "): " +
            runtime.GetSnapshot().statusMessage);
    runtime.Tick();
    Require(
        runtime.PendingCommandCount() == 0,
        "production runtime left a command pending after Tick");
}

void ConnectRuntimePreview(
    MaterialInstanceEditorRuntime& runtime,
    EditorCommandId commandId,
    EditorDocumentRevision revision)
{
    SubmitRuntimeCommand(
        runtime,
        commandId,
        EditorCommandType::ConnectMaterialInstancePreview,
        EditorCommandPayload{MaterialInstanceAssetPathPayload{
            kMaterialInstancePath}},
        revision);
    Require(
        RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Connected,
        "production runtime did not reconnect the material instance preview");
}

void QueueRenderStatePreviewPrepareFailure(
    material_instance_editor_test::FakeMaterialInstancePreviewAdapter& adapter)
{
    VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
    result.status =
        VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
    result.failureStage =
        VL::Editor::Preview::PreviewAdapterFailureStage::Prepare;
    result.message =
        "Material instance render-state preview changes are unavailable: "
        "pipeline rebuild is required.";
    adapter.QueueExecuteResult(std::move(result));
}

void TestProductionRenderStatePreviewBoundary()
{
    ProductionFixture fixture;
    material_instance_editor_test::FakeMaterialInstancePreviewAdapter fakeAdapter;
    MaterialInstanceEditorRuntime runtime;
    const RuntimeResult<void> initialized = runtime.Initialize(
        MaterialInstanceEditorRuntime::Config{
            fixture.resourceRoot,
            fixture.projectRoot,
            64,
            &fakeAdapter});
    Require(
        initialized.IsSuccess(),
        "production runtime failed to initialize render-state preview coverage");

    runtime.NotifyWorldChanged("scenes/scene_editor.json", 1);
    OpenMaterialInstanceAssetPayload openPayload;
    openPayload.assetPath = kMaterialInstancePath;
    openPayload.origin = EditorNavigationOrigin{
        "scenes/scene_editor.json",
        "mesh-car",
        0,
        "body"};
    SubmitRuntimeCommand(
        runtime,
        12001,
        EditorCommandType::OpenMaterialInstanceAsset,
        EditorCommandPayload{std::move(openPayload)});
    struct RenderStatePreviewCase
    {
        EditorMaterialRenderStateField field;
        const char* fieldName;
        const char* valueName;
        EditorMaterialRenderStateValue value;
    };

    const std::array<RenderStatePreviewCase, 3> cases = {{
        {
            EditorMaterialRenderStateField::RenderMode,
            "renderMode",
            "TransparentAlphaBlend",
            EditorMaterialRenderMode::TransparentAlphaBlend},
        {
            EditorMaterialRenderStateField::ShadingModel,
            "shadingModel",
            "Unlit",
            EditorMaterialShadingModel::Unlit},
        {
            EditorMaterialRenderStateField::CullMode,
            "cullMode",
            "Front",
            EditorMaterialCullMode::Front}}};

    EditorCommandId nextCommandId = 12010;
    for (const RenderStatePreviewCase& testCase : cases)
    {
        ConnectRuntimePreview(
            runtime,
            nextCommandId++,
            RequireUiDocument(runtime).revision);
        const EditorDocumentRevision revisionBeforeEdit =
            RequireUiDocument(runtime).revision;
        ExecuteEditorCommandBatchPayload editBatch;
        editBatch.commands.push_back(EditorCommandBatchItem{
            EditorCommandType::SetMaterialRenderStateOverride,
            SetMaterialRenderStateOverridePayload{
                kMaterialInstancePath,
                testCase.field,
                testCase.value}});
        SubmitRuntimeCommand(
            runtime,
            nextCommandId++,
            EditorCommandType::ExecuteEditorCommandBatch,
            EditorCommandPayload{std::move(editBatch)},
            revisionBeforeEdit);
        const auto& edited = RequireUiDocument(runtime);
        const EditorDocumentRevision revisionAfterEdit = edited.revision;
        Require(
            edited.status == EditorUi::EditorDocumentStatus::Dirty &&
                revisionAfterEdit == revisionBeforeEdit + 1,
            "render-state edit did not produce a dirty runtime document: " +
                runtime.GetSnapshot().statusMessage);

        QueueRenderStatePreviewPrepareFailure(fakeAdapter);
        SubmitRuntimeCommand(
            runtime,
            nextCommandId++,
            EditorCommandType::ApplyMaterialInstancePreview,
            EditorCommandPayload{ApplyMaterialInstancePreviewPayload{
                kMaterialInstancePath,
                revisionAfterEdit}},
            revisionAfterEdit);
        const auto& rejected = RequireUiDocument(runtime);
        Require(
            rejected.preview == EditorUi::EditorPreviewStatus::Failed &&
                rejected.status == EditorUi::EditorDocumentStatus::Dirty &&
                rejected.previewMessage.find("pipeline rebuild") !=
                    std::string::npos,
            "render-state preview failure was not surfaced without discarding the draft");
        Require(
            !fakeAdapter.executeCommands.empty() &&
                fakeAdapter.executeCommands.back().type ==
                    VL::Editor::Preview::MaterialInstancePreviewCommandType::
                        ApplyMaterialInstancePreview &&
                fakeAdapter.executeCommands.back().draft.has_value(),
            "runtime did not send the render-state draft to the preview adapter");

        const Json sentDraft = Json::parse(
            fakeAdapter.executeCommands.back().draft->serializedWorkingDraft);
        Require(
            sentDraft.at("renderStateOverrides").at(testCase.fieldName) ==
                testCase.valueName,
            "runtime preview draft lost the edited render-state value");

        ConnectRuntimePreview(runtime, nextCommandId++, revisionAfterEdit);
        SubmitRuntimeCommand(
            runtime,
            nextCommandId++,
            EditorCommandType::RestoreMaterialInstancePreviewBaseline,
            EditorCommandPayload{MaterialInstanceAssetPathPayload{
                kMaterialInstancePath}},
            revisionAfterEdit);
        const auto& restored = RequireUiDocument(runtime);
        Require(
            restored.preview == EditorUi::EditorPreviewStatus::Connected &&
                restored.revision == revisionAfterEdit &&
                restored.status == EditorUi::EditorDocumentStatus::Dirty,
            "preview baseline restore changed the render-state asset draft");
        Require(
            !fakeAdapter.executeCommands.empty() &&
                fakeAdapter.executeCommands.back().type ==
                    VL::Editor::Preview::MaterialInstancePreviewCommandType::
                        RestoreMaterialInstancePreviewBaseline &&
                fakeAdapter.executeCommands.back().draft.has_value(),
            "runtime did not send a baseline draft for render-state restore");
        const Json baselineDraft = Json::parse(
            fakeAdapter.executeCommands.back().draft->serializedWorkingDraft);
        Require(
            baselineDraft.at("renderStateOverrides").at("cullMode") == "None" &&
                !baselineDraft.at("renderStateOverrides").contains("renderMode") &&
                !baselineDraft.at("renderStateOverrides").contains("shadingModel"),
            "render-state restore did not send the saved baseline overrides");

        SubmitRuntimeCommand(
            runtime,
            nextCommandId++,
            EditorCommandType::RevertMaterialInstanceDocument,
            EditorCommandPayload{MaterialInstanceAssetPathPayload{
                kMaterialInstancePath}},
            revisionAfterEdit);
        Require(
            RequireUiDocument(runtime).status ==
                EditorUi::EditorDocumentStatus::Clean,
            "render-state preview boundary test did not restore the document baseline");
    }

    runtime.Shutdown();
}

void RequireSucceeded(
    const MaterialEditorServiceResult& result,
    std::string_view context)
{
    Require(result.succeeded, std::string(context) + ": " + result.message);
}

void TestProductionDocumentService()
{
    ProductionFixture fixture;
    const Json originalJson = ReadJson(
        kFixtureRoot / "MI_editor_test_original.json");
    MaterialInstanceDocumentService service(
        MaterialInstanceDocumentService::Config{
            fixture.resourceRoot,
            fixture.projectRoot,
            true});

    const MaterialEditorServiceResult listed =
        service.ListMaterialInstanceAssets({}, 0, 50);
    RequireSucceeded(listed, "list material instance assets");
    Require(
        listed.editor.has_value() && listed.editor->assets.size() == 1 &&
            listed.editor->assets.front().assetPath == kMaterialInstancePath,
        "production service did not enumerate the fixture MI asset");

    const EditorNavigationOrigin origin{
        "scenes/scene_editor.json",
        "mesh-car",
        0,
        "body"};
    const MaterialEditorServiceResult opened =
        service.OpenMaterialInstanceAsset(kMaterialInstancePath, origin);
    RequireSucceeded(opened, "open material instance asset");
    Require(
        opened.document.has_value() &&
            opened.document->revision == 1 &&
            opened.document->state == MaterialEditorDocumentState::Clean &&
            opened.document->validation == MaterialEditorValidationState::Unknown,
        "production service returned an unexpected initial document state");
    Require(
        FindParameter(*opened.document, "u_scalar") != nullptr &&
            FindParameter(*opened.document, "u_tint") != nullptr &&
            FindTexture(*opened.document, "normalMap") != nullptr,
        "production service did not expose the material schema in its snapshot");
    Require(
        !opened.document->serializedBaselineDraft.empty() &&
            opened.document->serializedBaselineDraft ==
                opened.document->serializedWorkingDraft,
        "production service did not expose an equal initial baseline and working draft");
    const std::string initialBaselineDraft =
        opened.document->serializedBaselineDraft;
    const MaterialEditorServiceResult rejectedOutOfRange =
        service.SetMaterialParameterOverride(
            kMaterialInstancePath,
            "u_scalar",
            EditorMaterialParameterType::Float,
            EditorMaterialParameterValue{1.1f});
    Require(
        !rejectedOutOfRange.succeeded &&
            rejectedOutOfRange.errorCode == EditorErrorCode::ValidationFailed &&
            rejectedOutOfRange.document.has_value() &&
            rejectedOutOfRange.document->revision == opened.document->revision,
        "production service accepted an out-of-range scalar parameter");
    RequireRenderState(
        *opened.document,
        "renderMode",
        "Opaque",
        "Opaque",
        std::nullopt);
    RequireRenderState(
        *opened.document,
        "shadingModel",
        "DefaultLit",
        "DefaultLit",
        std::nullopt);
    RequireRenderState(
        *opened.document,
        "cullMode",
        "Back",
        "None",
        std::string_view("None"));

    const MaterialEditorServiceResult renderModeSet =
        service.SetMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::RenderMode,
            EditorMaterialRenderStateValue{
                EditorMaterialRenderMode::TransparentAlphaBlend});
    RequireSucceeded(renderModeSet, "set material renderMode override");
    Require(
        renderModeSet.document.has_value(),
        "renderMode set did not return a document snapshot");
    RequireRenderState(
        *renderModeSet.document,
        "renderMode",
        "Opaque",
        "TransparentAlphaBlend",
        std::string_view("TransparentAlphaBlend"));
    RequireRenderState(
        *renderModeSet.document,
        "shadingModel",
        "DefaultLit",
        "DefaultLit",
        std::nullopt);
    RequireRenderState(
        *renderModeSet.document,
        "cullMode",
        "Back",
        "None",
        std::string_view("None"));

    const MaterialEditorServiceResult renderModeCleared =
        service.ClearMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::RenderMode);
    RequireSucceeded(renderModeCleared, "clear material renderMode override");
    Require(
        renderModeCleared.document.has_value(),
        "renderMode clear did not return a document snapshot");
    RequireRenderState(
        *renderModeCleared.document,
        "renderMode",
        "Opaque",
        "Opaque",
        std::nullopt);
    RequireRenderState(
        *renderModeCleared.document,
        "cullMode",
        "Back",
        "None",
        std::string_view("None"));

    const MaterialEditorServiceResult shadingModelSet =
        service.SetMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::ShadingModel,
            EditorMaterialRenderStateValue{EditorMaterialShadingModel::Unlit});
    RequireSucceeded(shadingModelSet, "set material shadingModel override");
    Require(
        shadingModelSet.document.has_value(),
        "shadingModel set did not return a document snapshot");
    RequireRenderState(
        *shadingModelSet.document,
        "shadingModel",
        "DefaultLit",
        "Unlit",
        std::string_view("Unlit"));
    RequireRenderState(
        *shadingModelSet.document,
        "renderMode",
        "Opaque",
        "Opaque",
        std::nullopt);

    const MaterialEditorServiceResult shadingModelCleared =
        service.ClearMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::ShadingModel);
    RequireSucceeded(shadingModelCleared, "clear material shadingModel override");
    Require(
        shadingModelCleared.document.has_value(),
        "shadingModel clear did not return a document snapshot");
    RequireRenderState(
        *shadingModelCleared.document,
        "shadingModel",
        "DefaultLit",
        "DefaultLit",
        std::nullopt);

    const MaterialEditorServiceResult cullModeSet =
        service.SetMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::CullMode,
            EditorMaterialRenderStateValue{EditorMaterialCullMode::Front});
    RequireSucceeded(cullModeSet, "set material cullMode override");
    Require(
        cullModeSet.document.has_value(),
        "cullMode set did not return a document snapshot");
    RequireRenderState(
        *cullModeSet.document,
        "cullMode",
        "Back",
        "Front",
        std::string_view("Front"));

    const MaterialEditorServiceResult cullModeCleared =
        service.ClearMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::CullMode);
    RequireSucceeded(cullModeCleared, "clear material cullMode override");
    Require(
        cullModeCleared.document.has_value(),
        "cullMode clear did not return a document snapshot");
    RequireRenderState(
        *cullModeCleared.document,
        "cullMode",
        "Back",
        "Back",
        std::nullopt);

    RequireSucceeded(
        service.SetMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::RenderMode,
            EditorMaterialRenderStateValue{
                EditorMaterialRenderMode::TransparentAlphaBlend}),
        "prepare renderMode override for scoped reset");
    RequireSucceeded(
        service.SetMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::ShadingModel,
            EditorMaterialRenderStateValue{EditorMaterialShadingModel::Unlit}),
        "prepare shadingModel override for scoped reset");
    RequireSucceeded(
        service.SetMaterialRenderStateOverride(
            kMaterialInstancePath,
            EditorMaterialRenderStateField::CullMode,
            EditorMaterialRenderStateValue{EditorMaterialCullMode::Front}),
        "prepare cullMode override for scoped reset");

    const MaterialEditorServiceResult renderStatesReset =
        service.ResetMaterialInstanceOverrides(
            kMaterialInstancePath,
            EditorResetScope::RenderStates);
    RequireSucceeded(renderStatesReset, "reset material render state overrides");
    Require(
        renderStatesReset.document.has_value() &&
            renderStatesReset.document->state == MaterialEditorDocumentState::Dirty,
        "render state reset unexpectedly changed document cleanliness");
    RequireRenderState(
        *renderStatesReset.document,
        "renderMode",
        "Opaque",
        "Opaque",
        std::nullopt);
    RequireRenderState(
        *renderStatesReset.document,
        "shadingModel",
        "DefaultLit",
        "DefaultLit",
        std::nullopt);
    RequireRenderState(
        *renderStatesReset.document,
        "cullMode",
        "Back",
        "Back",
        std::nullopt);
    const MaterialEditorParameterSnapshot* resetScalar =
        FindParameter(*renderStatesReset.document, "u_scalar");
    const MaterialEditorTextureBindingSnapshot* resetAlbedo =
        FindTexture(*renderStatesReset.document, "albedoMap");
    Require(
        resetScalar != nullptr &&
            std::get<float>(resetScalar->effectiveValue) == 0.75f &&
            resetAlbedo != nullptr &&
            resetAlbedo->effectiveAssetPath ==
                "textures/T_albedo_override.json",
        "render state reset changed parameter or texture overrides");

    const MaterialEditorServiceResult renderStateReverted =
        service.RevertMaterialInstanceDocument(kMaterialInstancePath);
    RequireSucceeded(
        renderStateReverted,
        "restore baseline after render state service coverage");
    Require(
        renderStateReverted.document.has_value() &&
            renderStateReverted.document->serializedWorkingDraft ==
                initialBaselineDraft,
        "render state service coverage did not restore the original baseline");

    const MaterialEditorServiceResult resolved =
        service.ResolveSceneMaterialAsset(
            ResolveSceneMaterialAssetPayload{
                "scenes/scene_editor.json",
                "mesh-car",
                0});
    RequireSucceeded(resolved, "resolve scene material asset");
    Require(
        resolved.references.size() == 1 &&
            resolved.references.front().objectType == "mesh" &&
            resolved.references.front().sourceAssetPath ==
                "models/SM_editor_car.json" &&
            resolved.references.front().materialInstancePath ==
                kMaterialInstancePath,
        "production service resolved the wrong scene material reference");
    Require(
        resolved.document.has_value() &&
            resolved.document->references.size() == 2,
        "production service did not retain both navigation origins");

    const MaterialEditorDocumentSnapshot beforeInvalidTexture =
        service.BuildDocumentSnapshot(kMaterialInstancePath).value();
    const MaterialEditorServiceResult invalidTexture =
        service.SetMaterialTextureOverride(
            kMaterialInstancePath,
            "normalMap",
            "textures/T_missing_source.json");
    Require(
        !invalidTexture.succeeded &&
            invalidTexture.errorCode == EditorErrorCode::InvalidTextureAssetReference,
        "production service accepted a texture asset with a missing source");
    const MaterialEditorDocumentSnapshot afterInvalidTexture =
        service.BuildDocumentSnapshot(kMaterialInstancePath).value();
    Require(
        afterInvalidTexture.revision == beforeInvalidTexture.revision &&
            afterInvalidTexture.serializedWorkingDraft ==
                beforeInvalidTexture.serializedWorkingDraft,
        "invalid texture edit changed the document despite rejection");

    ExecuteEditorCommandBatchPayload invalidBatch;
    invalidBatch.commands = {
        EditorCommandBatchItem{
            EditorCommandType::SetMaterialParameterOverride,
            SetMaterialParameterOverridePayload{
                kMaterialInstancePath,
                "u_scalar",
                EditorMaterialParameterType::Float,
                0.9f}},
        EditorCommandBatchItem{
            EditorCommandType::SetMaterialParameterOverride,
            SetMaterialParameterOverridePayload{
                kMaterialInstancePath,
                "u_not_in_schema",
                EditorMaterialParameterType::Float,
                0.1f}}};
    const MaterialEditorServiceResult rejectedBatch = service.ExecuteBatch(
        invalidBatch,
        beforeInvalidTexture.revision);
    Require(
        !rejectedBatch.succeeded &&
            rejectedBatch.errorCode == EditorErrorCode::UnknownParameter,
        "production service accepted an invalid all-or-nothing batch");
    const MaterialEditorDocumentSnapshot afterRejectedBatch =
        service.BuildDocumentSnapshot(kMaterialInstancePath).value();
    Require(
        afterRejectedBatch.revision == beforeInvalidTexture.revision &&
            afterRejectedBatch.serializedWorkingDraft ==
                beforeInvalidTexture.serializedWorkingDraft,
        "rejected batch partially changed the working document");

    ExecuteEditorCommandBatchPayload validBatch;
    validBatch.commands = {
        EditorCommandBatchItem{
            EditorCommandType::SetMaterialParameterOverride,
            SetMaterialParameterOverridePayload{
                kMaterialInstancePath,
                "u_scalar",
                EditorMaterialParameterType::Float,
                0.9f}},
        EditorCommandBatchItem{
            EditorCommandType::SetMaterialTextureOverride,
            SetMaterialTextureOverridePayload{
                kMaterialInstancePath,
                "normalMap",
                "textures/T_normal_override.json"}}};
    const MaterialEditorServiceResult appliedBatch = service.ExecuteBatch(
        validBatch,
        beforeInvalidTexture.revision);
    RequireSucceeded(appliedBatch, "apply valid material editor batch");
    Require(
        appliedBatch.document.has_value() &&
            appliedBatch.document->revision == beforeInvalidTexture.revision + 1 &&
            appliedBatch.document->state == MaterialEditorDocumentState::Dirty,
        "production service did not commit a valid batch as one revision");
    const auto* scalar = FindParameter(*appliedBatch.document, "u_scalar");
    const auto* normal = FindTexture(*appliedBatch.document, "normalMap");
    Require(
        scalar != nullptr && std::get<float>(scalar->effectiveValue) == 0.9f &&
            normal != nullptr &&
            normal->effectiveAssetPath == "textures/T_normal_override.json",
        "production service did not expose the valid batch values");
    Require(
        appliedBatch.document->serializedBaselineDraft == initialBaselineDraft &&
            appliedBatch.document->serializedWorkingDraft !=
                appliedBatch.document->serializedBaselineDraft,
        "production service lost the saved baseline while building a dirty draft");

    const MaterialEditorServiceResult reverted =
        service.RevertMaterialInstanceDocument(kMaterialInstancePath);
    RequireSucceeded(reverted, "revert material instance document to baseline");
    Require(
        reverted.document.has_value() &&
            reverted.document->state == MaterialEditorDocumentState::Clean &&
            reverted.document->serializedBaselineDraft == initialBaselineDraft &&
            reverted.document->serializedWorkingDraft == initialBaselineDraft,
        "production service did not restore the immutable baseline draft");

    const MaterialEditorServiceResult reappliedBatch = service.ExecuteBatch(
        validBatch,
        reverted.document->revision);
    RequireSucceeded(reappliedBatch, "reapply valid material editor batch");

    const MaterialEditorServiceResult saved =
        service.SaveMaterialInstanceDocument(kMaterialInstancePath);
    RequireSucceeded(saved, "save material instance document");
    Require(
        saved.document.has_value() &&
            saved.document->state == MaterialEditorDocumentState::Clean &&
            saved.document->validation == MaterialEditorValidationState::Valid,
        "production service did not return a clean saved document");
    Require(
        saved.document->serializedWorkingDraft ==
                saved.document->serializedBaselineDraft &&
            saved.document->serializedBaselineDraft != initialBaselineDraft,
        "production service did not advance the baseline after save");

    std::ifstream savedInput(fixture.resourceRoot / kMaterialInstancePath);
    Require(savedInput.is_open(), "saved production fixture could not be reopened");
    Json savedJson;
    savedInput >> savedJson;
    Require(
        savedJson.at("parameters").at("u_scalar").get<float>() == 0.9f,
        "production service save did not persist the parameter override");
    Require(
        savedJson.at("textures").at("normalMap").get<std::string>() ==
            "textures/T_normal_override.json",
        "production service save did not persist the texture override");
    Require(
        savedJson.at("configHelp") == originalJson.at("configHelp"),
        "production service save did not preserve configHelp");
    Require(
        savedJson.at("renderStateOverrides") ==
            originalJson.at("renderStateOverrides"),
        "production service save did not preserve renderStateOverrides");
    Require(
        savedJson.at("macros") == originalJson.at("macros"),
        "production service save did not preserve macros");
}

void TestProductionRuntime()
{
    ProductionFixture fixture;

    MaterialInstanceEditorRuntime unavailableRuntime;
    const RuntimeResult<void> unavailableInitialized =
        unavailableRuntime.Initialize(
            MaterialInstanceEditorRuntime::Config{
                fixture.resourceRoot,
                fixture.projectRoot,
                64});
    Require(
        unavailableInitialized.IsSuccess(),
        "production runtime failed to initialize its unavailable-preview baseline");
    unavailableRuntime.NotifyWorldChanged("scenes/scene_unavailable.json", 1);
    OpenMaterialInstanceAssetPayload unavailableOpenPayload;
    unavailableOpenPayload.assetPath = kMaterialInstancePath;
    unavailableOpenPayload.origin = EditorNavigationOrigin{
        "scenes/scene_editor.json",
        "mesh-car",
        0,
        "body"};
    SubmitRuntimeCommand(
        unavailableRuntime,
        9001,
        EditorCommandType::OpenMaterialInstanceAsset,
        EditorCommandPayload{std::move(unavailableOpenPayload)});
    EditorCommandEnvelope unavailableConnectCommand;
    unavailableConnectCommand.commandId = 9002;
    unavailableConnectCommand.source = EditorCommandSource::RuntimeTest;
    unavailableConnectCommand.type = EditorCommandType::ConnectMaterialInstancePreview;
    unavailableConnectCommand.payload = MaterialInstanceAssetPathPayload{
        kMaterialInstancePath};
    unavailableRuntime.Submit(std::move(unavailableConnectCommand));
    unavailableRuntime.Tick();
    const auto& unavailableDocument = RequireUiDocument(unavailableRuntime);
    Require(
        unavailableDocument.preview == EditorUi::EditorPreviewStatus::Unavailable &&
            unavailableDocument.previewMessage.find("renderer owner") !=
                std::string::npos,
        "production runtime presented unavailable preview as connected");
    unavailableRuntime.Shutdown();

    material_instance_editor_test::FakeMaterialInstancePreviewAdapter fakeAdapter;
    MaterialInstanceEditorRuntime runtime;
    const RuntimeResult<void> initialized = runtime.Initialize(
        MaterialInstanceEditorRuntime::Config{
            fixture.resourceRoot,
            fixture.projectRoot,
            64,
            &fakeAdapter});
    Require(initialized.IsSuccess(), "production runtime failed to initialize");
    Require(runtime.IsInitialized(), "production runtime did not enter initialized state");
    Require(
        runtime.GetSnapshot().assets.size() == 1 &&
            runtime.GetSnapshot().assets.front().assetPath == kMaterialInstancePath,
        "production runtime did not publish the initial asset list");

    OpenMaterialInstanceAssetPayload openPayload;
    openPayload.assetPath = kMaterialInstancePath;
    openPayload.origin = EditorNavigationOrigin{
        "scenes/scene_editor.json",
        "mesh-car",
        0,
        "body"};
    SubmitRuntimeCommand(
        runtime,
        1001,
        EditorCommandType::OpenMaterialInstanceAsset,
        EditorCommandPayload{std::move(openPayload)});
    const auto& opened = RequireUiDocument(runtime);
    Require(
        opened.assetPath == kMaterialInstancePath &&
            opened.revision == 1 &&
            opened.status == EditorUi::EditorDocumentStatus::Clean &&
            opened.validation == EditorUi::EditorValidationStatus::Unknown,
        "production runtime published an unexpected opened document");
    Require(
        FindUiParameter(opened, "u_scalar") != nullptr,
        "production runtime did not publish the material parameter schema");
    const EditorDocumentRevision openedRevision = opened.revision;

    runtime.NotifyWorldChanged("scenes/scene_auto_preview.json", 1);
    runtime.Tick();
    Require(
        RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Connected,
        "production runtime did not connect preview automatically after the active World became available");

    ResolveSceneMaterialAssetPayload resolvePayload{
        "scenes/scene_editor.json",
        "mesh-car",
        0};
    SubmitRuntimeCommand(
        runtime,
        1002,
        EditorCommandType::ResolveSceneMaterialAsset,
        EditorCommandPayload{std::move(resolvePayload)});
    Require(
        runtime.GetSnapshot().statusMessage.find(
            "scene material reference resolved") != std::string::npos,
        "production runtime did not route scene reference resolution");
    Require(
        RequireUiDocument(runtime).revision == openedRevision,
        "scene reference resolution unexpectedly changed document revision");

    const EditorDocumentRevision revisionBeforeEdit =
        RequireUiDocument(runtime).revision;
    SetMaterialParameterOverridePayload editPayload{
        kMaterialInstancePath,
        "u_scalar",
        EditorMaterialParameterType::Float,
        0.65f};
    SubmitRuntimeCommand(
        runtime,
        1003,
        EditorCommandType::SetMaterialParameterOverride,
        EditorCommandPayload{std::move(editPayload)},
        revisionBeforeEdit);
    const auto& edited = RequireUiDocument(runtime);
    const auto* editedScalar = FindUiParameter(edited, "u_scalar");
    Require(
        edited.revision == revisionBeforeEdit + 1 &&
            edited.status == EditorUi::EditorDocumentStatus::Dirty &&
            editedScalar != nullptr &&
            editedScalar->effectiveValue.values[0] == 0.65f &&
            editedScalar->overrideValue.has_value(),
        "production runtime did not apply a typed parameter edit");

    const EditorDocumentRevision staleRevision = revisionBeforeEdit;
    const EditorDocumentRevision revisionAfterEdit = edited.revision;
    const float valueAfterEdit = editedScalar->effectiveValue.values[0];
    SetMaterialParameterOverridePayload stalePayload{
        kMaterialInstancePath,
        "u_scalar",
            EditorMaterialParameterType::Float,
        0.95f};
    EditorCommandEnvelope staleCommand;
    staleCommand.commandId = 1004;
    staleCommand.source = EditorCommandSource::RuntimeTest;
    staleCommand.type = EditorCommandType::SetMaterialParameterOverride;
    staleCommand.expectedDocumentRevision = staleRevision;
    staleCommand.payload = std::move(stalePayload);
    runtime.Submit(std::move(staleCommand));
    Require(
        runtime.PendingCommandCount() == 0,
        "production runtime queued a stale revision command");
    runtime.Tick();
    const auto& afterStale = RequireUiDocument(runtime);
    const auto* afterStaleScalar = FindUiParameter(afterStale, "u_scalar");
    Require(
        afterStale.revision == revisionAfterEdit &&
            afterStaleScalar != nullptr &&
            afterStaleScalar->effectiveValue.values[0] == valueAfterEdit &&
            runtime.GetSnapshot().statusMessage.find("revision") !=
                std::string::npos,
        "production runtime did not preserve the document after stale rejection");

    const EditorDocumentRevision revisionBeforeBatch = afterStale.revision;
    ExecuteEditorCommandBatchPayload batchPayload;
    batchPayload.commands = {
        EditorCommandBatchItem{
            EditorCommandType::SetMaterialParameterOverride,
            SetMaterialParameterOverridePayload{
                kMaterialInstancePath,
                "u_scalar",
                EditorMaterialParameterType::Float,
                0.9f}},
        EditorCommandBatchItem{
            EditorCommandType::SetMaterialTextureOverride,
            SetMaterialTextureOverridePayload{
                kMaterialInstancePath,
                "normalMap",
                "textures/T_normal_override.json"}}};
    SubmitRuntimeCommand(
        runtime,
        1005,
        EditorCommandType::ExecuteEditorCommandBatch,
        EditorCommandPayload{std::move(batchPayload)},
        revisionBeforeBatch);
    const auto& batched = RequireUiDocument(runtime);
    const auto* batchedScalar = FindUiParameter(batched, "u_scalar");
    Require(
        batched.revision == revisionBeforeBatch + 1 &&
            batched.status == EditorUi::EditorDocumentStatus::Dirty &&
            batchedScalar != nullptr &&
            batchedScalar->effectiveValue.values[0] == 0.9f,
        "production runtime did not route the batch as one document change");

    const EditorDocumentRevision revisionBeforeSave = batched.revision;
    Require(
        runtime.SubmitSaveActiveDocument(),
        "production runtime did not admit the active-document save command");
    Require(
        runtime.PendingCommandCount() == 1,
        "production runtime save shortcut did not queue a command");
    runtime.Tick();
    Require(
        runtime.PendingCommandCount() == 0,
        "production runtime save command remained pending");
    const auto& saved = RequireUiDocument(runtime);
    Require(
        saved.revision == revisionBeforeSave + 1 &&
            saved.status == EditorUi::EditorDocumentStatus::Clean &&
            saved.validation == EditorUi::EditorValidationStatus::Valid,
        "production runtime did not complete the active-document save");
    const Json savedJson = ReadJson(fixture.resourceRoot / kMaterialInstancePath);
    Require(
        savedJson.at("parameters").at("u_scalar").get<float>() == 0.9f &&
            savedJson.at("textures").at("normalMap").get<std::string>() ==
                "textures/T_normal_override.json",
        "production runtime save did not persist the batch values");

    const EditorDocumentRevision revisionBeforeWorldChange = saved.revision;
    runtime.NotifyWorldChanged("scenes/scene_editor.json", 1);
    const auto& worldDocument = RequireUiDocument(runtime);
    Require(
        worldDocument.revision == revisionBeforeWorldChange &&
            worldDocument.preview == EditorUi::EditorPreviewStatus::Unavailable &&
            worldDocument.previewMessage.find(
                "Active World: scenes/scene_editor.json") != std::string::npos,
        "production runtime World notification disturbed the document or overlay");

    runtime.NotifyWorldChanged("scenes/scene_fake_preview.json", 2);
    const auto& switchedWorldDocument = RequireUiDocument(runtime);
    Require(
        switchedWorldDocument.revision == revisionBeforeWorldChange &&
            switchedWorldDocument.preview == EditorUi::EditorPreviewStatus::Unavailable &&
            switchedWorldDocument.previewMessage.find(
                "Active World: scenes/scene_fake_preview.json") != std::string::npos,
        "production runtime did not preserve the document across World change");

    runtime.Tick();
    Require(
        runtime.PendingCommandCount() == 0 &&
            RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Connected,
        "runtime did not reconnect preview automatically after a World change");
    Require(
        !fakeAdapter.executeCommands.empty() &&
            fakeAdapter.executeCommands.back().type ==
                VL::Editor::Preview::MaterialInstancePreviewCommandType::
                    ConnectMaterialInstancePreview &&
            fakeAdapter.executeCommands.back().bridgeLiveGeneration != 0,
        "runtime did not pass a live generation to the automatic preview connection");

    const auto countDisconnectCommands = [&fakeAdapter]() {
        return static_cast<std::size_t>(std::count_if(
            fakeAdapter.executeCommands.begin(),
            fakeAdapter.executeCommands.end(),
            [](const auto& command) {
                return command.type ==
                    VL::Editor::Preview::MaterialInstancePreviewCommandType::
                        DisconnectMaterialInstancePreview;
            }));
    };
    const std::size_t disconnectCountBeforeClose = countDisconnectCommands();
    SubmitRuntimeCommand(
        runtime,
        1010,
        EditorCommandType::CloseMaterialInstanceAsset,
        EditorCommandPayload{CloseMaterialInstanceAssetPayload{
            kMaterialInstancePath,
            EditorDirtyDocumentPolicy::DiscardChanges}});
    Require(
        !runtime.GetSnapshot().activeDocument.has_value() &&
            runtime.GetSnapshot().documentTabs.empty() &&
            countDisconnectCommands() > disconnectCountBeforeClose,
        "closing the last material instance tab did not release its preview connection");

    OpenMaterialInstanceAssetPayload reopenedPayload;
    reopenedPayload.assetPath = kMaterialInstancePath;
    SubmitRuntimeCommand(
        runtime,
        1014,
        EditorCommandType::OpenMaterialInstanceAsset,
        EditorCommandPayload{std::move(reopenedPayload)});
    Require(
        RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Connected,
        "reopening a material instance tab did not reconnect preview automatically");
    const EditorDocumentRevision revisionBeforeEditAfterReopen =
        RequireUiDocument(runtime).revision;

    SubmitRuntimeCommand(
        runtime,
        1011,
        EditorCommandType::SetMaterialParameterOverride,
        EditorCommandPayload{SetMaterialParameterOverridePayload{
            kMaterialInstancePath,
            "u_scalar",
            EditorMaterialParameterType::Float,
            0.85f}},
        revisionBeforeEditAfterReopen);
    Require(
        fakeAdapter.executeCommands.back().type ==
                VL::Editor::Preview::MaterialInstancePreviewCommandType::
                    ApplyMaterialInstancePreview &&
            fakeAdapter.executeCommands.back().draft.has_value() &&
            fakeAdapter.executeCommands.back().draft->documentRevision ==
                RequireUiDocument(runtime).revision &&
            fakeAdapter.executeCommands.back().draft->serializedWorkingDraft.find(
                "0.85") != std::string::npos,
        "runtime did not automatically apply the edited numeric draft to preview");
    const EditorDocumentRevision revisionBeforeFakeApply =
        RequireUiDocument(runtime).revision;
    SubmitRuntimeCommand(
        runtime,
        1012,
        EditorCommandType::ApplyMaterialInstancePreview,
        EditorCommandPayload{ApplyMaterialInstancePreviewPayload{
            kMaterialInstancePath,
            revisionBeforeFakeApply}},
        revisionBeforeFakeApply);
    Require(
        runtime.PendingCommandCount() == 0 &&
            RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Connected,
        "runtime did not apply the numeric draft through the injected adapter");
    Require(
        fakeAdapter.executeCommands.back().type ==
                VL::Editor::Preview::MaterialInstancePreviewCommandType::
                    ApplyMaterialInstancePreview &&
            fakeAdapter.executeCommands.back().draft.has_value() &&
            fakeAdapter.executeCommands.back().draft->documentRevision ==
                revisionBeforeFakeApply &&
            fakeAdapter.executeCommands.back().draft->serializedWorkingDraft.find(
                "0.85") != std::string::npos &&
            fakeAdapter.executeCommands.back().draft->serializedWorkingDraft.find(
                "textures") != std::string::npos &&
            fakeAdapter.executeCommands.back().draft->serializedWorkingDraft.find(
                "textures/T_albedo_override.json") != std::string::npos,
        "runtime did not pass the complete numeric working draft to the adapter");

    const EditorDocumentRevision revisionBeforeFakeRestore =
        RequireUiDocument(runtime).revision;
    SubmitRuntimeCommand(
        runtime,
        1013,
        EditorCommandType::RestoreMaterialInstancePreviewBaseline,
        EditorCommandPayload{MaterialInstanceAssetPathPayload{
            kMaterialInstancePath}},
        revisionBeforeFakeRestore);
    Require(
        runtime.PendingCommandCount() == 0 &&
            RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Connected &&
            fakeAdapter.executeCommands.back().type ==
                VL::Editor::Preview::MaterialInstancePreviewCommandType::
                    RestoreMaterialInstancePreviewBaseline,
        "runtime did not restore the baseline draft through the injected adapter");
    Require(
        fakeAdapter.executeCommands.back().draft.has_value() &&
            !fakeAdapter.executeCommands.back().draft->serializedWorkingDraft.empty(),
        "runtime restore did not pass a serialized baseline draft to the adapter");

    fakeAdapter.QueueExecuteResult(VL::Editor::Preview::MaterialInstancePreviewAdapterResult{
        VL::Editor::Preview::PreviewAdapterOperationStatus::Pending,
        1501,
        0,
        false,
        false,
        VL::Editor::Preview::PreviewAdapterFailureStage::None,
        "fake apply pending"});
    fakeAdapter.QueuePollResult(VL::Editor::Preview::MaterialInstancePreviewAdapterResult{
        VL::Editor::Preview::PreviewAdapterOperationStatus::Pending,
        1501,
        0,
        false,
        false,
        VL::Editor::Preview::PreviewAdapterFailureStage::None,
        "fake apply still pending"});
    SubmitRuntimeCommand(
        runtime,
        1015,
        EditorCommandType::ApplyMaterialInstancePreview,
        EditorCommandPayload{ApplyMaterialInstancePreviewPayload{
            kMaterialInstancePath,
            revisionBeforeFakeRestore}},
        revisionBeforeFakeRestore);
    Require(
        !fakeAdapter.pollOperationIds.empty() &&
            fakeAdapter.pollOperationIds.back() == 1501,
        "runtime did not poll the pending preview operation for generation testing");
    const EditorDocumentRevision revisionBeforeGeneration =
        RequireUiDocument(runtime).revision;
    runtime.NotifyWorldChanged("scenes/scene_generation_changed.json", 4);
    Require(
        RequireUiDocument(runtime).revision == revisionBeforeGeneration &&
            RequireUiDocument(runtime).preview == EditorUi::EditorPreviewStatus::Unavailable &&
            !fakeAdapter.canceledOperationIds.empty() &&
            fakeAdapter.canceledOperationIds.back() == 1501,
        "runtime did not invalidate and cancel a pending preview across World generation");

    runtime.Shutdown();
    Require(!runtime.IsInitialized(), "production runtime did not shut down cleanly");
}

} // namespace

TEST(MaterialInstanceEditorProduction, RenderStatePreviewBoundary)
{
    TestProductionRenderStatePreviewBoundary();
}

TEST(MaterialInstanceEditorProduction, DocumentService)
{
    TestProductionDocumentService();
}

TEST(MaterialInstanceEditorProduction, Runtime)
{
    TestProductionRuntime();
}
