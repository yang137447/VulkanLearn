#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "editor/service/sceneDocumentService.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "render/lighting/physicalLighting.h"
#include "render/camera/cameraExposure.h"
#include "world/world.h"
#include "sceneNode.h"
#include "script/scriptBlueprintAsset.h"

namespace
{
using namespace VL;
using namespace VL::Editor;
using namespace VL::EditorUi;
using Json = nlohmann::json;

class SceneDocumentTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-scene-save-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Maps");
        path = root / "Maps/SC_test.json";
        source = {
            {"name", "Scene"}, {"type", "scene"}, {"worldUnit", "cm"},
            {"description", "Disposable document persistence fixture"},
            {"opaque", {{"custom", 123}}},
            {"objects", Json::array({
                {{"name", "Camera"}, {"type", "camera"}, {"position", {0, 0, 300}},
                    {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60},
                    {"near_clip", 1}, {"far_clip", 10000}},
                {{"name", "Point"}, {"type", "pointLight"}, {"position", {0.1, 20, 30}},
                    {"rotation", {0, 0, 0}}, {"color", {1, 1, 1}}, {"intensity", 100},
                    {"intensityUnit", "lm"}, {"radius", 75}, {"custom", "preserved"}}
            })}
        };
        source["objects"][0]["exposure"] = DescribeCameraExposure(CameraExposure{});
        Write(source);
        service = std::make_unique<SceneDocumentService>(root);
        std::string error;
        ASSERT_TRUE(service->Open(path.string(), error)) << error;
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void Write(const Json& json) { std::ofstream file(path); file << json.dump(4); }
    Json Read() { std::ifstream file(path); return Json::parse(file); }
    EditorSceneActorSnapshot Point() const
    {
        EditorSceneActorSnapshot actor;
        actor.scenePath = path.string();
        actor.identity = "pointLight|Point";
        actor.type = EditorSceneActorType::PointLight;
        actor.position = Eigen::Vector3f(0.1f, 20, 30);
        actor.light.color = Eigen::Vector3f::Ones();
        actor.light.intensity = ReadCanonicalLightIntensity(source["objects"][1]);
        return actor;
    }
    EditorSceneActorSnapshot CameraBlueprint() const
    {
        EditorSceneActorSnapshot actor;
        actor.scenePath = path.string();
        actor.identity = "camera|Camera";
        actor.type = EditorSceneActorType::Camera;
        actor.camera.exposure = ReadCameraExposure(source["objects"][0]);
        return actor;
    }
    void Record(EditorSceneActorSnapshot actor, EditorActorProperty property, float value,
        uint32_t component = 0)
    {
        std::string error;
        ASSERT_TRUE(service->PrepareActorEdit(actor.scenePath, actor.identity, error)) << error;
        service->RecordActorEdit(actor, {1, actor.identity, property, component, value});
    }
    EditorCommandEnvelope SaveCommand() const
    {
        EditorCommandEnvelope command;
        command.commandId = 23;
        command.type = EditorCommandType::SaveSceneDocument;
        command.payload = MaterialInstanceAssetPathPayload{"Maps/SC_test.json"};
        const auto documents = service->ModifiedDocuments();
        command.expectedDocumentRevision = documents.empty() ? 1 : documents.front().revision;
        return command;
    }
    std::filesystem::path root, path;
    Json source;
    std::unique_ptr<SceneDocumentService> service;
};

TEST_F(SceneDocumentTest, PositionSavePreservesUntouchedFieldsAndOriginalNumericPrecision)
{
    auto actor = Point();
    actor.position.y() = 42;
    Record(actor, EditorActorProperty::Position, 42, 1);
    ASSERT_EQ(service->ModifiedDocuments().size(), 1u);
    EXPECT_EQ(Read(), source);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    Json expected = source;
    expected["objects"][1]["position"][1] = 42;
    EXPECT_EQ(Read(), expected);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, ScriptBlueprintCreateAndPropertySaveRetainPersistentIdentity)
{
    Json actor{{"type", "blueprintInstance"}, {"id", "persistent-a"}, {"name", "Rotator"},
        {"class", "Gameplay.RotatorBlueprint"}, {"properties", {{"DegreesPerSecond", 45}}},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"components", Json::array()}};
    std::string error;
    ASSERT_TRUE(service->StageScriptBlueprint(path.string(), actor, true, error)) << error;
    EXPECT_EQ(Read(), source);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    EXPECT_EQ(Read()["objects"][2], actor);
    actor["properties"]["DegreesPerSecond"] = 90;
    ASSERT_TRUE(service->StageScriptBlueprint(path.string(), actor, false, error)) << error;
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    SceneDocumentService reopened(root);
    ASSERT_TRUE(reopened.Open(path.string(), error)) << error;
    EXPECT_EQ(Read()["objects"][2]["id"], "persistent-a");
    EXPECT_EQ(Read()["objects"][2]["properties"]["DegreesPerSecond"], 90);
    EXPECT_EQ(Read()["opaque"], source["opaque"]);
}

TEST_F(SceneDocumentTest, InvalidScriptBlueprintDoesNotMutateDraftOrDisk)
{
    Json actor{{"type", "blueprintInstance"}, {"id", "bad"}, {"name", "Bad"},
        {"class", "Gameplay.RotatorBlueprint"}, {"properties", Json::object()},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"components", Json::array({{{"type", "terrain"}}})}};
    std::string error;
    EXPECT_FALSE(service->StageScriptBlueprint(path.string(), actor, true, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    EXPECT_EQ(Read(), source);
}

TEST_F(SceneDocumentTest, BlueprintAssetInstanceSaveRetainsSparseOverridesAndReference)
{
    Json actor{{"type", "blueprintInstance"}, {"id", "asset-instance"}, {"name", "Rotator"},
        {"class", "Gameplay.RotatorBlueprint"}, {"properties", {{"DegreesPerSecond", 45}}},
        {"instanceEditable", {"DegreesPerSecond"}},
        {"position", {10, 20, 30}}, {"rotation", {0, 10, 0}}, {"scale", {2, 2, 2}}};
    std::string error;
    const std::string assetPath = "Common/Blueprints/BP_rotator.json";
    ASSERT_TRUE(service->CreateScriptBlueprintAsset(assetPath, actor, error)) << error;
    actor.erase("class");
    actor.erase("instanceEditable");
    actor["blueprintPath"] = assetPath;
    actor["properties"] = Json::object();
    actor["unmanaged"] = "retained";
    ASSERT_TRUE(service->StageScriptBlueprint(path.string(), actor, true, error)) << error;
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    service->NotifyWorldLoaded(path.string());
    actor = service->ScriptBlueprintDraft(path.string(), "blueprintInstance|asset-instance", error);
    ASSERT_FALSE(actor.is_null()) << error;
    EXPECT_EQ(service->ResolveBlueprint(actor, path.string())["properties"]["DegreesPerSecond"], 45);
    actor["properties"]["DegreesPerSecond"] = 90;
    ASSERT_TRUE(service->StageScriptBlueprint(path.string(), actor, false, error)) << error;
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    service->NotifyWorldLoaded(path.string());
    actor = service->ScriptBlueprintDraft(path.string(), "blueprintInstance|asset-instance", error);
    actor["properties"].erase("DegreesPerSecond");
    ASSERT_TRUE(service->StageScriptBlueprint(path.string(), actor, false, error)) << error;
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    SceneDocumentService reopened(root);
    ASSERT_TRUE(reopened.Open(path.string(), error));
    actor = reopened.ScriptBlueprintDraft(path.string(), "blueprintInstance|asset-instance", error);
    EXPECT_EQ(actor["blueprintPath"], assetPath);
    EXPECT_TRUE(actor["properties"].empty());
    EXPECT_FALSE(actor.contains("class"));
    EXPECT_FALSE(actor.contains("components"));
    EXPECT_EQ(actor["position"], Json::array({10, 20, 30}));
    EXPECT_EQ(actor["unmanaged"], "retained");
    EXPECT_EQ(reopened.ResolveBlueprint(actor, path.string())["properties"]["DegreesPerSecond"], 45);
}

TEST_F(SceneDocumentTest, BlueprintAssetCreationNeverOverwritesExistingFile)
{
    Json actor{{"name", "Rotator"}, {"class", "Gameplay.RotatorBlueprint"}, {"properties", Json::object()},
        {"rotation", {0, 0, 0}}, {"scale", {3, 3, 3}}};
    std::string error;
    ASSERT_TRUE(service->CreateScriptBlueprintAsset("Common/Blueprints/BP_rotator.json", actor, error)) << error;
    actor["name"] = "Replacement";
    EXPECT_FALSE(service->CreateScriptBlueprintAsset("Common/Blueprints/BP_rotator.json", actor, error));
    const auto asset = ReadScriptBlueprintAsset(root, "Common/Blueprints/BP_rotator.json");
    EXPECT_EQ(asset["name"], "Rotator");
    EXPECT_EQ(asset["scale"], Json::array({3, 3, 3}));
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    for (const auto& entry : std::filesystem::directory_iterator(root / "Common/Blueprints"))
        EXPECT_EQ(entry.path().filename(), "BP_rotator.json");
}

TEST_F(SceneDocumentTest, BlueprintClassCreatesMultipleSparseAssetsWithoutSourcesOrSceneChanges)
{
    const Json catalog = Json::array({{{"name", "MyGame.DoorBlueprint"},
        {"properties", Json::array()}}});
    std::string error;
    for (const std::string destination : {"Common/Blueprints/Gameplay/BP_fresh.json",
        "Maps/SC_test/Blueprints/Interaction/BP_fresh.json"})
    {
        ASSERT_TRUE(service->CreateScriptBlueprintFromClass(destination, "MyGame.DoorBlueprint",
            "Fresh", catalog, error)) << error;
        const auto asset = ReadScriptBlueprintAsset(root, destination);
        EXPECT_EQ(asset["class"], "MyGame.DoorBlueprint");
        EXPECT_EQ(asset["name"], "Fresh");
        EXPECT_TRUE(asset["properties"].empty());
        EXPECT_TRUE(asset["instanceEditable"].empty());
        EXPECT_TRUE(asset["components"].empty());
        auto sourcePath = root / destination;
        sourcePath.replace_extension(".blueprint.cs");
        EXPECT_FALSE(std::filesystem::exists(sourcePath));
        for (const auto& entry : std::filesystem::directory_iterator(sourcePath.parent_path()))
            EXPECT_EQ(entry.path().extension(), ".json");
    }
    EXPECT_EQ(Read(), source);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, BlueprintClassCreationRejectsUnknownOrRemovedTypesBeforeWriting)
{
    std::string error;
    for (const std::string className : {"", "Game..Player", "Game.1Player", "Game.class",
        "Game.Player, Gameplay", "Game.Player<T>", "Game.Player\n{}", "VulkanLearn.Blueprint"})
    {
        EXPECT_FALSE(service->CreateScriptBlueprintFromClass("Common/Blueprints/BP_bad.json",
            className, "Bad", Json::array(), error)) << className;
        EXPECT_FALSE(error.empty());
    }
    EXPECT_FALSE(std::filesystem::exists(root / "Common"));
}

TEST_F(SceneDocumentTest, BlueprintClassCreationUsesTheExactRegisteredNamespace)
{
    const Json catalog = Json::array({{{"name", "Game.PlayerBlueprint"}},
        {{"name", "Game.CounterComponent"}}});
    std::string error;
    for (const char* name : {"OtherGame.PlayerBlueprint", "Game.UnknownBlueprint"})
        EXPECT_FALSE(service->CreateScriptBlueprintFromClass("Common/Blueprints/BP_player.json",
            name, "Player", catalog, error));
    EXPECT_FALSE(std::filesystem::exists(root / "Common"));
    const Json valid = Json::array({{{"name", "OtherGame.PlayerBlueprint"}, {"properties", Json::array()}}});
    EXPECT_TRUE(service->CreateScriptBlueprintFromClass("Common/Blueprints/BP_player.json",
        "OtherGame.PlayerBlueprint", "Player", valid, error)) << error;
}

TEST_F(SceneDocumentTest, BlueprintClassCreationDoesNotOwnOrOverwriteAnAdjacentSource)
{
    const auto sourcePath = root / "Common/Blueprints/BP_player.blueprint.cs";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::ofstream(sourcePath) << "existing-source";
    std::string error;
    EXPECT_TRUE(service->CreateScriptBlueprintFromClass("Common/Blueprints/BP_player.json",
        "Game.PlayerBlueprint", "Player", Json::array({{{"name", "Game.PlayerBlueprint"},
            {"properties", Json::array()}}}), error)) << error;
    std::ifstream file(sourcePath);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "existing-source");
    EXPECT_TRUE(std::filesystem::exists(root / "Common/Blueprints/BP_player.json"));
}

TEST_F(SceneDocumentTest, BlueprintClassCreationNeverOverwritesAnExistingAsset)
{
    const auto target = root / "Common/Blueprints/BP_player.json";
    std::filesystem::create_directories(target.parent_path());
    std::ofstream(target) << "existing-asset";
    std::string error;
    EXPECT_FALSE(service->CreateScriptBlueprintFromClass("Common/Blueprints/BP_player.json",
        "Game.PlayerBlueprint", "Player", Json::array({{{"name", "Game.PlayerBlueprint"},
            {"properties", Json::array()}}}), error));
    std::ifstream file(target);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "existing-asset");
    EXPECT_FALSE(std::filesystem::exists(root / "Common/Blueprints/BP_player.blueprint.cs"));
}

TEST_F(SceneDocumentTest, BlueprintClassCreationRejectsNonContentPathsWithoutWriting)
{
    std::string error;
    for (const char* destination : {"../BP_escape.json", "Generated/Blueprints/BP_generated.json",
        "Common/Scripts/BP_wrong.json", "Maps/Blueprints/BP_wrong.json",
        "Maps/SC_test/Blueprints/SM_wrong.json"})
        EXPECT_FALSE(service->CreateScriptBlueprintFromClass(destination, "Game.PlayerBlueprint",
            "Player", Json::array({{{"name", "Game.PlayerBlueprint"},
                {"properties", Json::array()}}}), error)) << destination;
    EXPECT_FALSE(std::filesystem::exists(root / "Common"));
    EXPECT_FALSE(std::filesystem::exists(root / "Generated"));
}

TEST_F(SceneDocumentTest, BlueprintClassCreationRequiresANonemptyDisplayName)
{
    std::string error;
    EXPECT_FALSE(service->CreateScriptBlueprintFromClass("Common/Blueprints/BP_empty.json",
        "Game.PlayerBlueprint", "", Json::array({{{"name", "Game.PlayerBlueprint"},
            {"properties", Json::array()}}}), error));
    EXPECT_FALSE(std::filesystem::exists(root / "Common"));
}

TEST_F(SceneDocumentTest, BlueprintCreationPublishesCanonicalTypeAndRetainsInstancePermissions)
{
    Json actor{{"name", "Rotator"}, {"class", "Gameplay.RotatorBlueprint"},
        {"properties", {{"DegreesPerSecond", 90}}}, {"__instanceEditable", {"DegreesPerSecond"}},
        {"rotation", {0, 30, 0}}, {"scale", {1, 1, 1}}};
    std::string error;
    for (const std::string destination : {"Common/Blueprints/BP_rotator.json",
        "Maps/SC_test/Blueprints/BP_rotator.json"})
    {
        ASSERT_TRUE(service->CreateScriptBlueprintAsset(destination, actor, error)) << error;
        const auto asset = ReadScriptBlueprintAsset(root, destination);
        EXPECT_EQ(asset["type"], "blueprint");
        EXPECT_EQ(asset["instanceEditable"], Json::array({"DegreesPerSecond"}));
        actor["name"] = "Replacement";
        EXPECT_FALSE(service->CreateScriptBlueprintAsset(destination, actor, error));
        EXPECT_EQ(ReadScriptBlueprintAsset(root, destination), asset);
    }
    for (const char* destination : {"Common/Blueprints/ACT_wrong.json", "Common/Actors/BP_wrong.json",
        "Generated/Blueprints/BP_wrong.json", "Maps/Blueprints/BP_wrong.json"})
        EXPECT_FALSE(service->CreateScriptBlueprintAsset(destination, actor, error)) << destination;
    EXPECT_TRUE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, BlueprintAssetCreationRejectsEscapesAndInvalidContentOwnership)
{
    Json actor{{"name", "Rotator"}, {"class", "Gameplay.RotatorBlueprint"}, {"properties", Json::object()},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}};
    std::string error;
    for (const char* destination : {"../BP_escape.json", "Generated/Blueprints/BP_generated.json",
        "Common/Blueprints/SM_wrong.json", "Maps/Blueprints/BP_wrong.json"})
        EXPECT_FALSE(service->CreateScriptBlueprintAsset(destination, actor, error)) << destination;
    std::filesystem::create_directories(root / "Maps/SC_test/Meshes");
    std::ofstream(root / "Maps/SC_test/Meshes/SM_model.json") << R"({"type":"mesh"})";
    actor["components"] = Json::array({{{"type", "mesh"}, {"modelPath", "Maps/SC_test/Meshes/SM_model.json"}}});
    EXPECT_FALSE(service->CreateScriptBlueprintAsset("Common/Blueprints/BP_rotator.json", actor, error));
    EXPECT_FALSE(std::filesystem::exists(root / "Common/Blueprints/BP_rotator.json"));
    ASSERT_TRUE(service->CreateScriptBlueprintAsset("Maps/SC_test/Blueprints/BP_rotator.json", actor, error)) << error;
}

TEST_F(SceneDocumentTest, ReturningToParsedBaselineClearsDirtyWithoutWriting)
{
    auto actor = Point();
    actor.position.x() = 10;
    Record(actor, EditorActorProperty::Position, 10);
    actor.position.x() = 0.1f;
    Record(actor, EditorActorProperty::Position, 0.1f);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    EXPECT_EQ(Read(), source);
}

TEST_F(SceneDocumentTest, ExposureSavePatchesOnlyEditedFieldsAndRetainsPhysicalParameters)
{
    source["objects"][0]["exposure"]["mode"] = "physical";
    source["objects"][0]["exposure"]["aperture"] = 2.8123456789;
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = CameraBlueprint();
    actor.camera.exposure.compensation = 2;
    Record(actor, EditorActorProperty::CameraExposureCompensation, 2);
    EXPECT_EQ(Read(), source);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    auto expected = source;
    expected["objects"][0]["exposure"]["compensation"] = 2;
    EXPECT_EQ(Read(), expected);
}

TEST_F(SceneDocumentTest, ExposureModeAndValuesReplayIntoReplacementAuthoredCamera)
{
    auto actor = CameraBlueprint();
    actor.camera.exposure.mode = ExposureMode::Auto;
    Record(actor, EditorActorProperty::CameraExposureMode, 2);
    actor.camera.exposure.autoSpeedDown = 2;
    Record(actor, EditorActorProperty::CameraAutoSpeedDown, 2);
    World replacement(9, path.string());
    auto camera = std::make_shared<Camera>();
    camera->SetName("Camera");
    replacement.SetCamera(camera);
    replacement.SetEditorViewEnabled(true);
    std::string error;
    for (const auto& edit : service->ReplayEdits(path.string(), 9, error))
        ASSERT_TRUE(ApplyEditorActorProperty(replacement, edit, error)) << error;
    EXPECT_EQ(camera->GetExposure().mode, ExposureMode::Auto);
    EXPECT_DOUBLE_EQ(camera->GetExposure().autoSpeedDown, 2);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    EXPECT_EQ(Read()["objects"][0]["exposure"]["mode"], "auto");
}

TEST_F(SceneDocumentTest, ReturningToOmittedAutomaticDefaultClearsDirtyWithoutInsertingKeys)
{
    source["objects"][0]["exposure"].erase("autoTargetGray");
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = CameraBlueprint();
    actor.camera.exposure.autoTargetGray = 0.25;
    Record(actor, EditorActorProperty::CameraAutoTargetGray, 0.25f);
    ASSERT_FALSE(service->ModifiedDocuments().empty());
    actor.camera.exposure.autoTargetGray = 0.18f;
    Record(actor, EditorActorProperty::CameraAutoTargetGray, 0.18f);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    EXPECT_EQ(Read(), source);
}

TEST_F(SceneDocumentTest, LumensUnitIsPreservedWhenSavingCanonicalCandela)
{
    auto actor = Point();
    actor.light.intensity = 42;
    Record(actor, EditorActorProperty::Intensity, 42);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    auto light = Read()["objects"][1];
    EXPECT_EQ(light["intensityUnit"], "lm");
    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(light), 42);
}

TEST_F(SceneDocumentTest, SourceConflictKeepsDraftAndDoesNotOverwriteExternalChanges)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    auto external = source;
    external["description"] = "External change";
    Write(external);
    EXPECT_EQ(service->Save(SaveCommand()).errorCode, EditorErrorCode::SourceChanged);
    ASSERT_EQ(service->ModifiedDocuments().size(), 1u);
    EXPECT_EQ(service->ModifiedDocuments().front().status, EditorDocumentStatus::SourceChanged);
    EXPECT_EQ(Read(), external);
}

TEST_F(SceneDocumentTest, StaleConfirmationRejectsWithoutWritingOrClearingDirty)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    auto command = SaveCommand();
    actor.position.y() = 60;
    Record(actor, EditorActorProperty::Position, 60, 1);
    EXPECT_EQ(service->Save(command).errorCode, EditorErrorCode::StaleDocumentRevision);
    EXPECT_EQ(Read(), source);
    EXPECT_FALSE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, DraftReplaysWithNewGenerationAndSurvivesOpeningAnotherScene)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error));
    auto edits = service->ReplayEdits(path.string(), 9, error);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits.front().worldGeneration, 9u);
    EXPECT_EQ(edits.front().identity, "pointLight|Point");
    EXPECT_EQ(edits.front().value, 55);
}

TEST_F(SceneDocumentTest, ExternallyChangedBaselineIsNeverUsedForDraftReplay)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    auto external = source;
    external["objects"][1]["position"][0] = 200;
    Write(external);
    std::string error;
    EXPECT_TRUE(service->ReplayEdits(path.string(), 9, error).empty());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(service->ModifiedDocuments().empty());
    EXPECT_EQ(service->ModifiedDocuments().front().status, EditorDocumentStatus::SourceChanged);
}

TEST_F(SceneDocumentTest, ReplayOutsideRootReportsErrorWithoutThrowing)
{
    std::string error;
    EXPECT_TRUE(service->ReplayEdits("../SC_escape.json", 9, error).empty());
    EXPECT_FALSE(error.empty());
}

TEST_F(SceneDocumentTest, RetainedEditsApplyToTheReplacementWorldNotTheObserverCamera)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    World replacement(9, path.string());
    auto point = std::make_shared<PointLight>();
    point->SetPosition(Eigen::Vector3f(0.1f, 20, 30));
    replacement.AddPointLight("Point", point);
    replacement.SetCamera(std::make_shared<Camera>());
    replacement.SetEditorViewEnabled(true);
    const auto cameraPosition = replacement.GetRenderCamera()->GetPosition();
    std::string error;
    for (const auto& edit : service->ReplayEdits(path.string(), 9, error))
        ASSERT_TRUE(ApplyEditorActorProperty(replacement, edit, error)) << error;
    EXPECT_EQ(point->GetPosition(), Eigen::Vector3f(55, 20, 30));
    EXPECT_EQ(replacement.GetRenderCamera()->GetPosition(), cameraPosition);
    EXPECT_EQ(Read(), source);
}

TEST_F(SceneDocumentTest, SpotConeSavePreservesLumensUnitAndCanonicalRuntimeStrength)
{
    auto& light = source["objects"][1];
    light["type"] = "spotLight";
    light["cone_angle_inner"] = 15;
    light["cone_angle_outer"] = 30;
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = Point();
    actor.identity = "spotLight|Point";
    actor.type = EditorSceneActorType::SpotLight;
    actor.light.coneAngleInner = 15 * static_cast<float>(LightingPi / 180);
    actor.light.coneAngleOuter = 60 * static_cast<float>(LightingPi / 180);
    Record(actor, EditorActorProperty::OuterCone, 60);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto saved = Read()["objects"][1];
    EXPECT_EQ(saved["intensityUnit"], "lm");
    EXPECT_NEAR(saved["cone_angle_outer"].get<double>(), 60, 1.0e-5);
    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(saved), actor.light.intensity);
}

TEST_F(SceneDocumentTest, AreaSizeSavePreservesLumensUnitAndRadiance)
{
    auto& light = source["objects"][1];
    light["type"] = "squareAreaLight";
    light["size"] = 100;
    light["normal"] = {0, -1, 0};
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = Point();
    actor.identity = "squareAreaLight|Point";
    actor.type = EditorSceneActorType::SquareAreaLight;
    actor.light.squareSize = 200;
    Record(actor, EditorActorProperty::AreaSize, 200);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto saved = Read()["objects"][1];
    EXPECT_EQ(saved["intensityUnit"], "lm");
    EXPECT_EQ(saved["size"], 200);
    EXPECT_NEAR(saved["intensity"].get<double>(), 400, 1.0e-4);
    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(saved), actor.light.intensity);
}

TEST_F(SceneDocumentTest, MeshTrsSavePreservesReferencesAndUnmanagedFields)
{
    source["objects"].push_back({{"type", "mesh"}, {"name", "Model"},
        {"modelPath", "Meshes/SM_model.json"}, {"position", {0, 0, 0}},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"custom", "retained"}});
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    EditorSceneActorSnapshot actor;
    actor.scenePath = path.string();
    actor.identity = "mesh|Model";
    actor.type = EditorSceneActorType::Mesh;
    actor.position = Eigen::Vector3f(100, 20, 30);
    actor.rotation = Eigen::Vector3f(0, 45, 0);
    actor.scale = Eigen::Vector3f(-2, 3, 4);
    Record(actor, EditorActorProperty::Position, 100);
    Record(actor, EditorActorProperty::Rotation, 45, 1);
    Record(actor, EditorActorProperty::Scale, -2);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto mesh = Read()["objects"][2];
    EXPECT_EQ(mesh["position"], Json::array({100, 20, 30}));
    EXPECT_EQ(mesh["rotation"], Json::array({0, 45, 0}));
    EXPECT_EQ(mesh["scale"], Json::array({-2, 3, 4}));
    EXPECT_EQ(mesh["modelPath"], "Meshes/SM_model.json");
    EXPECT_EQ(mesh["custom"], "retained");
}

TEST_F(SceneDocumentTest, CsmDefaultRevertClearsDirtyAndSaveRetainsOtherShadowSettings)
{
    auto& light = source["objects"][1];
    light["type"] = "directionalLight";
    light["intensityUnit"] = "lux";
    light["shadow"] = {{"distance", 5000}};
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = Point();
    actor.identity = "directionalLight|Point";
    actor.type = EditorSceneActorType::DirectionalLight;
    actor.castShadows = false;
    Record(actor, EditorActorProperty::CastShadows, 0);
    actor.castShadows = true;
    Record(actor, EditorActorProperty::CastShadows, 1);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    actor.castShadows = false;
    Record(actor, EditorActorProperty::CastShadows, 0);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto shadow = Read()["objects"][1]["shadow"];
    EXPECT_EQ(shadow["castShadows"], false);
    EXPECT_EQ(shadow["distance"], 5000);
}

TEST_F(SceneDocumentTest, PreviewSceneEditsNeverBecomeSaveableDocuments)
{
    auto actor = Point();
    actor.scenePath = (root / "Maps/SC_model_preview.json").string();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, CameraTransformRemovesOverridingLookAtButKeepsExposure)
{
    source["objects"][0]["look_at"] = {0, 0, 0};
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    EditorSceneActorSnapshot actor;
    actor.scenePath = path.string();
    actor.identity = "camera|Camera";
    actor.type = EditorSceneActorType::Camera;
    actor.position = Eigen::Vector3f(10, 0, 300);
    actor.rotation = Eigen::Vector3f(0, 35, 0);
    Record(actor, EditorActorProperty::Position, 10);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto camera = Read()["objects"][0];
    EXPECT_FALSE(camera.contains("look_at"));
    EXPECT_EQ(camera["rotation"], Json::array({0, 35, 0}));
    EXPECT_EQ(camera["fov"], 60);
    EXPECT_EQ(camera["exposure"], source["objects"][0]["exposure"]);
}

TEST_F(SceneDocumentTest, PathEscapeAndAmbiguousActorAreRejected)
{
    std::string error;
    EXPECT_FALSE(service->Open("../SC_escape.json", error));
    source["objects"].push_back(source["objects"][1]);
    Write(source);
    ASSERT_TRUE(service->Open(path.string(), error));
    EXPECT_FALSE(service->PrepareActorEdit(path.string(), "pointLight|Point", error));
}

TEST_F(SceneDocumentTest, SceneSaveProtocolRoundTripsAndRequiresRevision)
{
    auto command = SaveCommand();
    const auto wire = EditorCommandJsonCodec::Encode(command);
    EXPECT_EQ(wire["command"], "scene.save");
    const auto decoded = EditorCommandJsonCodec::DecodeCommand(wire);
    EXPECT_EQ(decoded.type, EditorCommandType::SaveSceneDocument);
    EXPECT_EQ(decoded.expectedDocumentRevision, command.expectedDocumentRevision);
    EXPECT_FALSE(ValidateEditorCommandEnvelope(command).has_value());
    command.expectedDocumentRevision.reset();
    EXPECT_EQ(ValidateEditorCommandEnvelope(command), EditorErrorCode::MissingExpectedDocumentRevision);
}
} // namespace
