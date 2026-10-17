#include <gtest/gtest.h>
#include "mesh/meshCollisionGeometry.h"
#include "script/scriptPhysicsMeshGeometry.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

std::shared_ptr<const std::vector<Eigen::Vector3f>> Triangle()
{
    MeshSection section;
    section.vertices.resize(3);
    section.vertices[0].position = {0, 0, 0};
    section.vertices[1].position = {10, 0, 0};
    section.vertices[2].position = {0, 0, 10};
    section.indices = {0, 2, 1};
    return FreezeMeshCollisionTriangles(section);
}

Json Blueprint(const std::string& id)
{
    return {{"id", id}, {"position", {100, 200, 300}}, {"rotation", {0, 35, 0}}, {"scale", {2, 2, 2}},
        {"physics", {{"shape", "mesh"}, {"component", "body"}}},
        {"components", Json::array({{{"id", "body"}, {"type", "mesh"},
            {"modelPath", "Common/Meshes/SM_test.json"}, {"position", {3, 4, 5}}}})}};
}

void AddMesh(World& world, const Json& blueprint, const std::string& name)
{
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity(blueprint.at("id"), blueprint["components"][0]);
    mesh.model = ScriptComponentMatrix(blueprint, 0);
    mesh.collisionTriangles = Triangle();
    world.AddMeshObject(name, mesh);
}

TEST(ScriptPhysicsMeshGeometry, FrozenTriangleOrderMatchesImportedIndexWinding)
{
    const auto triangles = Triangle();
    ASSERT_EQ(triangles->size(), 3u);
    EXPECT_EQ((*triangles)[0], Eigen::Vector3f(0, 0, 0));
    EXPECT_EQ((*triangles)[1], Eigen::Vector3f(0, 0, 10));
    EXPECT_EQ((*triangles)[2], Eigen::Vector3f(10, 0, 0));
}

TEST(ScriptPhysicsMeshGeometry, RootLocalTrianglesPreserveComponentPlacementAndRemoveRootTrs)
{
    World world(7, "scene.json");
    const auto blueprint = Blueprint("floor");
    world.SetScriptBlueprints(Json::array({blueprint}));
    AddMesh(world, blueprint, "body");
    const auto geometry = BuildScriptPhysicsMeshGeometry(world);
    ASSERT_EQ(geometry.size(), 1u);
    const auto& triangles = geometry.at("floor/body");
    EXPECT_TRUE(triangles[0].isApprox(Eigen::Vector3f(3, 4, 5), 1e-4f));
    EXPECT_TRUE(triangles[1].isApprox(Eigen::Vector3f(3, 4, 15), 1e-4f));
}

TEST(ScriptPhysicsMeshGeometry, SectionsAndSpawnTemplatesAreCollectedUnderStableComponentKeys)
{
    World world(7, "scene.json");
    const auto blueprint = Blueprint("~template.floor");
    world.SetScriptTemplates({{"Blueprints/BP_floor.json", blueprint}});
    AddMesh(world, blueprint, "section-a");
    AddMesh(world, blueprint, "section-b");
    const auto geometry = BuildScriptPhysicsMeshGeometry(world);
    ASSERT_EQ(geometry.at("~template.floor/body").size(), 6u);
}

TEST(ScriptPhysicsMeshGeometry, AsyncRefreshRemovesLiveRootAndComponentMotionBeforeFreezing)
{
    World world(7, "scene.json");
    const auto authored = Blueprint("floor");
    world.SetScriptBlueprints(Json::array({authored}));
    AddMesh(world, authored, "body");
    const auto original = BuildScriptPhysicsMeshGeometry(world).at("floor/body");
    auto live = authored;
    live["position"] = {600, 400, -300};
    live["rotation"] = {0, 90, 0};
    live["components"][0]["position"] = {90, -40, 30};
    AddMesh(world, live, "body");
    world.SetScriptBlueprintStates(Json::array({live}));
    const auto refreshed = BuildScriptPhysicsMeshGeometry(world).at("floor/body");
    ASSERT_EQ(refreshed.size(), original.size());
    for (size_t index = 0; index < original.size(); ++index)
        EXPECT_TRUE(refreshed[index].isApprox(original[index], 1e-3f));
}

TEST(ScriptPhysicsMeshGeometry, MeshesWithoutCpuGeometryCannotInventCollisionFromBounds)
{
    World world(7, "scene.json");
    const auto blueprint = Blueprint("floor");
    world.SetScriptBlueprints(Json::array({blueprint}));
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = ScriptMeshIdentity("floor", blueprint["components"][0]);
    mesh.localBoundsMin = {-100, -100, -100};
    mesh.localBoundsMax = {100, 100, 100};
    world.AddMeshObject("body", mesh);
    const auto geometry = BuildScriptPhysicsMeshGeometry(world);
    EXPECT_TRUE(geometry.at("floor/body").empty());
}

TEST(ScriptPhysicsMeshGeometry, NonMeshPhysicsDoesNotRetainExpandedTriangleCopies)
{
    World world(7, "scene.json");
    auto blueprint = Blueprint("box");
    blueprint["physics"] = {{"shape", "box"}, {"halfExtents", {10, 10, 10}}};
    world.SetScriptBlueprints(Json::array({blueprint}));
    AddMesh(world, blueprint, "body");
    EXPECT_TRUE(BuildScriptPhysicsMeshGeometry(world).empty());
}
} // namespace
