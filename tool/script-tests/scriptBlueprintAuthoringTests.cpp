#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "editor/scene/scriptBlueprintAuthoring.h"
#include "editor/scene/scriptBlueprintConfiguration.h"
#include "editor/service/sceneDocumentService.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptRuntime.h"
#include "ui/uiAction.h"
#include "world/world.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

TEST(ScriptPhysicsAuthoring, CompoundSelectionCreatesColliderInSameCandidateAndRemovalCleansIt)
{
    Json blueprint{{"components", Json::array({{{"id", "pivot"}, {"type", "scene"}}})}};
    Editor::EditScriptPhysics(blueprint, {{"field", "shape"}, {"value", "compound"}});
    EXPECT_EQ(blueprint["components"][0]["collision"]["shape"], "box");
    Editor::EditScriptComponentCollision(blueprint["components"][0],
        {{"field", "collisionShape"}, {"value", "sphere"}});
    Editor::EditScriptComponentCollision(blueprint["components"][0],
        {{"field", "collisionRadius"}, {"value", 42}});
    EXPECT_EQ(blueprint["components"][0]["collision"]["radius"], 42);
    Editor::EditScriptPhysics(blueprint, {{"field", "motion"}, {"value", "none"}});
    EXPECT_FALSE(blueprint.contains("physics"));
    EXPECT_FALSE(blueprint["components"][0].contains("collision"));
}

TEST(ScriptPhysicsAuthoring, ConstraintAxisChangesAtomicallyAndLayerRemainsInteger)
{
    Json blueprint{{"components", Json::array()}};
    Editor::EditScriptPhysics(blueprint, {{"field", "collisionLayer"}, {"value", 3.0}});
    EXPECT_TRUE(blueprint["physics"]["collisionLayer"].is_number_unsigned());
    Editor::EditScriptPhysics(blueprint, {{"field", "constraint-add"}, {"value", "anchor"}});
    Editor::EditScriptPhysics(blueprint, {{"field", "constraint-type"}, {"index", 0}, {"value", "hinge"}});
    Editor::EditScriptPhysics(blueprint,
        {{"field", "constraint-axis"}, {"index", 0}, {"value", {0, 0, 1}}});
    EXPECT_EQ(blueprint["physics"]["constraints"][0]["axis"], Json::array({0, 0, 1}));
    EXPECT_NO_THROW(ValidateScriptPhysics(blueprint.at("physics")));
}

TEST(ScriptPhysicsAuthoring, PrimitiveAndDisabledPhysicsPreserveAbsentComponentArray)
{
    Json blueprint = Json::object();
    Editor::EditScriptPhysics(blueprint, {{"field", "motion"}, {"value", "dynamic"}});
    EXPECT_FALSE(blueprint.contains("components"));
    EXPECT_NO_THROW(ValidateScriptPhysics(blueprint.at("physics")));
    Editor::EditScriptPhysics(blueprint, {{"field", "motion"}, {"value", "none"}});
    EXPECT_FALSE(blueprint.contains("components"));
    EXPECT_FALSE(blueprint.contains("physics"));
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
class ScriptBlueprintAuthoringTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-blueprint-authoring-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Maps/SC_test");
        scenePath = (root / "Maps/SC_test/SC_test.json").string();
        scene = {{"type", "scene"}, {"name", "Test"}, {"worldUnit", "cm"},
            {"description", "C# authoring integration fixture"}, {"objects", Json::array({
                {{"type", "camera"}, {"name", "Camera"}, {"position", {0, 100, 300}},
                    {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60},
                    {"near_clip", 1}, {"far_clip", 10000}, {"exposure", DescribeCameraExposure(CameraExposure{})}}
            })}};
        std::ofstream(scenePath) << scene.dump();
        documents = std::make_unique<Editor::SceneDocumentService>(root);
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(42, scenePath);
        runtime.SynchronizeWorld(world);
        Json templateBlueprint{{"name", "Rotator"}, {"class", "Gameplay.RotatorBlueprint"},
            {"instanceEditable", {"DegreesPerSecond"}},
            {"properties", {{"DegreesPerSecond", 90}}}, {"rotation", {0, 0, 0}}, {"scale", {2, 2, 2}}};
        std::string error;
        ASSERT_TRUE(documents->CreateScriptBlueprintAsset("Common/Blueprints/BP_rotator.json", templateBlueprint, error)) << error;
    }
    void TearDown() override
    {
        runtime.Shutdown();
        world.reset();
        documents.reset();
        std::filesystem::remove_all(root);
    }
    UiAction Add() const
    {
        UiAction action;
        action.type = UiActionType::AddScriptBlueprint;
        action.actorProperty.worldGeneration = 42;
        action.script = {{"blueprintPath", "Common/Blueprints/BP_rotator.json"}};
        return action;
    }
    void PublishDraft()
    {
        std::string error;
        scene = documents->DraftForWorldLoad(scenePath, error);
        ASSERT_FALSE(scene.is_null()) << error;
        runtime.PrepareWorld(*world, ResolveScriptBlueprintScene(root, scene, scenePath), scene);
        documents->NotifyWorldLoaded(scenePath);
    }
    std::filesystem::path root;
    std::string scenePath;
    Json scene;
    std::unique_ptr<Editor::SceneDocumentService> documents;
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
};

TEST_F(ScriptBlueprintAuthoringTest, VectorAxesMergeFromLatestDraftAndResetRemainsSparse)
{
    auto action = Add();
    action.script = {{"class", "ScriptFixtures.TypedPropertiesBlueprint"}, {"name", "Typed"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    action.type = UiActionType::SetScriptProperty;
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.script = {{"property", "Offset"}, {"axis", 0}, {"value", 10}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    action.script["axis"] = 2; action.script["value"] = 30;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    auto draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_EQ(draft["properties"]["Offset"], Json::array({10, 4, 30}));
    action.script = {{"property", "Offset"}, {"reset", true}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_TRUE(draft["properties"].empty());
}

TEST_F(ScriptBlueprintAuthoringTest, AssetPlacementCreatesDistinctPersistentInstancesWithSparseProperties)
{
    std::string first, second, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), first, error)) << error;
    PublishDraft();
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), second, error)) << error;
    PublishDraft();
    EXPECT_NE(first, second);
    ASSERT_EQ(world->GetScriptBlueprints().size(), 2u);
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["DegreesPerSecond"], 90);
    EXPECT_EQ(world->GetScriptBlueprints()[0]["scale"], Json::array({2, 2, 2}));
    for (const auto& source : world->GetScriptBlueprintSources())
    {
        EXPECT_EQ(source["blueprintPath"], "Common/Blueprints/BP_rotator.json");
        EXPECT_TRUE(source["properties"].empty());
        EXPECT_FALSE(source.contains("class"));
    }
}

TEST_F(ScriptBlueprintAuthoringTest, PropertyEditAndResetUseAuthorDraftRatherThanFilledRuntimeValues)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction edit;
    edit.type = UiActionType::SetScriptProperty;
    edit.actorProperty.worldGeneration = 42;
    edit.actorProperty.identity = "blueprintInstance|" + id;
    edit.script = {{"property", "DegreesPerSecond"}, {"value", 120}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["DegreesPerSecond"], 120);
    edit.script = {{"property", "DegreesPerSecond"}, {"reset", true}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    PublishDraft();
    EXPECT_TRUE(world->GetScriptBlueprintSources()[0]["properties"].empty());
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["DegreesPerSecond"], 90);
}

TEST_F(ScriptBlueprintAuthoringTest, NonExposedPropertyRequestCannotStageSceneChanges)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction edit;
    edit.type = UiActionType::SetScriptProperty;
    edit.actorProperty.worldGeneration = 42;
    edit.actorProperty.identity = "blueprintInstance|" + id;
    edit.script = {{"property", "Category"}, {"value", "Unauthorized"}};
    const auto before = world->GetScriptBlueprintSources();
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error));
    EXPECT_NE(error.find("Instance Editable"), std::string::npos);
    EXPECT_EQ(world->GetScriptBlueprintSources(), before);
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, edit.actorProperty.identity, error), before[0]);
}

TEST_F(ScriptBlueprintAuthoringTest, InlinePlacementKeepsClassDefaultsInherited)
{
    auto action = Add();
    action.script = {{"class", "Gameplay.PlayerBlueprint"}, {"name", "Player"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["MoveSpeed"], 500);
    EXPECT_TRUE(world->GetScriptBlueprintSources()[0]["properties"].empty());
}

TEST_F(ScriptBlueprintAuthoringTest, StaleAndInvalidAssetRequestsLeaveSceneUnchanged)
{
    std::string id, error;
    auto action = Add();
    action.actorProperty.worldGeneration = 41;
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    action = Add();
    action.script["blueprintPath"] = "Common/Blueprints/BP_missing.json";
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    action.script = {{"class", "Gameplay.Unknown"}, {"name", "Unknown"}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_TRUE(documents->ModifiedDocuments().empty());
    EXPECT_TRUE(world->GetScriptBlueprints().empty());
}

TEST_F(ScriptBlueprintAuthoringTest, InlineComponentEditsStageLocalPoseAndRejectCyclesAtomically)
{
    Json blueprint{{"type", "blueprintInstance"}, {"id", "tree"}, {"name", "Tree"}, {"class", "Gameplay.RotatorBlueprint"},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", Json::object()}, {"components", Json::array({
            {{"id", "root"}, {"type", "scene"}}, {{"id", "child"}, {"type", "scene"}, {"parent", "root"}}})}};
    std::string error;
    ASSERT_TRUE(documents->StageScriptBlueprint(scenePath, blueprint, true, error)) << error;
    PublishDraft();
    UiAction edit;
    edit.type = UiActionType::SetScriptComponent;
    edit.actorProperty.worldGeneration = 42; edit.actorProperty.identity = "blueprintInstance|tree";
    edit.script = {{"component", "child"}, {"field", "position"}, {"value", {10, 20, 30}}};
    std::string id;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprints()[0]["components"][1]["position"], Json::array({10, 20, 30}));
    const auto before = documents->ScriptBlueprintDraft(scenePath, "blueprintInstance|tree", error);
    edit.script = {{"component", "root"}, {"field", "parent"}, {"value", "child"}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error));
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, "blueprintInstance|tree", error), before);
    EditorCommandEnvelope save;
    save.commandId = 1; save.type = EditorCommandType::SaveSceneDocument;
    save.payload = MaterialInstanceAssetPathPayload{"Maps/SC_test/SC_test.json"};
    save.expectedDocumentRevision = documents->ModifiedDocuments().front().revision;
    ASSERT_EQ(documents->Save(save).status, EditorCommandStatus::Succeeded);
    Json saved; std::ifstream(scenePath) >> saved;
    EXPECT_EQ(saved["objects"][1]["components"][1]["position"], Json::array({10, 20, 30}));
}

TEST_F(ScriptBlueprintAuthoringTest, SpringArmFieldsAuthoringRoundTrip)
{
    Json blueprint{{"type", "blueprintInstance"}, {"id", "tree"}, {"name", "Tree"},
        {"class", "Gameplay.RotatorBlueprint"}, {"position", {0, 0, 0}},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"properties", Json::object()},
        {"components", Json::array({
            {{"id", "arm"}, {"type", "springArm"}, {"parent", ""}, {"position", {0, 90, 0}},
                {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"targetArmLength", 300},
                {"socketOffset", {0, 0, 0}}, {"probeRadius", 12}, {"doCollisionTest", true},
                {"collisionMask", 65535}, {"enableCameraLag", false}},
            {{"id", "camera"}, {"type", "camera"}, {"parent", "arm"}, {"position", {0, 0, 0}},
                {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60},
                {"near_clip", 1}, {"far_clip", 10000}, {"autoActivate", true}, {"priority", 0}}})}};
    std::string error;
    ASSERT_TRUE(documents->StageScriptBlueprint(scenePath, blueprint, true, error)) << error;
    PublishDraft();
    UiAction edit;
    edit.type = UiActionType::SetScriptComponent;
    edit.actorProperty.worldGeneration = 42;
    edit.actorProperty.identity = "blueprintInstance|tree";
    std::string id;
    edit.script = {{"component", "arm"}, {"field", "targetArmLength"}, {"value", 450.0}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "socketOffset"}, {"axis", 1}, {"value", 50.0}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "doCollisionTest"}, {"value", false}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "cameraLagSpeed"}, {"value", 5.0}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "targetOffset"}, {"axis", 2}, {"value", 25.0}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "cameraLagMaxTimeStep"}, {"value", 0.01}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "collisionMask"}, {"value", 3.0}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "inheritYaw"}, {"value", false}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "arm"}, {"field", "drawDebug"}, {"value", true}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script = {{"component", "camera"}, {"field", "priority"}, {"value", 3.0}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    PublishDraft();
    const auto& components = world->GetScriptBlueprints()[0]["components"];
    EXPECT_DOUBLE_EQ(components[0]["targetArmLength"].get<double>(), 450.0);
    EXPECT_EQ(components[0]["socketOffset"], Json::array({0.0, 50.0, 0.0}));
    EXPECT_FALSE(components[0]["doCollisionTest"].get<bool>());
    EXPECT_DOUBLE_EQ(components[0]["cameraLagSpeed"].get<double>(), 5.0);
    EXPECT_FALSE(components[0]["inheritYaw"].get<bool>());
    EXPECT_TRUE(components[0]["drawDebug"].get<bool>());
    EXPECT_EQ(components[1]["priority"].get<double>(), 3.0);
    EditorCommandEnvelope save;
    save.commandId = 2; save.type = EditorCommandType::SaveSceneDocument;
    save.payload = MaterialInstanceAssetPathPayload{"Maps/SC_test/SC_test.json"};
    save.expectedDocumentRevision = documents->ModifiedDocuments().front().revision;
    ASSERT_EQ(documents->Save(save).status, EditorCommandStatus::Succeeded);
    Json saved; std::ifstream(scenePath) >> saved;
    const auto& savedArm = saved["objects"][1]["components"][0];
    EXPECT_DOUBLE_EQ(savedArm["targetArmLength"].get<double>(), 450.0);
    EXPECT_EQ(savedArm["socketOffset"], Json::array({0.0, 50.0, 0.0}));
    EXPECT_FALSE(savedArm["doCollisionTest"].get<bool>());
    EXPECT_DOUBLE_EQ(savedArm["cameraLagSpeed"].get<double>(), 5.0);
    EXPECT_EQ(savedArm["targetOffset"], Json::array({0, 0, 25}));
    EXPECT_DOUBLE_EQ(savedArm["cameraLagMaxTimeStep"].get<double>(), 0.01);
    EXPECT_EQ(savedArm["collisionMask"], 3);
    EXPECT_FALSE(savedArm["inheritYaw"].get<bool>());
    EXPECT_TRUE(savedArm["drawDebug"].get<bool>());
    EXPECT_EQ(saved["objects"][1]["components"][1]["priority"].get<double>(), 3.0);
}

TEST_F(ScriptBlueprintAuthoringTest, InheritedComponentsCannotBeModifiedThroughSceneInstance)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    UiAction edit;
    edit.type = UiActionType::SetScriptComponent;
    edit.actorProperty.worldGeneration = 42; edit.actorProperty.identity = "blueprintInstance|" + id;
    edit.script = {{"component", "root"}, {"field", "parent"}, {"value", ""}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error));
    EXPECT_NE(error.find("not Instance Editable"), std::string::npos);
}

TEST_F(ScriptBlueprintAuthoringTest, InheritedComponentVectorEditsMergeSparseAxesAndReset)
{
    Json asset{{"type", "blueprint"}, {"name", "Components"}, {"class", "ScriptFixtures.PassiveBlueprint"},
        {"properties", Json::object()}, {"components", Json::array({
            {{"id", "pivot"}, {"type", "scene"}, {"position", {1, 2, 3}}},
            {{"id", "camera"}, {"type", "camera"}, {"parent", "pivot"}, {"fov", 60}}})},
        {"componentInstanceEditable", {{"pivot", {"position"}}, {"camera", {"fov"}}}}};
    const auto assetPath = root / "Common/Blueprints/BP_rotator.json";
    std::ofstream(assetPath) << asset.dump();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction edit;
    edit.type = UiActionType::SetScriptComponent;
    edit.actorProperty = {42, "blueprintInstance|" + id};
    edit.script = {{"component", "pivot"}, {"field", "position"}, {"axis", 0}, {"value", 10}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.script["axis"] = 2; edit.script["value"] = 30;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    auto authored = documents->ScriptBlueprintDraft(scenePath, edit.actorProperty.identity, error);
    EXPECT_EQ(authored["componentOverrides"], Json({{"pivot", {{"position", {10, 2, 30}}}}}));
    EXPECT_FALSE(authored.contains("components"));
    EXPECT_TRUE(authored["properties"].empty());
    edit.script = {{"component", "pivot"}, {"field", "position"}, {"reset", true}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    authored = documents->ScriptBlueprintDraft(scenePath, edit.actorProperty.identity, error);
    EXPECT_FALSE(authored.contains("componentOverrides"));
    EXPECT_EQ(documents->ResolveBlueprint(authored, scenePath)["components"], asset["components"]);
}

TEST_F(ScriptBlueprintAuthoringTest, InheritedComponentRejectedEditsCannotStageStructuralOrUnexposedChanges)
{
    Json asset{{"type", "blueprint"}, {"name", "Components"}, {"class", "ScriptFixtures.PassiveBlueprint"},
        {"properties", Json::object()}, {"components", Json::array({{{"id", "pivot"}, {"type", "scene"}}})},
        {"componentInstanceEditable", {{"pivot", {"position"}}}}};
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction edit; edit.type = UiActionType::SetScriptComponent;
    edit.actorProperty = {42, "blueprintInstance|" + id};
    const auto before = documents->ScriptBlueprintDraft(scenePath, edit.actorProperty.identity, error);
    for (const auto& payload : {Json{{"component", "pivot"}, {"field", "parent"}, {"value", ""}},
        Json{{"component", "pivot"}, {"field", "rotation"}, {"value", {0, 90, 0}}},
        Json{{"component", "pivot"}, {"field", "position"}, {"value", {0, 0}}}})
    {
        edit.script = payload;
        EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << payload;
        EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, edit.actorProperty.identity, error), before);
    }
}

TEST_F(ScriptBlueprintAuthoringTest, InheritedComponentOverridesSurviveDuplicateSaveAndReopen)
{
    Json asset{{"type", "blueprint"}, {"name", "Components"}, {"class", "ScriptFixtures.PassiveBlueprint"},
        {"properties", Json::object()}, {"components", Json::array({{{"id", "camera"}, {"type", "camera"}}})},
        {"componentInstanceEditable", {{"camera", {"fov"}}}}};
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction edit; edit.type = UiActionType::SetScriptComponent;
    edit.actorProperty = {42, "blueprintInstance|" + id};
    edit.script = {{"component", "camera"}, {"field", "fov"}, {"value", 80}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, id, error)) << error;
    edit.type = UiActionType::DuplicateScriptBlueprint;
    std::string copy;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, edit, copy, error)) << error;
    PublishDraft();
    EditorCommandEnvelope save;
    save.commandId = 1; save.type = EditorCommandType::SaveSceneDocument;
    save.payload = MaterialInstanceAssetPathPayload{"Maps/SC_test/SC_test.json"};
    save.expectedDocumentRevision = documents->ModifiedDocuments().front().revision;
    ASSERT_EQ(documents->Save(save).status, EditorCommandStatus::Succeeded);
    Editor::SceneDocumentService reopened(root);
    ASSERT_TRUE(reopened.Open("Maps/SC_test/SC_test.json", error)) << error;
    for (const auto& identity : {id, copy})
    {
        const auto authored = reopened.ScriptBlueprintDraft(scenePath, "blueprintInstance|" + identity, error);
        EXPECT_EQ(authored["componentOverrides"], Json({{"camera", {{"fov", 80}}}}));
        EXPECT_FALSE(authored.contains("components"));
    }
}

TEST_F(ScriptBlueprintAuthoringTest, DuplicateAndRenamePreserveSparseAssetOverridesAndPersistentIdentity)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction action;
    action.actorProperty = {42, "blueprintInstance|" + id};
    action.type = UiActionType::DuplicateScriptBlueprint;
    const auto original = world->GetScriptBlueprintSources()[0];
    std::string copy;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, copy, error)) << error;
    PublishDraft();
    EXPECT_NE(copy, id);
    const auto duplicated = world->GetScriptBlueprintSources()[1];
    EXPECT_EQ(duplicated["blueprintPath"], original["blueprintPath"]);
    EXPECT_TRUE(duplicated["properties"].empty());
    EXPECT_EQ(duplicated["position"], original["position"]);
    action.type = UiActionType::RenameScriptBlueprint;
    action.actorProperty.identity = "blueprintInstance|" + copy;
    action.script = {{"name", "Renamed"}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, copy, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprintSources()[1]["id"], copy);
    EXPECT_EQ(world->GetScriptBlueprintSources()[1]["name"], "Renamed");
    EXPECT_EQ(world->GetScriptBlueprintSources()[0], original);
}

TEST_F(ScriptBlueprintAuthoringTest, DeleteUndoRedoAndSaveOperateOnAuthorSceneOnly)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    const auto before = world->GetScriptBlueprintSources();
    UiAction action;
    action.actorProperty = {42, "blueprintInstance|" + id};
    action.type = UiActionType::DeleteScriptBlueprint;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_TRUE(world->GetScriptBlueprintSources().empty());
    action.type = UiActionType::UndoSceneEdit;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprintSources(), before);
    action.type = UiActionType::RedoSceneEdit;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_TRUE(world->GetScriptBlueprintSources().empty());
    Json disk; std::ifstream(scenePath) >> disk;
    EXPECT_EQ(disk["objects"].size(), 1u);
}

TEST_F(ScriptBlueprintAuthoringTest, UndoAfterSavingCreatesDirtyDraftWithoutChangingDisk)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    EditorCommandEnvelope save;
    save.commandId = 1; save.type = EditorCommandType::SaveSceneDocument;
    save.payload = MaterialInstanceAssetPathPayload{"Maps/SC_test/SC_test.json"};
    save.expectedDocumentRevision = documents->ModifiedDocuments().front().revision;
    ASSERT_EQ(documents->Save(save).status, EditorCommandStatus::Succeeded);
    ASSERT_TRUE(documents->CanUndo(scenePath));
    ASSERT_TRUE(documents->Undo(scenePath, false, error)) << error;
    PublishDraft();
    EXPECT_TRUE(world->GetScriptBlueprintSources().empty());
    EXPECT_EQ(documents->ModifiedDocuments().size(), 1u);
    Json disk; std::ifstream(scenePath) >> disk;
    EXPECT_EQ(disk["objects"].size(), 2u);
}

TEST_F(ScriptBlueprintAuthoringTest, InlineComponentAddAndSubtreeDeleteCanBeUndone)
{
    auto action = Add();
    action.script = {{"class", "ScriptFixtures.PassiveBlueprint"}, {"name", "Tree"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    PublishDraft();
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.type = UiActionType::AddScriptComponent;
    action.script = {{"node", {{"type", "scene"}}}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    const auto rootId = world->GetScriptBlueprintSources()[0]["components"][0]["id"].get<std::string>();
    action.script = {{"node", {{"type", "camera"}, {"parent", rootId}, {"autoActivate", true}}}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprintSources()[0]["components"].size(), 2u);
    action.type = UiActionType::RemoveScriptComponent;
    action.script = {{"component", rootId}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_TRUE(world->GetScriptBlueprintSources()[0]["components"].empty());
    ASSERT_TRUE(documents->Undo(scenePath, false, error));
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprintSources()[0]["components"].size(), 2u);
}

TEST_F(ScriptBlueprintAuthoringTest, PhysicsShapeSwitchIsAtomicAndInvalidCoefficientKeepsDraft)
{
    auto action = Add();
    action.script = {{"class", "ScriptFixtures.PassiveBlueprint"}, {"name", "Body"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    PublishDraft();
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.type = UiActionType::SetScriptPhysics;
    action.script = {{"field", "motion"}, {"value", "dynamic"}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    EXPECT_EQ(world->GetScriptBlueprintSources()[0]["physics"]["shape"], "box");
    action.script = {{"field", "motion"}, {"value", "character"}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    const auto before = world->GetScriptBlueprintSources()[0];
    EXPECT_EQ(before["physics"]["shape"], "capsule");
    EXPECT_FALSE(before["physics"].contains("halfExtents"));
    action.script = {{"field", "restitution"}, {"value", 2}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, "blueprintInstance|" + id, error), before);
    action.script = {{"field", "motion"}, {"value", "none"}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    PublishDraft();
    EXPECT_FALSE(world->GetScriptBlueprintSources()[0].contains("physics"));
}

TEST_F(ScriptBlueprintAuthoringTest, InheritedPhysicsAndPlayingAuthorRequestsAreRejected)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    UiAction action;
    action.actorProperty = {42, "blueprintInstance|" + id};
    action.type = UiActionType::SetScriptPhysics;
    action.script = {{"field", "motion"}, {"value", "dynamic"}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    action.type = UiActionType::DeleteScriptBlueprint;
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_EQ(world->GetScriptBlueprints().size(), 1u);
}

TEST_F(ScriptBlueprintAuthoringTest, UndoRecoversStagedCandidateAndNewEditDiscardsRedo)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    // 即使资源准备失败、World 尚未提交，也能撤销草稿；不能被 awaitingWorldLoad 锁死。
    ASSERT_TRUE(documents->Undo(scenePath, false, error));
    PublishDraft();
    EXPECT_TRUE(world->GetScriptBlueprintSources().empty());
    EXPECT_TRUE(documents->CanUndo(scenePath, true));
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    EXPECT_FALSE(documents->CanUndo(scenePath, true));
}

TEST_F(ScriptBlueprintAuthoringTest, StandardApiDraftChangeInvalidatesSceneUndo)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    ASSERT_TRUE(documents->CanUndo(scenePath));
    EditorCommandEnvelope command;
    command.commandId = 1; command.type = EditorCommandType::ModifySceneDocument;
    command.expectedDocumentRevision = documents->ModifiedDocuments().front().revision;
    EditorAssetFieldChange change;
    change.field = "name"; change.value = std::string("API title");
    command.payload = ModifyAssetDocumentPayload{"Maps/SC_test/SC_test.json", {change}};
    ASSERT_EQ(documents->Execute(command).status, EditorCommandStatus::Succeeded);
    EXPECT_FALSE(documents->CanUndo(scenePath));
    EXPECT_FALSE(documents->Undo(scenePath, false, error));
    EXPECT_EQ(documents->DraftForWorldLoad(scenePath, error)["name"], "API title");
}

TEST_F(ScriptBlueprintAuthoringTest, SceneHistoryKeepsOnlyTheLast32Edits)
{
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error));
    PublishDraft();
    UiAction rename;
    rename.type = UiActionType::RenameScriptBlueprint;
    rename.actorProperty = {42, "blueprintInstance|" + id};
    for (int index = 0; index < 35; ++index)
    {
        rename.script = {{"name", "Name " + std::to_string(index)}};
        ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, rename, id, error)) << error;
        PublishDraft();
    }
    for (int index = 0; index < 32; ++index)
        ASSERT_TRUE(documents->Undo(scenePath, false, error)) << error;
    EXPECT_FALSE(documents->CanUndo(scenePath));
    PublishDraft();
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, "blueprintInstance|" + id, error)["name"], "Name 2");
}

TEST_F(ScriptBlueprintAuthoringTest, AddingCameraPromotesLegacyRootIdentityAndValidatesProjectionEdits)
{
    auto action = Add();
    action.script = {{"class", "ScriptFixtures.PassiveBlueprint"}, {"name", "Legacy"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    PublishDraft();
    auto blueprint = documents->ScriptBlueprintDraft(scenePath, "blueprintInstance|" + id, error);
    blueprint["components"] = Json::array({{{"type", "scene"}}});
    ASSERT_TRUE(documents->StageScriptBlueprint(scenePath, blueprint, false, error));
    PublishDraft();
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.type = UiActionType::AddScriptComponent;
    action.script = {{"node", {{"type", "camera"}, {"parent", "root"}}}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    const auto before = world->GetScriptBlueprintSources()[0];
    EXPECT_EQ(before["components"][0]["id"], "root");
    action.type = UiActionType::SetScriptComponent;
    action.script = {{"component", before["components"][1]["id"]}, {"field", "fov"}, {"value", 75}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    action.script["value"] = 180;
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, "blueprintInstance|" + id, error)["components"][1]["fov"], 75);
}
TEST_F(ScriptBlueprintAuthoringTest, LogicComponentInlineEditsUseClassPermissionsAndSupportUndo)
{
    auto action = Add();
    action.script = {{"class", "ScriptFixtures.PassiveBlueprint"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.type = UiActionType::AddScriptLogicComponent;
    action.script = {{"class", "ScriptFixtures.CounterComponent"}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    const auto component = world->GetScriptBlueprintSources()[0]["scriptComponents"][0]["id"];
    action.type = UiActionType::SetScriptLogicComponent;
    action.script = {{"component", component}, {"property", "Speed"}, {"value", 35}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    auto draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_EQ(draft["scriptComponents"][0]["properties"], Json({{"Speed", 35}}));
    action.script = {{"component", component}, {"property", "Ticks"}, {"value", 99}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error), draft);
    action.script = {{"component", component}, {"property", "Speed"}, {"reset", true}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    EXPECT_TRUE(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error)
        ["scriptComponents"][0]["properties"].empty());
    action.type = UiActionType::RemoveScriptLogicComponent;
    action.script = {{"component", component}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    EXPECT_FALSE(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error).contains("scriptComponents"));
    ASSERT_TRUE(documents->Undo(scenePath, false, error)) << error;
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error)
        ["scriptComponents"][0]["id"], component);
}

TEST_F(ScriptBlueprintAuthoringTest, LogicComponentInheritedVectorAxesMergeAndResetRemainsSparse)
{
    Json asset{{"type", "blueprint"}, {"name", "Logic"}, {"class", "ScriptFixtures.PassiveBlueprint"},
        {"properties", Json::object()}, {"scriptComponents", Json::array({
            {{"id", "health"}, {"class", "ScriptFixtures.CounterComponent"},
                {"properties", {{"Orientation", {0.6, 0, 0.8, 0}}}},
                {"instanceEditable", {"Speed", "Orientation"}}}})}};
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction action;
    action.type = UiActionType::SetScriptLogicComponent;
    action.actorProperty = {42, "blueprintInstance|" + id};
    action.script = {{"component", "health"}, {"property", "Orientation"}, {"axis", 0}, {"value", -0.6}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    action.script["axis"] = 2; action.script["value"] = -0.8;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    auto draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_EQ(draft["scriptComponentOverrides"]["health"]["Orientation"], Json::array({-0.6, 0, -0.8, 0}));
    EXPECT_FALSE(draft.contains("scriptComponents"));
    action.script = {{"component", "health"}, {"property", "Orientation"}, {"reset", true}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_FALSE(draft.contains("scriptComponentOverrides"));
    EXPECT_EQ(documents->ResolveBlueprint(draft, scenePath)["scriptComponents"], asset["scriptComponents"]);
}

TEST_F(ScriptBlueprintAuthoringTest, LogicComponentInheritedRejectedRequestsCannotChangeDraft)
{
    Json asset{{"type", "blueprint"}, {"name", "Logic"}, {"class", "ScriptFixtures.PassiveBlueprint"},
        {"properties", Json::object()}, {"scriptComponents", Json::array({
            {{"id", "health"}, {"class", "ScriptFixtures.CounterComponent"},
                {"properties", Json::object()}, {"instanceEditable", {"Speed"}}}})}};
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    UiAction action;
    action.actorProperty = {42, "blueprintInstance|" + id};
    const auto before = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    for (const auto type : {UiActionType::SetScriptLogicComponent,
        UiActionType::AddScriptLogicComponent, UiActionType::RemoveScriptLogicComponent})
    {
        action.type = type;
        action.script = {{"component", "health"}, {"class", "ScriptFixtures.CounterComponent"},
            {"property", "Ticks"}, {"value", 99}};
        EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
        EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error), before);
    }
    action.type = UiActionType::SetScriptLogicComponent;
    action.script = {{"component", "health"}, {"field", "enabled"}, {"value", false}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    action.script = {{"component", "health"}, {"property", "Speed"}, {"value", "invalid"}};
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error), before);
}

TEST_F(ScriptBlueprintAuthoringTest, CustomDataLeafEditsAndResetPreserveSparseSiblingAndUndo)
{
    auto action = Add();
    action.script = {{"class", "ScriptFixtures.CustomDataBlueprint"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    action.type = UiActionType::SetScriptProperty;
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.script = {{"property", "Settings"}, {"path", {"Stats", "Damage"}}, {"value", 42}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    action.script = {{"property", "Settings"}, {"path", {"Stats", "Offset", 2}}, {"value", 30}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    auto draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_EQ(draft["properties"]["Settings"], (Json{{"Stats",
        {{"Damage", 42}, {"Offset", Json::array({1, 2, 30})}}}}));
    action.script = {{"property", "Settings"}, {"path", {"Stats", "Damage"}}, {"operation", "reset"}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    draft = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    EXPECT_FALSE(draft["properties"]["Settings"]["Stats"].contains("Damage"));
    EXPECT_EQ(draft["properties"]["Settings"]["Stats"]["Offset"], Json::array({1, 2, 30}));
    ASSERT_TRUE(documents->Undo(scenePath, false, error)) << error;
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error)
        ["properties"]["Settings"]["Stats"]["Damage"], 42);
}

TEST_F(ScriptBlueprintAuthoringTest, CustomDataAssetCreationIncludesEffectiveDefaultsAndLogicConfiguration)
{
    Json asset{{"type", "blueprint"}, {"name", "Custom"}, {"class", "ScriptFixtures.CustomDataBlueprint"},
        {"properties", {{"Settings", {{"Stats", {{"Damage", 20}, {"Offset", {4, 5, 6}}}}}}}},
        {"instanceEditable", {"Settings"}}, {"scriptComponents", Json::array({
            {{"id", "data"}, {"class", "ScriptFixtures.CustomDataComponent"},
                {"properties", {{"Settings", {{"Stats", {{"Damage", 30}, {"Offset", {7, 8, 9}}}}}}}},
                {"instanceEditable", {"Settings"}}}})}};
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump();
    auto action = Add();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    PublishDraft();
    action.actorProperty.identity = "blueprintInstance|" + id;
    action.type = UiActionType::SetScriptProperty;
    action.script = {{"property", "Settings"}, {"path", {"Stats", "Damage"}}, {"value", 50}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    action.type = UiActionType::SetScriptLogicComponent;
    action.script = {{"component", "data"}, {"property", "Settings"},
        {"path", {"Stats", "Damage"}}, {"value", 60}};
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    const auto before = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    action.type = UiActionType::CreateScriptBlueprintAsset;
    action.stringValue = "Common/Blueprints/BP_custom_copy.json";
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    const auto copy = ReadScriptBlueprintAsset(root, action.stringValue);
    EXPECT_EQ(copy["properties"]["Settings"]["Stats"]["Damage"], 50);
    EXPECT_EQ(copy["properties"]["Settings"]["Stats"]["Offset"], Json::array({4, 5, 6}));
    EXPECT_EQ(copy["scriptComponents"][0]["properties"]["Settings"]["Stats"]["Damage"], 60);
    EXPECT_EQ(copy["scriptComponents"][0]["properties"]["Settings"]["Stats"]["Offset"], Json::array({7, 8, 9}));
    EXPECT_FALSE(copy.contains("__propertyDefaults"));
    EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error), before);
}

TEST_F(ScriptBlueprintAuthoringTest, BlueprintClassAssetCreationDoesNotRequireAnExistingInstance)
{
    UiAction action;
    action.type = UiActionType::CreateScriptBlueprintAsset;
    action.actorProperty.worldGeneration = 42;
    action.stringValue = "Maps/SC_test/Blueprints/BP_door.json";
    action.script = {{"class", "ScriptFixtures.PassiveBlueprint"}, {"name", "Door"}};
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error)) << error;
    EXPECT_EQ(ReadScriptBlueprintAsset(root, action.stringValue)["class"], "ScriptFixtures.PassiveBlueprint");
    EXPECT_FALSE(std::filesystem::exists(root / "Maps/SC_test/Blueprints/BP_door.blueprint.cs"));
    EXPECT_TRUE(documents->ModifiedDocuments().empty());
    EXPECT_TRUE(world->GetScriptBlueprints().empty());
    EXPECT_TRUE(id.empty());
}

TEST_F(ScriptBlueprintAuthoringTest, BlueprintClassAssetCreationRejectsStaleWorldUnknownAndComponentTypes)
{
    UiAction action;
    action.type = UiActionType::CreateScriptBlueprintAsset;
    action.actorProperty.worldGeneration = 41;
    action.stringValue = "Maps/SC_test/Blueprints/BP_door.json";
    action.script = {{"class", "ScriptFixtures.PassiveBlueprint"}, {"name", "Door"}};
    std::string id, error;
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    action.actorProperty.worldGeneration = 42;
    action.script["class"] = "MyGame.UnknownBlueprint";
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    action.script["class"] = "ScriptFixtures.CounterComponent";
    EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
    EXPECT_FALSE(std::filesystem::exists(root / "Maps/SC_test/Blueprints"));
}

TEST_F(ScriptBlueprintAuthoringTest, CustomDataChildPathCannotBypassAssetRootPermission)
{
    Json asset{{"type", "blueprint"}, {"name", "Custom"}, {"class", "ScriptFixtures.CustomDataBlueprint"},
        {"properties", Json::object()}, {"instanceEditable", Json::array()}};
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump();
    std::string id, error;
    ASSERT_TRUE(StageScriptBlueprintAction(*world, runtime, *documents, Add(), id, error)) << error;
    PublishDraft();
    auto action = Add();
    action.type = UiActionType::SetScriptProperty;
    action.actorProperty.identity = "blueprintInstance|" + id;
    const auto before = documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error);
    for (const auto operation : {"set", "reset"})
    {
        action.script = {{"property", "Settings"}, {"path", {"Stats", "Damage"}},
            {"operation", operation}, {"value", 99}};
        EXPECT_FALSE(StageScriptBlueprintAction(*world, runtime, *documents, action, id, error));
        EXPECT_EQ(documents->ScriptBlueprintDraft(scenePath, action.actorProperty.identity, error), before);
    }
}
#endif
}
