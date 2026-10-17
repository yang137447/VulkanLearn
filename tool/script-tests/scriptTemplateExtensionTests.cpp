#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "engine/assetPreparationSourceSnapshot.h"
#include "script/scriptScene.h"
#include "script/scriptRuntime.h"
#include "world/scriptTemplateExtension.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;
const std::string path = "Common/Blueprints/BP_dynamic.json";

Json Template()
{
    return {{"type", "blueprintInstance"}, {"id", "~dynamic.test"}, {"name", "Dynamic"},
        {"class", "Gameplay.RotatorBlueprint"}, {"properties", Json::object()},
        {"__templatePath", path}, {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"components", Json::array({{{"type", "mesh"}, {"id", "body"},
            {"modelPath", "Common/Meshes/SM_dynamic.json"}}})}};
}
WorldMeshObject Mesh()
{
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("~dynamic.test", Template()["components"][0]);
    mesh.debugName = mesh.sceneObjectIdentity;
    mesh.meshKey = "dynamic-mesh";
    return mesh;
}

TEST(ScriptTemplateExtension, PreparationDoesNotPublishTemplatesOrHiddenMeshes)
{
    World world(8, "scene");
    auto extension = world.PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {});
    EXPECT_TRUE(world.GetScriptTemplates().empty());
    EXPECT_TRUE(world.GetMeshObjects().empty());
    EXPECT_EQ(extension->templates.at(path), Template());
    EXPECT_FALSE(extension->meshes.at(Mesh().debugName).visible);
}

TEST(ScriptTemplateExtension, CommitPreservesAuthorDeclarationsRuntimeStatesAndGeneration)
{
    World world(8, "scene");
    const Json authored = Json::array({{{"id", "existing"}}});
    const Json states = Json::array({{{"id", "existing"}, {"position", {1, 2, 3}}}});
    world.SetScriptBlueprints(authored);
    world.SetScriptBlueprintStates(states);
    world.CommitScriptTemplateExtension(world.PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {}));
    EXPECT_EQ(world.GetGeneration(), 8u);
    EXPECT_EQ(world.GetScriptBlueprints(), authored);
    EXPECT_EQ(world.GetScriptBlueprintStates(), states);
    EXPECT_EQ(world.GetScriptAssetDigests().at(path), "digest");
    EXPECT_EQ(world.GetScriptTemplates().at(path), Template());
    EXPECT_FALSE(world.GetMeshObjects().at(Mesh().debugName).visible);
}

TEST(ScriptTemplateExtension, RemovalRejectsLiveInstancesWithoutChangingPublishedState)
{
    World world(8, "scene");
    world.CommitScriptTemplateExtension(world.PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {}));
    world.SetScriptBlueprintStates(Json::array({{{"id", "~spawn.1"}, {"__spawnPath", path}, {"destroyed", false}}}));
    EXPECT_THROW(world.PrepareScriptTemplateRemoval(path), std::invalid_argument);
    EXPECT_TRUE(world.GetScriptTemplates().contains(path));
    EXPECT_EQ(world.GetMeshObjects().size(), 1u);
}

TEST(ScriptTemplateExtension, RemovalDropsHiddenTemplateAndDestroyedSpawnGeometry)
{
    World world(8, "scene");
    world.CommitScriptTemplateExtension(world.PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {}));
    auto spawn = Template();
    spawn["id"] = "~spawn.1";
    world.CloneScriptBlueprintMeshes("~dynamic.test", spawn);
    world.SetScriptBlueprintStates(Json::array({{{"id", "~spawn.1"}, {"__spawnPath", path}, {"destroyed", true}}}));
    ASSERT_EQ(world.GetMeshObjects().size(), 2u);
    world.CommitScriptTemplateExtension(world.PrepareScriptTemplateRemoval(path));
    EXPECT_TRUE(world.GetScriptTemplates().empty());
    EXPECT_TRUE(world.GetMeshObjects().empty());
    EXPECT_TRUE(world.GetScriptResolvedBlueprints().empty());
    EXPECT_TRUE(world.GetScriptAssetDigests().empty());
}

TEST(ScriptTemplateExtension, DuplicatePathAndMeshIdentityFailBeforePublishing)
{
    World world(8, "scene");
    world.AddMeshObject(Mesh().debugName, Mesh());
    EXPECT_THROW(world.PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {}), std::invalid_argument);
    EXPECT_TRUE(world.GetScriptTemplates().empty());
    world.SetScriptTemplates({{path, Template()}});
    EXPECT_THROW(world.PrepareScriptTemplateExtension(path, "digest", Template(), {}, {}), std::invalid_argument);
}

TEST(ScriptTemplateExtension, AsyncCommitDoesNotRebuildExistingMeshPoolFromMovedLiveObjects)
{
    World world(8, "scene");
    auto existing = Template();
    existing["id"] = "existing";
    existing["components"][0]["modelPath"] = "Common/Meshes/SM_existing.json";
    existing.erase("__templatePath");
    world.SetScriptResolvedBlueprints(Json::array({existing}));
    auto mesh = Mesh();
    mesh.sceneObjectIdentity = ScriptMeshIdentity("existing", existing["components"][0]);
    mesh.debugName = mesh.sceneObjectIdentity;
    world.AddMeshObject(mesh.debugName, mesh);
    world.PrepareScriptComponentMeshTemplates();
    world.ApplyScriptBlueprintVisuals(existing);
    existing["position"] = {100, 0, 0};
    world.ApplyScriptBlueprintVisuals(existing);
    world.CommitScriptTemplateExtension(world.PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {}));
    world.CommitScriptTemplateExtension(world.PrepareScriptTemplateRemoval(path));
    auto extra = existing["components"][0];
    extra["id"] = "extra";
    extra["position"] = {5, 0, 0};
    existing["components"].push_back(extra);
    world.SynchronizeScriptComponentMeshes(existing);
    const auto& extraMesh = world.GetMeshObjects().at(ScriptMeshIdentity("existing", extra));
    EXPECT_TRUE((extraMesh.model.block<3, 1>(0, 3).isApprox(Eigen::Vector3f(105, 0, 0))));
}

TEST(PreparedAssetSourceSnapshot, SameTimestampGeometryAndTextureChangesAreRejected)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("vl-source-snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto geometry = root / "mesh.bin", texture = root / "texture.bin";
    std::ofstream(geometry, std::ios::binary) << "geometry";
    std::ofstream(texture, std::ios::binary) << "texture";
    const std::map<std::filesystem::path, ContentDigest> sources{
        {geometry, ContentHasher::HashFile(geometry)}, {texture, ContentHasher::HashFile(texture)}};
    EXPECT_NO_THROW(ValidatePreparedAssetSources(sources));
    const auto stamp = std::filesystem::last_write_time(geometry);
    std::ofstream(geometry, std::ios::binary) << "new-mesh";
    std::filesystem::last_write_time(geometry, stamp);
    EXPECT_THROW(ValidatePreparedAssetSources(sources), std::runtime_error);
    std::ofstream(geometry, std::ios::binary) << "geometry";
    const auto textureStamp = std::filesystem::last_write_time(texture);
    std::ofstream(texture, std::ios::binary) << "new-map";
    std::filesystem::last_write_time(texture, textureStamp);
    EXPECT_THROW(ValidatePreparedAssetSources(sources), std::runtime_error);
    std::filesystem::remove_all(root);
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
TEST(ScriptTemplateExtension, LiveManagedContextSurvivesTemplateCommitAndRemoval)
{
    ScriptRuntimeSubsystem runtime;
    runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
        std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
    auto world = std::make_shared<World>(8, "scene");
    Json actor{{"type", "blueprintInstance"}, {"id", "existing"}, {"name", "Existing"},
        {"class", "Gameplay.RotatorBlueprint"}, {"properties", Json::object()},
        {"components", Json::array()}, {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}};
    runtime.PrepareWorld(*world, {{"objects", Json::array({actor})}});
    runtime.SynchronizeWorld(world);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    const auto context = world->GetScriptContext();
    const auto declarations = context->declarations;
    const auto states = world->GetScriptBlueprintStates();
    world->CommitScriptTemplateExtension(world->PrepareScriptTemplateExtension(path, "digest", Template(), {Mesh()}, {}));
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_EQ(context->declarations, declarations);
    EXPECT_EQ(world->GetScriptBlueprintStates(), states);
    world->CommitScriptTemplateExtension(world->PrepareScriptTemplateRemoval(path));
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_FALSE(runtime.IsEditing());
    runtime.Shutdown();
}
#endif
}
