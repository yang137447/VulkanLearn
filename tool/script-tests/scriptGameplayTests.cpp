#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "script/scriptBlueprintAsset.h"
#include "script/scriptPhysics.h"
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"
#include "world/scriptTemplateExtension.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"
#include "sceneNode.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

Json GameplayBlueprint(const std::string& id, const std::string& type = "ScriptFixtures.PassiveBlueprint")
{
    return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", type},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", Json::object()}, {"components", Json::array()}};
}
Json ComponentTree()
{
    return Json::array({
        {{"id", "arm"}, {"type", "scene"}, {"position", {100, 0, 0}}, {"scale", {2, 1, 1}}},
        {{"id", "ball"}, {"type", "mesh"}, {"parent", "arm"}, {"position", {0, 0, 100}},
            {"rotation", {0, 45, 0}}, {"modelPath", "Common/Meshes/SM_ball.json"}}});
}
Json Body(const std::string& motion)
{
    return {{"motion", motion}, {"shape", "box"}, {"halfExtents", {50, 50, 50}}, {"mass", 1}};
}

TEST(ScriptScene, ComponentTreeRejectsDuplicateMissingAndCyclicParents)
{
    auto nodes = ComponentTree();
    ASSERT_NO_THROW(ValidateScriptComponents(nodes));
    nodes[1]["id"] = "arm";
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
    nodes = ComponentTree(); nodes[1]["parent"] = "missing";
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
    nodes = ComponentTree(); nodes[0]["parent"] = "ball";
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
    nodes = ComponentTree(); nodes[0].erase("id");
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
}

TEST(ScriptScene, ComponentMatrixPreservesParentShearAndImportedSection)
{
    auto blueprint = GameplayBlueprint("tree");
    blueprint["components"] = ComponentTree();
    blueprint["position"] = {10, 20, 30};
    World world(1, "tree");
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = "__blueprint/tree/ball";
    mesh.model(1, 3) = 7;
    const Eigen::Matrix4f imported = mesh.model;
    world.AddMeshObject("__blueprint/tree/ball|section", mesh);
    world.ApplyScriptBlueprintVisuals(blueprint);
    Eigen::Matrix4f expected = ScriptTransformMatrix(blueprint) *
        ScriptTransformMatrix(blueprint["components"][0]) * ScriptTransformMatrix(blueprint["components"][1]) * imported;
    EXPECT_TRUE(world.GetMeshObjects().begin()->second.model.isApprox(expected));
    for (int index = 0; index < 100; ++index) world.ApplyScriptBlueprintVisuals(blueprint);
    EXPECT_TRUE(world.GetMeshObjects().begin()->second.model.isApprox(expected, 1e-4f));
    const Eigen::Vector3f x = expected.block<3, 1>(0, 0), z = expected.block<3, 1>(0, 2);
    EXPECT_GT(std::abs(x.dot(z)), 0.5f);
}

TEST(ScriptScene, PhysicsRejectsInvalidShapesCoefficientsAndRootScale)
{
    auto blueprint = GameplayBlueprint("body");
    blueprint["physics"] = Body("dynamic");
    ASSERT_NO_THROW(ValidateScriptBlueprint(blueprint));
    blueprint["scale"] = {1, 2, 1};
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["scale"] = {-1, -1, -1};
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["scale"] = {1, 1, 1};
    blueprint["physics"]["halfExtents"] = {0, 1, 1};
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"] = Body("dynamic"); blueprint["physics"]["restitution"] = 2;
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"] = {{"shape", "sphere"}, {"radius", 10}};
    EXPECT_NO_THROW(ValidateScriptBlueprint(blueprint));
}

TEST(ScriptScene, CompoundAndMeshPhysicsValidateReferencesDimensionsAndScale)
{
    auto blueprint = GameplayBlueprint("compound");
    blueprint["components"] = Json::array({{{"id", "collider"}, {"type", "scene"},
        {"collision", {{"shape", "sphere"}, {"radius", 25}}}}});
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"] = {{"shape", "compound"}, {"motion", "dynamic"}};
    EXPECT_NO_THROW(ValidateScriptBlueprint(blueprint));
    blueprint["components"][0]["scale"] = {1, 2, 1};
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["components"][0]["scale"] = {2, 2, 2};
    EXPECT_NO_THROW(ValidateScriptBlueprint(blueprint));
    blueprint["components"][0]["collision"]["mass"] = 2;
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["components"][0].erase("collision");
    blueprint["physics"] = {{"shape", "mesh"}, {"component", "collider"}};
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["components"][0]["type"] = "mesh";
    blueprint["components"][0]["modelPath"] = "Common/Meshes/SM_test.json";
    EXPECT_NO_THROW(ValidateScriptBlueprint(blueprint));
    blueprint["physics"]["motion"] = "dynamic";
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
}

TEST(ScriptScene, ConstraintsRejectMissingOrIncompatibleBodiesAndSpawnTemplates)
{
    auto owner = GameplayBlueprint("owner"), target = GameplayBlueprint("target");
    owner["physics"] = Body("dynamic"); target["physics"] = Body("static");
    owner["physics"]["constraints"] = Json::array({{{"id", "joint"}, {"type", "hinge"}, {"target", "target"},
        {"anchor", {0, 0, 0}}, {"axis", {0, 0, 1}}, {"minAngle", -45}, {"maxAngle", 45}}});
    EXPECT_NO_THROW(ValidateScriptBlueprint(owner));
    EXPECT_NO_THROW(ValidateScriptPhysicsReferences(Json::array({owner, target}), Json::object()));
    EXPECT_THROW(ValidateScriptPhysicsReferences(Json::array({owner}), Json::object()), std::invalid_argument);
    EXPECT_THROW(ValidateScriptPhysicsReferences(Json::array({owner, target}), {{"path", owner}}), std::invalid_argument);
    owner["physics"]["constraints"][0]["axis"] = {1, 1, 0};
    EXPECT_THROW(ValidateScriptBlueprint(owner), std::invalid_argument);
    owner["physics"]["constraints"][0]["axis"] = {0, 0, 1};
    owner["physics"]["constraints"][0]["typo"] = 1;
    EXPECT_THROW(ValidateScriptBlueprint(owner), std::invalid_argument);
}

TEST(ScriptScene, CapsuleCharacterAndTriggerConfigurationValidatedAtSource)
{
    auto blueprint = GameplayBlueprint("character");
    blueprint["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    EXPECT_NO_THROW(ValidateScriptBlueprint(blueprint));
    blueprint["rotation"] = {1, 0, 0};
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["rotation"] = {0, 30, 0}; blueprint["physics"]["trigger"] = true;
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"]["trigger"] = false; blueprint["physics"]["maxSlopeAngle"] = 90;
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"].erase("maxSlopeAngle"); blueprint["physics"]["halfHeight"] = 0;
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"] = Body("static"); blueprint["physics"]["trigger"] = "true";
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint["physics"] = Body("dynamic"); blueprint["physics"]["stepHeight"] = 20;
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
}

TEST(ScriptScene, CameraComponentsValidateProjectionAndRejectMeshFields)
{
    auto nodes = Json::array({{{"type", "camera"}, {"autoActivate", true}}});
    EXPECT_NO_THROW(ValidateScriptComponents(nodes));
    nodes[0]["fov"] = 179;
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
    nodes[0]["fov"] = 60; nodes[0]["near_clip"] = 20; nodes[0]["far_clip"] = 10;
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
    nodes[0]["far_clip"] = 100; nodes[0]["modelPath"] = "mesh.json";
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
}

TEST(ScriptScene, LightComponentsRequireCanonicalUnitsAndValidPhotometry)
{
    auto nodes = Json::array({{{"type", "spotLight"}, {"intensityUnit", "cd"}}});
    EXPECT_NO_THROW(ValidateScriptComponents(nodes));
    for (const auto& patch : std::vector<Json>{
        {{"intensityUnit", "lm"}}, {{"intensity", -1}}, {{"color", {1, -1, 1}}},
        {{"visible", 1}}, {{"coneAngleInner", 45}}, {{"coneAngleOuter", 90}},
        {{"modelPath", "mesh.json"}}})
    {
        auto candidate = nodes;
        candidate[0].update(patch);
        EXPECT_THROW(ValidateScriptComponents(candidate), std::exception) << patch;
    }
    nodes[0] = {{"type", "directionalLight"}, {"intensityUnit", "lux"}};
    EXPECT_NO_THROW(ValidateScriptComponents(nodes));
    nodes[0]["coneAngleInner"] = 10;
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
    nodes[0] = {{"type", "pointLight"}};
    EXPECT_THROW(ValidateScriptComponents(nodes), std::invalid_argument);
}

TEST(GameplayInputTest, NamedButtonsAndAxesTrackPhysicalEdges)
{
    GameplayInputMap map;
    map.Configure({{"actions", {{"Use", {"F", "MouseLeft"}}}},
        {"axes", {{"Custom", {{{"key", "D"}, {"scale", 2}}, {{"key", "A"}, {"scale", -2}}}}}}});
    map.Sample({}, true, true);
    auto pressed = map.Sample({"F", "D"}, true, true);
    EXPECT_TRUE(pressed["actions"]["Use"]["held"]);
    EXPECT_TRUE(pressed["actions"]["Use"]["pressed"]);
    EXPECT_EQ(pressed["axes"]["Custom"], 2);
    EXPECT_FALSE(map.Sample({"F"}, true, true)["actions"]["Use"]["pressed"]);
    EXPECT_TRUE(map.Sample({}, true, true)["actions"]["Use"]["released"]);
}

TEST(GameplayInputTest, FocusAndUiMaskDoNotCreateSyntheticEdgesOrBlockOtherDevices)
{
    GameplayInputMap map;
    map.Configure({{"actions", {{"Use", {"F", "MouseLeft"}}}}, {"axes", Json::object()}});
    map.Sample({}, true, false);
    EXPECT_TRUE(map.Sample({"F"}, true, false)["actions"]["Use"]["pressed"]);
    auto blocked = map.Sample({"F"}, false, false);
    EXPECT_FALSE(blocked["actions"]["Use"]["held"]);
    EXPECT_FALSE(blocked["actions"]["Use"]["released"]);
    auto restored = map.Sample({"F"}, true, true);
    EXPECT_TRUE(restored["actions"]["Use"]["held"]);
    EXPECT_FALSE(restored["actions"]["Use"]["pressed"]);
    EXPECT_TRUE(map.Sample({}, true, true)["actions"]["Use"]["released"]);
}

TEST(GameplayInputTest, InvalidAxisAndActionBindingsLeavePreviousConfigurationUsable)
{
    GameplayInputMap map;
    EXPECT_THROW(map.Configure({{"actions", {{"Jump", {"MouseX"}}}}}), std::invalid_argument);
    EXPECT_THROW(map.Configure({{"axes", {{"Move", {{{"key", "W"}, {"scale", 1e38}}}}}}}),
        std::invalid_argument);
    map.Sample({}, true, true);
    EXPECT_TRUE(map.Sample({"Space"}, true, true)["actions"]["Jump"]["pressed"]);
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
class ScriptGameplayTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "gameplay");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }
    void Prepare(Json blueprints)
    {
        for (const auto& blueprint : blueprints)
            for (const auto& component : blueprint.at("components"))
                if (component.at("type") == "mesh")
                {
                    WorldMeshObject mesh;
                    mesh.sceneObjectIdentity = ScriptMeshIdentity(blueprint.at("id"), component);
                    mesh.meshKey = "shared-mesh";
                    mesh.materialKey = "shared-material";
                    world->AddMeshObject(mesh.sceneObjectIdentity, mesh);
                }
        runtime.PrepareWorld(*world, {{"objects", blueprints}});
        runtime.SynchronizeWorld(world);
    }
    Json Template(bool physics = false)
    {
        auto blueprint = GameplayBlueprint("template");
        blueprint["__templatePath"] = "Common/Blueprints/BP_child.json";
        blueprint["components"] = ComponentTree();
        if (physics) blueprint["physics"] = Body("dynamic");
        return blueprint;
    }
    void Play()
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    void Tick(float dt = 1.0f / 60, const InputActionState& input = {}) { runtime.Tick(world, dt, input, diagnostics); }
    const Json& State(const std::string& id)
    {
        for (const auto& state : world->GetScriptBlueprintStates())
            if (state.at("id") == id) return state;
        throw std::runtime_error("Missing test blueprint " + id);
    }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptGameplayTest, RuntimeComponentsCommitAfterCallbackReuseMeshesAndRestoreOnStop)
{
    auto owner = GameplayBlueprint("owner", "ScriptFixtures.RuntimeComponentsBlueprint");
    auto source = GameplayBlueprint("source");
    source["__templatePath"] = "Common/Blueprints/BP_source.json";
    source["components"] = Json::array({{{"id", "loaded"}, {"type", "mesh"},
        {"modelPath", "Common/Meshes/SM_test.json"}}});
    Prepare(Json::array({owner, source})); Play();
    ASSERT_EQ(State("owner")["components"].size(), 4u);
    EXPECT_TRUE(world->GetMeshObjects().count("__blueprint/owner/visual"));
    Tick();
    EXPECT_EQ(State("owner")["components"][1]["parent"], "");
    Tick();
    ASSERT_EQ(State("owner")["components"].size(), 1u);
    EXPECT_FALSE(world->GetScriptLights().count("__blueprint/owner/lamp"));
    Tick(); EXPECT_TRUE(State("owner")["components"].empty());
    EXPECT_FALSE(world->GetMeshObjects().count("__blueprint/owner/visual"));
    Tick(); ASSERT_EQ(State("owner")["components"].size(), 1u);
    EXPECT_FALSE(State("owner")["failed"].get<bool>());
    EXPECT_TRUE(world->GetMeshObjects().count("__blueprint/owner/visual"));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error)) << error;
    EXPECT_FALSE(world->GetMeshObjects().count("__blueprint/owner/visual"));
}

TEST_F(ScriptGameplayTest, RuntimeComponentFailedCallbackDropsEntireStructureQueue)
{
    auto owner = GameplayBlueprint("owner", "ScriptFixtures.RuntimeComponentsBlueprint");
    owner["properties"]["Fail"] = true;
    auto source = GameplayBlueprint("source");
    source["__templatePath"] = "Common/Blueprints/BP_source.json";
    source["components"] = Json::array({{{"id", "loaded"}, {"type", "mesh"},
        {"modelPath", "Common/Meshes/SM_test.json"}}});
    Prepare(Json::array({owner, source})); Play();
    EXPECT_TRUE(State("owner")["components"].empty());
    EXPECT_TRUE(State("owner")["failed"].get<bool>());
    EXPECT_FALSE(world->GetMeshObjects().count("__blueprint/owner/visual"));
}

TEST_F(ScriptGameplayTest, RuntimeComponentAttachCycleRollsBackTransformAndStructuralChanges)
{
    auto owner = GameplayBlueprint("owner", "ScriptFixtures.RuntimeComponentsBlueprint");
    owner["properties"]["Cycle"] = true;
    auto source = GameplayBlueprint("source");
    source["__templatePath"] = "Common/Blueprints/BP_source.json";
    source["components"] = Json::array({{{"id", "loaded"}, {"type", "mesh"},
        {"modelPath", "Common/Meshes/SM_test.json"}}});
    Prepare(Json::array({owner, source})); Play(); Tick();
    EXPECT_TRUE(State("owner")["failed"].get<bool>());
    EXPECT_EQ(State("owner")["components"][1]["parent"], "arm");
}

Json LightTree()
{
    return Json::array({
        {{"type", "scene"}, {"id", "arm"}, {"position", {100, 20, 0}},
            {"rotation", {0, 20, 0}}, {"scale", {2, 3, 1}}},
        {{"type", "spotLight"}, {"id", "lamp"}, {"parent", "arm"},
            {"position", {10, 30, 40}}, {"rotation", {15, 0, 0}}, {"intensityUnit", "cd"}},
        {{"type", "pointLight"}, {"id", "fill"}, {"intensityUnit", "cd"}},
        {{"type", "directionalLight"}, {"id", "sun"}, {"intensityUnit", "lux"}}});
}

TEST_F(ScriptGameplayTest, LightValuesFollowHierarchyAndFreezeWithoutChangingPrimarySun)
{
    auto blueprint = GameplayBlueprint("light", "ScriptFixtures.LightBlueprint");
    blueprint["position"] = {10, 100, 20}; blueprint["rotation"] = {0, 45, 0}; blueprint["scale"] = {2, 1, 3};
    blueprint["components"] = LightTree();
    auto sun = std::make_shared<DirectionalLight>();
    world->AddDirectionalLight("authoredSun", sun);
    Prepare(Json::array({blueprint})); Play();
    WorldSnapshotBuilder builder;
    const auto frozen = builder.Build(*world, {}).Value();
    ASSERT_EQ(frozen.lights.size(), 4u);
    const auto identity = "__blueprint/light/lamp";
    const auto before = world->GetScriptLights().at(identity);
    EXPECT_TRUE(before.position.isApprox(ScriptComponentMatrix(blueprint, 1).block<3, 1>(0, 3)));
    EXPECT_TRUE(before.direction.isApprox(ScriptComponentRotation(blueprint, 1) * -Eigen::Vector3f::UnitZ()));
    Tick(0.1f);
    const auto& after = world->GetScriptLights().at(identity);
    EXPECT_EQ(after.intensity, 2500);
    EXPECT_TRUE(after.color.isApprox(Eigen::Vector3f(0.2f, 0.4f, 0.6f)));
    EXPECT_NEAR(after.coneAngleOuter, 20 * 3.14159265358979323846f / 180, 1e-6f);
    EXPECT_FALSE(after.position.isApprox(before.position));
    EXPECT_EQ(world->GetPrimaryDirectionalLight(), sun);
    EXPECT_EQ(world->GetDirectionalLights().size(), 1u);
    for (const auto& light : frozen.lights) EXPECT_NE(light.intensity, 2500);
    EXPECT_EQ(world->GetScriptBlueprints()[0]["components"], blueprint["components"]);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error)) << error;
    EXPECT_EQ(world->GetScriptLights().at(identity).intensity, 1000);
    EXPECT_TRUE(world->GetScriptLights().at(identity).position.isApprox(before.position));
}

TEST_F(ScriptGameplayTest, InvalidLightCallbackRollsBackAndSuppressesFailedOwnerLights)
{
    auto blueprint = GameplayBlueprint("light", "ScriptFixtures.LightBlueprint");
    blueprint["components"] = LightTree(); blueprint["properties"]["Invalid"] = true;
    Prepare(Json::array({blueprint})); Play(); Tick();
    EXPECT_TRUE(State("light")["failed"]);
    EXPECT_EQ(State("light")["components"][1]["intensity"], 1000);
    EXPECT_EQ(State("light")["components"][1]["coneAngleOuter"], 45);
    EXPECT_TRUE(world->GetScriptLights().empty());
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_EQ(world->GetScriptLights().size(), 3u);
}

TEST_F(ScriptGameplayTest, GameplayLightControllerUsesVectorEnumAndStableBlueprintReference)
{
    auto target = GameplayBlueprint("target");
    target["position"] = {100, 200, 300};
    auto controller = GameplayBlueprint("controller", "Gameplay.LightControllerBlueprint");
    controller["components"] = Json::array({{{"type", "pointLight"}, {"intensityUnit", "cd"}}});
    controller["properties"] = {{"Tint", {0.2f, 0.4f, 0.6f}}, {"Intensity", 4000},
        {"Mode", "Pulse"}, {"Period", 2}, {"FollowTarget", "target"}};
    Prepare(Json::array({target, controller})); Play();
    EXPECT_TRUE(world->GetScriptLights().at("__blueprint/controller").position.isApprox(Eigen::Vector3f(100, 200, 300)));
    EXPECT_EQ(world->GetScriptLights().at("__blueprint/controller").intensity, 4000);
    Tick(0.5f);
    EXPECT_NEAR(world->GetScriptLights().at("__blueprint/controller").intensity, 2000, 0.01f);
    EXPECT_FALSE(State("controller")["failed"]);
}

TEST_F(ScriptGameplayTest, DisabledDestroyedAndSpawnedLightsRespectPlayLifetime)
{
    auto disabled = GameplayBlueprint("disabled", "ScriptFixtures.LightBlueprint"), destroyed = disabled;
    disabled["components"] = destroyed["components"] = LightTree();
    disabled["properties"]["Disable"] = true;
    destroyed["id"] = "destroyed"; destroyed["properties"]["DestroyOnTick"] = true;
    auto spawner = GameplayBlueprint("spawner", "ScriptFixtures.SpawnerBlueprint");
    auto child = Template(); child["components"] = LightTree();
    Prepare(Json::array({disabled, destroyed, spawner, child}));
    EXPECT_EQ(world->GetScriptLights().size(), 6u); // 预加载模板不发光。
    Play(); EXPECT_EQ(world->GetScriptLights().size(), 9u);
    Tick(); EXPECT_EQ(world->GetScriptLights().size(), 3u);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_EQ(world->GetScriptLights().size(), 6u);
    for (const auto& item : world->GetScriptLights()) EXPECT_EQ(item.first.find("~spawn."), std::string::npos);
}

TEST_F(ScriptGameplayTest, CameraUsesFullParentPositionAndRotationWithoutModifyingAuthorCamera)
{
    auto blueprint = GameplayBlueprint("camera", "ScriptFixtures.CameraBlueprint");
    blueprint["position"] = {10, 100, 20}; blueprint["rotation"] = {0, 45, 0}; blueprint["scale"] = {2, 3, 1};
    blueprint["components"] = Json::array({
        {{"type", "scene"}, {"id", "boom"}, {"rotation", {0, 30, 0}}, {"scale", {1, 2, 1}}},
        {{"type", "camera"}, {"id", "lens"}, {"parent", "boom"}, {"position", {0, 50, 300}},
            {"fov", 70}, {"near_clip", 2}, {"far_clip", 5000}, {"autoActivate", true}}});
    const auto authored = world->GetCamera();
    const auto position = authored->GetPosition();
    const auto view = authored->GetViewMatrix();
    Prepare(Json::array({blueprint}));
    EXPECT_EQ(world->GetRenderCamera(), authored);
    Play(); Tick();
    auto expected = blueprint;
    expected["position"] = State("camera")["position"];
    const auto actual = world->GetRenderCamera();
    ASSERT_NE(actual, authored);
    EXPECT_TRUE(actual->GetPosition().isApprox(ScriptComponentMatrix(expected, 1).block<3, 1>(0, 3)));
    EXPECT_TRUE(actual->GetForwardVector().isApprox(ScriptComponentRotation(expected, 1) * -Eigen::Vector3f::UnitZ()));
    EXPECT_FLOAT_EQ(actual->GetHFOV(), 70);
    EXPECT_TRUE(authored->GetPosition().isApprox(position));
    EXPECT_TRUE(authored->GetViewMatrix().isApprox(view));
    WorldSnapshotBuilder builder;
    const auto snapshot = builder.Build(*world, {});
    ASSERT_TRUE(snapshot.IsSuccess());
    EXPECT_TRUE(snapshot.Value().camera.position.isApprox(actual->GetPosition()));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    const auto paused = actual->GetPosition();
    Tick(1);
    EXPECT_TRUE(world->GetRenderCamera()->GetPosition().isApprox(paused));
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_EQ(world->GetRenderCamera(), authored);
}

TEST_F(ScriptGameplayTest, FailedCameraActivationRollsBackToOtherBlueprintsCamera)
{
    auto first = GameplayBlueprint("first");
    first["components"] = Json::array({{{"id", "lens"}, {"type", "camera"}, {"autoActivate", true},
        {"position", {0, 200, 400}}}});
    auto second = GameplayBlueprint("second", "ScriptFixtures.CameraBlueprint");
    second["components"] = Json::array({{{"id", "lens"}, {"type", "camera"}, {"position", {100, 100, 100}}}});
    second["properties"] = {{"Activate", true}, {"InvalidProjection", true}};
    Prepare(Json::array({first, second}));
    Play(); Tick();
    EXPECT_TRUE(State("second")["failed"]);
    EXPECT_FLOAT_EQ(State("second")["components"][0]["far_clip"], 10000);
    EXPECT_TRUE(world->GetRenderCamera()->GetPosition().isApprox(Eigen::Vector3f(0, 200, 400)));
}

TEST_F(ScriptGameplayTest, SwitchingCameraInvalidatesPassivePreviousOwnerInDeltaBatch)
{
    auto first = GameplayBlueprint("first");
    first["position"] = {0, 200, 0};
    first["components"] = Json::array({{{"id", "lens"}, {"type", "camera"}, {"autoActivate", true}}});
    auto second = GameplayBlueprint("second", "ScriptFixtures.CameraBlueprint");
    second["position"] = {0, 400, 0};
    second["properties"]["Activate"] = true;
    second["components"] = Json::array({{{"id", "lens"}, {"type", "camera"}}});
    Prepare(Json::array({first, second}));
    Play();
    EXPECT_NEAR(world->GetRenderCamera()->GetPosition().y(), 200, 1e-4f);
    Tick();
    EXPECT_FALSE(State("first")["components"][0]["active"].get<bool>());
    EXPECT_TRUE(State("second")["components"][0]["active"].get<bool>());
    EXPECT_NEAR(world->GetRenderCamera()->GetPosition().y(), 400, 1e-4f);
}

TEST_F(ScriptGameplayTest, DestroyedActiveCameraFallsBackToAuthoredView)
{
    auto blueprint = GameplayBlueprint("camera", "ScriptFixtures.CameraBlueprint");
    blueprint["components"] = Json::array({{{"id", "lens"}, {"type", "camera"}, {"autoActivate", true}}});
    blueprint["properties"] = {{"DestroyOnTick", true}};
    Prepare(Json::array({blueprint}));
    Play();
    EXPECT_NE(world->GetRenderCamera(), world->GetCamera());
    Tick();
    EXPECT_EQ(world->GetRenderCamera(), world->GetCamera());
}

TEST_F(ScriptGameplayTest, CameraTracksPostPhysicsPositionInSameFrame)
{
    auto blueprint = GameplayBlueprint("body");
    blueprint["position"] = {0, 1000, 0}; blueprint["physics"] = Body("dynamic");
    blueprint["components"] = Json::array({{{"id", "lens"}, {"type", "camera"},
        {"autoActivate", true}, {"position", {0, 100, 400}}}});
    Prepare(Json::array({blueprint}));
    Play(); Tick(1.0f / 30);
    EXPECT_LT(State("body")["position"][1].get<float>(), 1000);
    EXPECT_NEAR(world->GetRenderCamera()->GetPosition().y(), State("body")["position"][1].get<float>() + 100, 1e-3f);
}

TEST_F(ScriptGameplayTest, ComponentCallbackUpdatesChildMatrixAndStopRestoresLocalPose)
{
    auto blueprint = GameplayBlueprint("tree", "ScriptFixtures.ComponentBlueprint");
    blueprint["components"] = ComponentTree();
    Prepare(Json::array({blueprint}));
    Play(); Tick(1);
    auto changed = blueprint; changed["components"][0]["rotation"] = {0, 90, 0};
    EXPECT_TRUE(world->GetMeshObjects().at("__blueprint/tree/ball").model.isApprox(ScriptComponentMatrix(changed, 1), 1e-4f));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_TRUE(world->GetMeshObjects().at("__blueprint/tree/ball").model.isApprox(ScriptComponentMatrix(blueprint, 1), 1e-4f));
}

TEST_F(ScriptGameplayTest, ComponentCallbackFailureRollsBackLocalTransform)
{
    auto blueprint = GameplayBlueprint("tree", "ScriptFixtures.ComponentBlueprint");
    blueprint["components"] = ComponentTree(); blueprint["properties"]["Fail"] = true;
    Prepare(Json::array({blueprint}));
    Play(); Tick(1);
    EXPECT_TRUE(State("tree").at("failed"));
    EXPECT_TRUE(world->GetMeshObjects().at("__blueprint/tree/ball").model.isApprox(ScriptComponentMatrix(blueprint, 1)));
}

TEST_F(ScriptGameplayTest, SpawnReusesResourcesPublishesFreshSnapshotAndStopRemovesClones)
{
    auto spawner = GameplayBlueprint("spawner", "ScriptFixtures.SpawnerBlueprint");
    spawner["properties"]["Count"] = 2;
    Prepare(Json::array({spawner, Template()}));
    WorldSnapshotBuilder snapshots;
    auto before = snapshots.Build(*world, {});
    ASSERT_TRUE(before.IsSuccess()); EXPECT_TRUE(before.Value().meshDraws.empty());
    Play();
    ASSERT_EQ(world->GetScriptBlueprintStates().size(), 3u);
    EXPECT_EQ(State("~spawn.1").at("position")[1], 300);
    EXPECT_EQ(world->GetMeshObjects().at("__blueprint/~spawn.1/ball").meshKey, "shared-mesh");
    EXPECT_EQ(world->GetMeshObjects().at("__blueprint/~spawn.2/ball").materialKey, "shared-material");
    auto after = snapshots.Build(*world, {});
    ASSERT_TRUE(after.IsSuccess()); ASSERT_EQ(after.Value().meshDraws.size(), 2u);
    EXPECT_NE(after.Value().meshDraws[0].objectId, after.Value().meshDraws[1].objectId);
    EXPECT_TRUE(before.Value().meshDraws.empty());
    Tick();
    EXPECT_EQ(State("spawner").at("position")[0], 2);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_EQ(world->GetMeshObjects().size(), 1u);
    EXPECT_FALSE(world->GetMeshObjects().at("__blueprint/template/ball").visible);
    EXPECT_EQ(world->GetScriptBlueprints().size(), 1u);
}

TEST_F(ScriptGameplayTest, SpawnDuringTickAndDestroyUseDeferredStructuralCommands)
{
    auto spawner = GameplayBlueprint("spawner", "ScriptFixtures.SpawnerBlueprint");
    spawner["properties"] = {{"SpawnDuringTick", true}, {"DestroySpawn", true}};
    Prepare(Json::array({spawner, Template()}));
    Play(); ASSERT_EQ(world->GetScriptBlueprintStates().size(), 1u);
    Tick();
    EXPECT_TRUE(State("~spawn.1").at("destroyed"));
    EXPECT_EQ(world->GetMeshObjects().count("__blueprint/~spawn.1/ball"), 0u);
}

TEST_F(ScriptGameplayTest, FailedCallbackDropsQueuedSpawns)
{
    auto spawner = GameplayBlueprint("spawner", "ScriptFixtures.SpawnerBlueprint");
    spawner["properties"]["FailAfterSpawn"] = true;
    Prepare(Json::array({spawner, Template()}));
    Play();
    EXPECT_TRUE(State("spawner").at("failed"));
    EXPECT_EQ(world->GetScriptBlueprintStates().size(), 1u);
}

TEST_F(ScriptGameplayTest, UnknownSpawnAssetAndBudgetOverflowFailOnlyCallingBlueprint)
{
    auto missing = GameplayBlueprint("missing", "ScriptFixtures.SpawnerBlueprint");
    missing["properties"]["Asset"] = "Common/Blueprints/BP_missing.json";
    auto overflow = GameplayBlueprint("overflow", "ScriptFixtures.SpawnerBlueprint");
    overflow["properties"]["Count"] = 257;
    Prepare(Json::array({missing, overflow, Template()}));
    Play(); Tick();
    EXPECT_TRUE(State("missing").at("failed"));
    EXPECT_TRUE(State("overflow").at("failed"));
    EXPECT_EQ(world->GetScriptBlueprintStates().size(), 2u);
}

TEST_F(ScriptGameplayTest, StaleHandleRejectsRequestWithoutKillingOtherBlueprints)
{
    auto spawner = GameplayBlueprint("spawner", "ScriptFixtures.SpawnerBlueprint");
    spawner["properties"]["StaleHandle"] = true;
    Prepare(Json::array({spawner, Template()}));
    Play(); Tick();
    EXPECT_TRUE(State("spawner").at("failed"));
    EXPECT_FALSE(State("~spawn.1").at("destroyed"));
}

TEST_F(ScriptGameplayTest, PhysicsGravityPauseAndStopKeepAuthorStateSeparate)
{
    auto blueprint = GameplayBlueprint("fall");
    blueprint["position"] = {0, 300, 0}; blueprint["physics"] = Body("dynamic");
    Prepare(Json::array({blueprint})); Play();
    for (int frame = 0; frame < 30; ++frame) Tick();
    const auto y = State("fall").at("position")[1].get<float>();
    EXPECT_LT(y, 200); EXPECT_GT(y, 150);
    EXPECT_EQ(world->GetScriptBlueprints()[0].at("position")[1], 300);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick(1); EXPECT_EQ(State("fall").at("position")[1], y);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    Play(); EXPECT_EQ(State("fall").at("position")[1], 300);
}

TEST_F(ScriptGameplayTest, PhysicsCollisionEventsRunBeforeLateTickAndExitAfterTeleport)
{
    auto floor = GameplayBlueprint("floor");
    floor["position"] = {0, -50, 0}; floor["physics"] = Body("static");
    floor["physics"]["halfExtents"] = {1000, 50, 1000};
    auto blueprint = GameplayBlueprint("fall", "ScriptFixtures.PhysicsBlueprint");
    blueprint["position"] = {0, 200, 0}; blueprint["physics"] = Body("dynamic");
    blueprint["components"] = Json::array({{{"id", "events"}, {"type", "scene"}}});
    Prepare(Json::array({blueprint, floor})); Play();
    for (int frame = 0; frame < 120; ++frame) Tick();
    EXPECT_NEAR(State("fall").at("position")[1].get<float>(), 50, 0.2f);
    EXPECT_EQ(State("fall").at("components")[0].at("position")[0], 1);
    // 对接触中的地面作作者声明之外的托管位姿同步，验证下一物理帧产生 Exit。
    auto states = world->GetScriptBlueprintStates();
    for (auto& state : states) if (state.at("id") == "floor") state["position"] = {5000, -50, 0};
    world->SetScriptBlueprintStates(states);
    Tick();
    EXPECT_EQ(State("fall").at("components")[0].at("position")[1], 1);
}

TEST_F(ScriptGameplayTest, PhysicsImpulseIsConsumedOnceAndRootScaleCannotChangeInCallback)
{
    auto blueprint = GameplayBlueprint("body", "ScriptFixtures.PhysicsBlueprint");
    blueprint["physics"] = Body("dynamic"); blueprint["properties"] = {{"ImpulseX", 100}, {"Resize", true}};
    Prepare(Json::array({blueprint})); Play();
    Tick();
    EXPECT_NEAR(State("body").at("bodyVelocity")[0].get<float>(), 100, 0.01f);
    EXPECT_TRUE(State("body").at("failed"));
    EXPECT_EQ(State("body").at("scale"), Json::array({1, 1, 1}));
    Tick();
    EXPECT_NEAR(State("body").at("bodyVelocity")[0].get<float>(), 100, 0.01f);
}

TEST_F(ScriptGameplayTest, SpawnedRigidBodyUsesRequestedScaleAndFalls)
{
    auto spawner = GameplayBlueprint("spawner", "ScriptFixtures.SpawnerBlueprint");
    spawner["properties"]["SpawnScale"] = 2;
    Prepare(Json::array({spawner, Template(true)})); Play();
    EXPECT_EQ(world->GetScriptContext()->declarations.at("~spawn.1").at("scale"), Json::array({2, 2, 2}));
    Tick();
    EXPECT_LT(State("~spawn.1").at("position")[1].get<float>(), 300);
}

Json ProbeComponents()
{
    return Json::array({{{"id", "probe"}, {"type", "scene"}},
        {{"id", "timing"}, {"type", "scene"}}, {{"id", "events"}, {"type", "scene"}}});
}

TEST_F(ScriptGameplayTest, RaycastCallbackRunsAfterPhysicsBeforeLateTickAndIgnoresOwner)
{
    auto probe = GameplayBlueprint("probe", "ScriptFixtures.PhysicsProbeBlueprint"), target = GameplayBlueprint("target");
    probe["components"] = ProbeComponents(); probe["physics"] = Body("static");
    target["position"] = {400, 0, 0}; target["physics"] = Body("static");
    Prepare(Json::array({probe, target})); Play(); Tick();
    EXPECT_NEAR(State("probe")["components"][0]["position"][0].get<float>(), 350, 0.01f);
    EXPECT_NEAR(State("probe")["components"][0]["position"][1].get<float>(), -1, 0.001f);
    EXPECT_EQ(State("probe")["components"][0]["position"][2], 1);
    EXPECT_EQ(State("probe")["components"][1]["position"][0], 1);
    EXPECT_FALSE(State("probe")["failed"]);
}

TEST_F(ScriptGameplayTest, LateRaycastDeliversNextFrameAndFailedCallbackRollsBackTransform)
{
    auto late = GameplayBlueprint("late", "ScriptFixtures.PhysicsProbeBlueprint");
    auto faulty = GameplayBlueprint("faulty", "ScriptFixtures.PhysicsProbeBlueprint");
    late["components"] = ProbeComponents(); late["properties"]["RayInLate"] = true;
    faulty["components"] = ProbeComponents(); faulty["properties"]["FailRay"] = true;
    Prepare(Json::array({late, faulty})); Play(); Tick();
    EXPECT_EQ(State("late")["components"][0]["position"][0], 0);
    EXPECT_TRUE(State("faulty")["failed"]);
    EXPECT_EQ(State("faulty")["components"][0]["position"][0], 0);
    Tick();
    EXPECT_EQ(State("late")["components"][0]["position"][0], -1);
    EXPECT_EQ(State("late")["components"][1]["position"][0], 1);
}

TEST_F(ScriptGameplayTest, TriggerEnterAndExitStaySeparateFromCollisionCallbacks)
{
    auto sensor = GameplayBlueprint("sensor", "ScriptFixtures.PhysicsProbeBlueprint"), ball = GameplayBlueprint("ball");
    sensor["components"] = ProbeComponents(); sensor["physics"] = Body("static");
    sensor["physics"]["trigger"] = true;
    ball["physics"] = Body("kinematic");
    Prepare(Json::array({sensor, ball})); Play(); Tick();
    EXPECT_EQ(State("sensor")["components"][2]["position"], Json::array({1, 0, 0}));
    auto states = world->GetScriptBlueprintStates();
    for (auto& state : states) if (state["id"] == "ball") state["position"] = {1000, 0, 0};
    world->SetScriptBlueprintStates(states); Tick();
    EXPECT_EQ(State("sensor")["components"][2]["position"], Json::array({1, 1, 0}));
}

TEST_F(ScriptGameplayTest, ManagedCharacterMovesAndReadsGroundStateAfterPhysics)
{
    auto player = GameplayBlueprint("player", "ScriptFixtures.CharacterBlueprint"), floor = GameplayBlueprint("floor");
    player["components"] = ProbeComponents();
    player["position"] = {0, 100, 0};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    floor["position"] = {0, -50, 0}; floor["physics"] = Body("static");
    floor["physics"]["halfExtents"] = {2000, 50, 2000};
    Prepare(Json::array({player, floor})); Play();
    for (int frame = 0; frame < 60; ++frame) Tick();
    EXPECT_GT(State("player")["position"][0].get<float>(), 95);
    EXPECT_EQ(State("player")["components"][0]["position"][0], 1);
    EXPECT_FALSE(State("player")["failed"]);
}

TEST_F(ScriptGameplayTest, InvalidCharacterMoveIsolatedAndRolledBack)
{
    auto player = GameplayBlueprint("player", "ScriptFixtures.CharacterBlueprint");
    player["components"] = ProbeComponents(); player["properties"]["InvalidMove"] = true;
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    Prepare(Json::array({player})); Play(); Tick();
    EXPECT_TRUE(State("player")["failed"]);
    EXPECT_EQ(State("player")["position"][0], 0);
}

TEST_F(ScriptGameplayTest, TimersUsePlayClockFreezeWhilePausedAndSkipRepeatBacklog)
{
    auto blueprint = GameplayBlueprint("timer", "ScriptFixtures.TimerBlueprint");
    blueprint["properties"]["Repeat"] = true;
    Prepare(Json::array({blueprint})); Play(); Tick(0.05f);
    EXPECT_EQ(State("timer")["position"][0], 0);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    Tick(1); EXPECT_EQ(State("timer")["position"][0], 0);
    Play(); Tick(1);
    EXPECT_EQ(State("timer")["position"][0], 1);
    Tick(0.01f); EXPECT_EQ(State("timer")["position"][0], 1);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    Play(); Tick(0.05f);
    EXPECT_EQ(State("timer")["position"][0], 0);
}

TEST_F(ScriptGameplayTest, CancelledFaultedAndDestroyedOwnersLeaveNoTimerCallbacks)
{
    auto cancelled = GameplayBlueprint("cancelled", "ScriptFixtures.TimerBlueprint");
    auto faulted = GameplayBlueprint("faulted", "ScriptFixtures.TimerBlueprint");
    auto destroyed = GameplayBlueprint("destroyed", "ScriptFixtures.TimerBlueprint");
    cancelled["properties"]["Cancel"] = true; faulted["properties"]["Fail"] = true;
    destroyed["properties"]["DestroyOnTick"] = true;
    Prepare(Json::array({cancelled, faulted, destroyed})); Play();
    Tick(0.01f); Tick(1);
    EXPECT_EQ(State("cancelled")["position"][0], 0);
    EXPECT_EQ(State("faulted")["position"][0], 0);
    EXPECT_TRUE(State("faulted")["failed"]);
    EXPECT_TRUE(State("destroyed")["destroyed"]);
    EXPECT_EQ(State("destroyed")["position"][0], 0);
}

TEST_F(ScriptGameplayTest, TargetedEventsAndWorldQueriesUseContextHandlesAndValueSnapshots)
{
    auto sender = GameplayBlueprint("sender", "ScriptFixtures.MessageBlueprint");
    auto receiver = GameplayBlueprint("receiver", "ScriptFixtures.MessageBlueprint");
    sender["properties"] = {{"Sender", true}, {"Targeted", true}};
    receiver["position"] = {42, 0, 0};
    Prepare(Json::array({sender, receiver})); Play(); Tick();
    EXPECT_EQ(State("sender")["position"], Json::array({2, 0, 42}));
    EXPECT_EQ(State("receiver")["position"], Json::array({42, 1, 0}));
    EXPECT_FALSE(State("sender")["failed"]); EXPECT_FALSE(State("receiver")["failed"]);
}

TEST_F(ScriptGameplayTest, FailedCallbackDropsEventsWhileOtherSubscribersRemainUsable)
{
    auto sender = GameplayBlueprint("sender", "ScriptFixtures.MessageBlueprint");
    auto receiver = GameplayBlueprint("receiver", "ScriptFixtures.MessageBlueprint");
    sender["properties"] = {{"Sender", true}, {"Fail", true}};
    Prepare(Json::array({sender, receiver})); Play(); Tick();
    EXPECT_TRUE(State("sender")["failed"]);
    EXPECT_EQ(State("receiver")["position"][1], 0);
    EXPECT_FALSE(State("receiver")["failed"]);
}

TEST_F(ScriptGameplayTest, NamedInputReachesManagedCallbackAsValueState)
{
    Prepare(Json::array({GameplayBlueprint("input", "ScriptFixtures.NamedInputBlueprint")})); Play();
    InputActionState input;
    input.gameplay = {{"actions", {{"Use", {{"held", true}, {"pressed", true}, {"released", false}}}}},
        {"axes", {{"Custom", 3}}}};
    Tick(1.0f / 60, input);
    EXPECT_EQ(State("input")["position"], Json::array({3, 1, 1}));
}

TEST_F(ScriptGameplayTest, AsyncBlueprintCompletionSpawnsWithoutRestartAndLeasePinsUntilInstanceDestroyed)
{
    auto owner = GameplayBlueprint("loader", "ScriptFixtures.AsyncAssetsBlueprint");
    owner["properties"] = {{"UnloadOnComplete", true}, {"DestroySpawnFrame", 3}};
    Prepare(Json::array({owner})); Play();
    const auto context = world->GetScriptContext();
    auto requests = runtime.TakeAssetRequests(*world);
    ASSERT_EQ(requests.size(), 1u);
    auto loaded = GameplayBlueprint("~dynamic.test");
    loaded["__templatePath"] = "Common/Blueprints/BP_dynamic.json";
    loaded["components"] = Json::array({{{"id", "mesh"}, {"type", "mesh"}, {"modelPath", "Common/Meshes/SM_test.json"}}});
    WorldMeshObject section;
    section.debugName = section.sceneObjectIdentity = "__blueprint/~dynamic.test/mesh";
    section.meshKey = "shared-mesh"; section.materialKey = "shared-material";
    world->CommitScriptTemplateExtension(world->PrepareScriptTemplateExtension(
        "Common/Blueprints/BP_dynamic.json", "test-digest", loaded, {section}, {}));
    ASSERT_TRUE(runtime.CompleteAssetLoad(*world, requests[0]["id"], requests[0]["path"], loaded));
    EXPECT_EQ(world->GetScriptContext(), context);
    Tick();
    ASSERT_EQ(world->GetScriptBlueprintStates().size(), 2u);
    EXPECT_FALSE(State("loader")["failed"]);
    EXPECT_EQ(State("loader")["position"][0], 1);
    EXPECT_EQ(State("~spawn.1")["position"], Json::array({0, 200, 0}));
    EXPECT_TRUE(world->GetMeshObjects().at("__blueprint/~spawn.1/mesh").visible);
    EXPECT_TRUE(runtime.CollectUnusedBlueprintAssets(*world).empty());
    Tick(); EXPECT_TRUE(runtime.CollectUnusedBlueprintAssets(*world).empty());
    Tick();
    EXPECT_TRUE(State("~spawn.1")["destroyed"]);
    EXPECT_EQ(runtime.CollectUnusedBlueprintAssets(*world),
        std::vector<std::string>({"Common/Blueprints/BP_dynamic.json"}));
    world->CommitScriptTemplateExtension(world->PrepareScriptTemplateRemoval("Common/Blueprints/BP_dynamic.json"));
    runtime.ForgetBlueprintAsset(*world, "Common/Blueprints/BP_dynamic.json");
    Tick(); EXPECT_TRUE(runtime.CollectUnusedBlueprintAssets(*world).empty());
    EXPECT_EQ(world->GetScriptContext(), context);
    EXPECT_FALSE(runtime.IsEditing());
}

TEST_F(ScriptGameplayTest, AsyncAssetRequestFailureAndOwnerDestroyCancelOnlyTheirRequests)
{
    auto failed = GameplayBlueprint("failed", "ScriptFixtures.AsyncAssetsBlueprint");
    auto destroyed = GameplayBlueprint("destroyed", "ScriptFixtures.AsyncAssetsBlueprint");
    failed["properties"]["FailRequest"] = true; destroyed["properties"]["DestroyOwnerFrame"] = 1;
    Prepare(Json::array({failed, destroyed})); Play();
    auto requests = runtime.TakeAssetRequests(*world);
    ASSERT_EQ(requests.size(), 1u); EXPECT_EQ(requests[0]["owner"], "destroyed");
    EXPECT_TRUE(State("failed")["failed"]);
    Tick();
    EXPECT_EQ(runtime.TakeAssetCancellations(*world), Json::array({requests[0]["id"]}));
    EXPECT_FALSE(runtime.CompleteAssetLoad(*world, requests[0]["id"], requests[0]["path"], Json::object(), "cancelled"));
    EXPECT_TRUE(State("destroyed")["destroyed"]);
}

TEST_F(ScriptGameplayTest, CompletionAfterOwnerDisabledStillPublishesCancellationInDelta)
{
    Prepare(Json::array({GameplayBlueprint("loader", "ScriptFixtures.DeltaDisabledAssetBlueprint")}));
    Play();
    const auto requests = runtime.TakeAssetRequests(*world);
    ASSERT_EQ(requests.size(), 1u);
    Tick();
    ASSERT_FALSE(State("loader")["enabled"].get<bool>());
    auto loaded = GameplayBlueprint("~dynamic.disabled");
    const std::string path = "Common/Blueprints/BP_dynamic.json";
    loaded["__templatePath"] = path;
    world->CommitScriptTemplateExtension(world->PrepareScriptTemplateExtension(path, "disabled-digest", loaded, {}, {}));
    ASSERT_TRUE(runtime.CompleteAssetLoad(*world, requests[0]["id"], path, loaded));
    Tick();
    EXPECT_FALSE(runtime.IsEditing());
    EXPECT_FALSE(State("loader")["failed"].get<bool>());
    EXPECT_TRUE(world->GetScriptContext()->assetLoads.empty());
    EXPECT_EQ(runtime.TakeAssetCancellations(*world), Json::array({requests[0]["id"]}));
    EXPECT_EQ(runtime.CollectUnusedBlueprintAssets(*world), std::vector<std::string>({path}));
}

TEST_F(ScriptGameplayTest, AsyncCancellationReusesFullRequestBudgetWithinSameCallback)
{
    auto owner = GameplayBlueprint("loader", "ScriptFixtures.AsyncAssetsBlueprint");
    owner["properties"] = {{"LoadCount", 256}, {"CancelOnTick", true}, {"ReloadAfterCancel", true}};
    Prepare(Json::array({owner})); Play();
    auto initial = runtime.TakeAssetRequests(*world);
    ASSERT_EQ(initial.size(), 256u);
    Tick();
    ASSERT_FALSE(State("loader")["failed"]); EXPECT_FALSE(runtime.IsEditing());
    auto replacement = runtime.TakeAssetRequests(*world);
    ASSERT_EQ(replacement.size(), 1u);
    EXPECT_NE(initial[0]["id"], replacement[0]["id"]);
    EXPECT_EQ(runtime.TakeAssetCancellations(*world).size(), 256u);
    EXPECT_EQ(world->GetScriptContext()->assetLoads.size(), 1u);
}

TEST_F(ScriptGameplayTest, AsyncAssetFailureDeliversStructuredErrorAndCanBeCancelledBeforeCompletion)
{
    auto cancelled = GameplayBlueprint("cancelled", "ScriptFixtures.AsyncAssetsBlueprint");
    auto failure = GameplayBlueprint("failure", "ScriptFixtures.AsyncAssetsBlueprint");
    cancelled["properties"]["CancelOnTick"] = true;
    Prepare(Json::array({cancelled, failure})); Play();
    auto requests = runtime.TakeAssetRequests(*world);
    ASSERT_EQ(requests.size(), 2u);
    Tick();
    EXPECT_FALSE(runtime.CompleteAssetLoad(*world, requests[0]["id"], requests[0]["path"], Json::object(), "cancelled"));
    ASSERT_TRUE(runtime.CompleteAssetLoad(*world, requests[1]["id"], requests[1]["path"], Json::object(), "missing asset"));
    Tick();
    EXPECT_EQ(State("cancelled")["position"], Json::array({0, 0, 0}));
    EXPECT_EQ(State("failure")["position"], Json::array({0, 1, 0}));
    EXPECT_FALSE(State("failure")["failed"]);
}

TEST_F(ScriptGameplayTest, AsyncCompletionFailureRollsBackSpawnAndReleasesSuccessfulLease)
{
    auto owner = GameplayBlueprint("loader", "ScriptFixtures.AsyncAssetsBlueprint");
    owner["properties"]["FailCompletion"] = true;
    Prepare(Json::array({owner})); Play();
    auto request = runtime.TakeAssetRequests(*world).at(0);
    auto loaded = GameplayBlueprint("~dynamic.test");
    loaded["__templatePath"] = "Common/Blueprints/BP_dynamic.json";
    world->SetScriptTemplates({{"Common/Blueprints/BP_dynamic.json", loaded}});
    ASSERT_TRUE(runtime.CompleteAssetLoad(*world, request["id"], request["path"], loaded));
    Tick();
    EXPECT_EQ(world->GetScriptBlueprintStates().size(), 1u);
    EXPECT_TRUE(State("loader")["failed"]); EXPECT_EQ(State("loader")["position"][0], 0);
    EXPECT_EQ(runtime.CollectUnusedBlueprintAssets(*world),
        std::vector<std::string>({"Common/Blueprints/BP_dynamic.json"}));
}

TEST_F(ScriptGameplayTest, AsyncResultFromPreviousPlayCannotCompleteNewRequest)
{
    auto owner = GameplayBlueprint("loader", "ScriptFixtures.AsyncAssetsBlueprint");
    Prepare(Json::array({owner})); Play();
    auto request = runtime.TakeAssetRequests(*world).at(0);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    Play();
    // 同一路径在新 Play 中拥有全新请求身份，旧 worker 的延迟结果不能满足它。
    EXPECT_NE(world->GetScriptContext(), nullptr);
    ASSERT_FALSE(runtime.CompleteAssetLoad(*world, request["id"], request["path"], Json::object()));
}

TEST_F(ScriptGameplayTest, InvalidCompoundRuntimeEditsRollbackOnlyTheirBlueprintCallbacks)
{
    auto removed = GameplayBlueprint("removed", "ScriptFixtures.ColliderMutatorBlueprint"), resized = removed;
    removed["components"] = Json::array({{{"id", "collider"}, {"type", "scene"},
        {"collision", {{"shape", "sphere"}, {"radius", 20}}}}});
    removed["physics"] = {{"shape", "compound"}};
    resized = removed; resized["id"] = "resized";
    removed["properties"]["RemoveCollider"] = true; resized["properties"]["Nonuniform"] = true;
    Prepare(Json::array({removed, resized})); Play(); Tick();
    EXPECT_TRUE(State("removed")["failed"]); EXPECT_TRUE(State("resized")["failed"]);
    EXPECT_EQ(State("removed")["components"].size(), 1u);
    EXPECT_EQ(State("resized")["components"][0]["scale"], Json::array({1, 1, 1}));
    EXPECT_EQ(State("removed")["position"][0], 0); EXPECT_EQ(State("resized")["position"][0], 0);
    EXPECT_FALSE(runtime.IsEditing());
}

TEST_F(ScriptGameplayTest, PlaygroundCameraSphereProbeRetractsBeforeWall)
{
    auto player = GameplayBlueprint("player", "Gameplay.ThirdPersonCharacterBlueprint");
    player["position"] = {0, 90, 300};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["components"] = Json::array({{{"id", "camera"}, {"type", "camera"}, {"position", {0, 110, 400}}}});
    auto floor = GameplayBlueprint("floor"); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    auto wall = GameplayBlueprint("wall"); wall["position"] = {0, 200, 500};
    wall["physics"] = {{"shape", "box"}, {"halfExtents", {200, 200, 20}}};
    InputActionState input;
    GameplayInputMap map; input.gameplay = map.Sample({}, true, true);
    Prepare(Json::array({floor, player, wall})); Play(); Tick(1.0f / 60, input);
    ASSERT_FALSE(State("player")["failed"]);
    const float arm = State("player")["components"][0]["position"][2].get<float>();
    EXPECT_GT(arm, 150); EXPECT_LT(arm, 180);
    EXPECT_LT(world->GetRenderCamera()->GetPosition().z(), 480 - 12);
}

TEST_F(ScriptGameplayTest, PlaygroundCameraProbeConvertsScaledArmBetweenLocalAndWorldCentimeters)
{
    auto player = GameplayBlueprint("player", "Gameplay.ThirdPersonCharacterBlueprint");
    player["position"] = {0, 180, 300}; player["scale"] = {2, 2, 2};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["components"] = Json::array({{{"id", "camera"}, {"type", "camera"}, {"position", {0, 110, 400}}}});
    auto floor = GameplayBlueprint("floor"); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    auto wall = GameplayBlueprint("wall"); wall["position"] = {0, 400, 600};
    wall["physics"] = {{"shape", "box"}, {"halfExtents", {200, 150, 20}}};
    InputActionState input;
    GameplayInputMap map; input.gameplay = map.Sample({}, true, true);
    Prepare(Json::array({floor, player, wall})); Play(); Tick(1.0f / 60, input);
    ASSERT_FALSE(State("player")["failed"]);
    EXPECT_NEAR(State("player")["components"][0]["position"][2].get<float>(), 133, 0.5f);
    EXPECT_NEAR(world->GetRenderCamera()->GetPosition().z(), 566, 1);
}

TEST_F(ScriptGameplayTest, PlaygroundCameraProbeUsesPostPhysicsPoseWhenBackingTowardWall)
{
    auto player = GameplayBlueprint("player", "Gameplay.ThirdPersonCharacterBlueprint");
    player["position"] = {0, 90, 300};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["components"] = Json::array({{{"id", "camera"}, {"type", "camera"}, {"position", {0, 110, 400}}}});
    auto floor = GameplayBlueprint("floor"); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    auto wall = GameplayBlueprint("wall"); wall["position"] = {0, 200, 500};
    wall["physics"] = {{"shape", "box"}, {"halfExtents", {200, 200, 20}}};
    InputActionState input;
    GameplayInputMap map; input.gameplay = map.Sample({"S"}, true, true);
    Prepare(Json::array({floor, player, wall})); Play(); Tick(1.0f / 60, input);
    ASSERT_FALSE(State("player")["failed"]);
    EXPECT_GT(State("player")["position"][2].get<float>(), 305);
    EXPECT_NEAR(world->GetRenderCamera()->GetPosition().z(), 466, 0.5f);
    EXPECT_LT(world->GetRenderCamera()->GetPosition().z(), 480 - 12);
}

TEST_F(ScriptGameplayTest, PlaygroundCharacterMovesJumpsAndCameraFollowsWithoutAuthorMutation)
{
    auto player = GameplayBlueprint("player", "Gameplay.ThirdPersonCharacterBlueprint");
    player["position"] = {0, 90, 300};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["components"] = Json::array({{{"id", "camera"}, {"type", "camera"}, {"position", {0, 110, 400}}}});
    auto floor = GameplayBlueprint("floor"); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    Prepare(Json::array({floor, player})); Play();
    InputActionState input;
    GameplayInputMap map; map.Sample({}, true, true);
    for (int frame = 0; frame < 30; ++frame)
    {
        input.gameplay = map.Sample({"W"}, true, true);
        Tick(1.0f / 60, input);
    }
    ASSERT_FALSE(State("player")["failed"]);
    EXPECT_LT(State("player")["position"][2].get<float>(), 150);
    EXPECT_NEAR(world->GetRenderCamera()->GetPosition().z(), State("player")["position"][2].get<float>() + 400, 1e-3f);
    const auto before = State("player")["position"][1].get<float>();
    input.gameplay = map.Sample({"Space"}, true, true);
    Tick(1.0f / 60, input);
    EXPECT_GT(State("player")["position"][1].get<float>(), before);
    EXPECT_EQ(world->GetScriptBlueprints()[1]["position"], player["position"]);
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_EQ(world->GetRenderCamera(), world->GetCamera());
}

TEST_F(ScriptGameplayTest, PlaygroundTriggerOnlySpawnsForCharacterInteractingInsideZone)
{
    auto player = GameplayBlueprint("player", "Gameplay.ThirdPersonCharacterBlueprint");
    player["position"] = {0, 90, 0};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["components"] = Json::array({{{"id", "camera"}, {"type", "camera"}}});
    auto zone = GameplayBlueprint("zone", "Gameplay.SpawnTriggerBlueprint");
    zone["position"] = {0, 90, 0};
    zone["physics"] = {{"shape", "box"}, {"halfExtents", {150, 150, 150}}, {"trigger", true}};
    zone["properties"]["SpawnAsset"] = "Common/Blueprints/BP_child.json";
    auto floor = GameplayBlueprint("floor"); floor["position"] = {0, -50, 0};
    floor["physics"] = {{"shape", "box"}, {"halfExtents", {2000, 50, 2000}}};
    Prepare(Json::array({floor, player, zone, Template(true)})); Play();
    InputActionState input;
    GameplayInputMap map; map.Sample({}, true, true);
    input.gameplay = map.Sample({}, true, true);
    Tick(1.0f / 60, input);
    input.gameplay = map.Sample({"F"}, true, true);
    Tick(1.0f / 60, input);
    ASSERT_EQ(world->GetScriptBlueprintStates().size(), 4u);
    for (int frame = 0; frame < 60; ++frame)
    {
        input.gameplay = map.Sample({"W"}, true, true);
        Tick(1.0f / 60, input);
    }
    ASSERT_LT(State("player")["position"][2].get<float>(), -250);
    input.gameplay = map.Sample({"F"}, true, true);
    Tick(1.0f / 60, input);
    EXPECT_EQ(world->GetScriptBlueprintStates().size(), 4u);
    EXPECT_FALSE(State("zone")["failed"]);
    EXPECT_FALSE(State("player")["failed"]);
}

TEST(ScriptPhysicsTest, StaticAndKinematicBodiesKeepPoseWhileDynamicSphereFalls)
{
    auto stationary = GameplayBlueprint("static"), moving = GameplayBlueprint("kinematic"), falling = GameplayBlueprint("sphere");
    stationary["physics"] = Body("static"); moving["physics"] = Body("kinematic");
    moving["position"] = {1000, 0, 0};
    falling["position"] = {2000, 300, 0};
    falling["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 20}};
    Json declarations{{"static", stationary}, {"kinematic", moving}, {"sphere", falling}};
    Json states = Json::array({stationary, moving, falling});
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 1.0f / 60);
    states[1]["position"] = {1000, 100, 0};
    physics.Step(states, declarations, 1.0f / 60);
    EXPECT_EQ(states[0].at("position"), stationary.at("position"));
    EXPECT_EQ(states[1].at("position")[1], 100);
    EXPECT_LT(states[2].at("position")[1].get<float>(), 300);
}

TEST(ScriptPhysicsTest, VelocityAndImpulseCommandsAreNotReplayedByZeroStep)
{
    auto blueprint = GameplayBlueprint("body"); blueprint["physics"] = Body("dynamic");
    Json states = Json::array({blueprint}); states[0]["impulse"] = {100, 0, 0};
    ScriptPhysicsWorld physics;
    physics.Step(states, {{"body", blueprint}}, 0);
    ASSERT_FALSE(states[0].contains("impulse"));
    physics.Step(states, {{"body", blueprint}}, 0);
    EXPECT_FLOAT_EQ(states[0].at("bodyVelocity")[0].get<float>(), 100);
    states[0]["velocity"] = {50, 0, 0};
    physics.Step(states, {{"body", blueprint}}, 1.0f / 60);
    ASSERT_FALSE(states[0].contains("velocity"));
    physics.Step(states, {{"body", blueprint}}, 0);
    EXPECT_FLOAT_EQ(states[0].at("bodyVelocity")[0].get<float>(), 50);
}

TEST(ScriptPhysicsTest, TriggerOverlapsWithoutBlockingAndRemovalClearsPairs)
{
    auto sensor = GameplayBlueprint("sensor"), ball = GameplayBlueprint("ball");
    sensor["physics"] = Body("static"); sensor["physics"]["trigger"] = true;
    ball["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 20}};
    Json states = Json::array({sensor, ball}), declarations{{"sensor", sensor}, {"ball", ball}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 1.0f / 60);
    EXPECT_TRUE(physics.Contacts().empty());
    EXPECT_EQ(physics.Triggers(), Json::array({Json::array({"ball", "sensor"})}));
    for (int frame = 0; frame < 60; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_LT(states[1]["position"][1].get<float>(), -400);
    EXPECT_TRUE(physics.Triggers().empty());
    states[1]["position"] = {0, 0, 0}; states[1]["velocity"] = {0, 0, 0};
    physics.Step(states, declarations, 0);
    ASSERT_EQ(physics.Triggers().size(), 1u);
    states[0]["destroyed"] = true;
    physics.Step(states, declarations, 0);
    EXPECT_TRUE(physics.Triggers().empty());
}

TEST(ScriptPhysicsTest, RaycastReturnsClosestBlueprintCentimetersNormalAndOptionalTriggers)
{
    auto owner = GameplayBlueprint("owner"), sensor = GameplayBlueprint("sensor"), target = GameplayBlueprint("target");
    for (auto* blueprint : {&owner, &sensor, &target}) (*blueprint)["physics"] = Body("static");
    sensor["position"] = {200, 0, 0}; sensor["physics"]["trigger"] = true;
    target["position"] = {400, 0, 0};
    Json states = Json::array({owner, sensor, target});
    ScriptPhysicsWorld physics;
    physics.Step(states, {{"owner", owner}, {"sensor", sensor}, {"target", target}}, 0);
    Json query{{"id", 7}, {"owner", "owner"}, {"from", {0, 0, 0}}, {"to", {1000, 0, 0}}};
    auto hit = physics.Raycast(Json::array({query})).at(0);
    EXPECT_EQ(hit["blueprint"], "target");
    EXPECT_NEAR(hit["point"][0].get<float>(), 350, 0.01f);
    EXPECT_NEAR(hit["distance"].get<float>(), 350, 0.01f);
    EXPECT_NEAR(hit["normal"][0].get<float>(), -1, 0.001f);
    query["includeTriggers"] = true;
    EXPECT_EQ(physics.Raycast(Json::array({query}))[0]["blueprint"], "sensor");
    query["to"] = {0, 1000, 0};
    EXPECT_FALSE(physics.Raycast(Json::array({query}))[0].contains("blueprint"));
}

TEST(ScriptPhysicsTest, GroundProbeFiltersOwnerTriggersAndBothMasksWithoutSeeingThroughDynamicSupport)
{
    auto owner = GameplayBlueprint("owner"), sensor = GameplayBlueprint("sensor");
    auto excludedLayer = GameplayBlueprint("excluded-layer"), excludedMask = GameplayBlueprint("excluded-mask");
    auto floor = GameplayBlueprint("floor"), moving = GameplayBlueprint("moving");
    for (auto* blueprint : {&owner, &sensor, &excludedLayer, &excludedMask, &floor, &moving})
    {
        (*blueprint)["physics"] = Body("static");
        (*blueprint)["physics"]["halfExtents"] = {20, 20, 20};
        (*blueprint)["physics"]["collisionLayer"] = 3;
        (*blueprint)["physics"]["collisionMask"] = 2;
    }
    owner["position"] = {0, 270, 0};
    owner["physics"]["collisionLayer"] = 1; owner["physics"]["collisionMask"] = 10;
    sensor["position"] = {0, 220, 0}; sensor["physics"]["trigger"] = true;
    excludedLayer["position"] = {0, 170, 0}; excludedLayer["physics"]["collisionLayer"] = 2;
    excludedMask["position"] = {0, 120, 0}; excludedMask["physics"]["collisionMask"] = 1;
    floor["position"] = {0, -20, 0};
    moving["position"] = {300, 60, 0}; moving["physics"]["motion"] = "dynamic";
    Json states = Json::array({owner, sensor, excludedLayer, excludedMask, floor, moving});
    Json declarations{{"owner", owner}, {"sensor", sensor}, {"excluded-layer", excludedLayer},
        {"excluded-mask", excludedMask}, {"floor", floor}, {"moving", moving}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 0);
    const Eigen::Vector3f origin(0, 320, 0), displacement(0, -400, 0);
    const auto hit = physics.ProbeGround("owner", origin, displacement);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.supportIdentity, "floor");
    EXPECT_NEAR(hit.point.norm(), 0, 0.01f);
    EXPECT_TRUE(hit.normal.isApprox(Eigen::Vector3f::UnitY(), 0.001f));

    // LateTick 式零时步同步后，最近的动态遮挡必须产生 miss，不能继续看穿至静态地板。
    states[5]["position"] = {0, 60, 0};
    physics.Step(states, declarations, 0);
    const auto blocked = physics.ProbeGround("owner", origin, displacement);
    EXPECT_FALSE(blocked.hit);
    EXPECT_TRUE(blocked.supportIdentity.empty());
}

TEST(ScriptPhysicsTest, RigidBodyAccumulatesInternalSubstepsAndCapsLargeFrames)
{
    auto blueprint = GameplayBlueprint("body"); blueprint["physics"] = Body("dynamic");
    Json states = Json::array({blueprint}), declarations{{"body", blueprint}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 1.0f / 120);
    EXPECT_FLOAT_EQ(states[0]["position"][1].get<float>(), 0); // 不足一个 1/60 子步，不推进。
    physics.Step(states, declarations, 1.0f / 120);
    EXPECT_LT(states[0]["position"][1].get<float>(), 0);       // 两帧补满一个内部子步。
    // 大 dt 只追赶物理世界的 16 个 1/60 子步，不模拟完整的 10 秒自由落体。
    physics.Step(states, declarations, 10);
    const float capped = states[0]["position"][1].get<float>();
    EXPECT_GT(capped, -100);
    EXPECT_LT(capped, -20);
    // 上限命中时丢弃整步多余量，下一帧只前进一个子步。
    physics.Step(states, declarations, 1.0f / 60);
    const float after = states[0]["position"][1].get<float>();
    EXPECT_LT(after, capped);
    EXPECT_GT(after, capped - 10);
    const auto pose = states[0]["position"];
    physics.Step(states, declarations, 0);
    EXPECT_EQ(states[0]["position"], pose);
}

TEST(ScriptPhysicsTest, RayStartingInsideSphereReturnsFiniteOpposingNormal)
{
    auto blueprint = GameplayBlueprint("sphere");
    blueprint["physics"] = {{"shape", "sphere"}, {"radius", 50}};
    Json states = Json::array({blueprint});
    ScriptPhysicsWorld physics;
    physics.Step(states, {{"sphere", blueprint}}, 0);
    Json query{{"id", 1}, {"owner", "none"}, {"from", {0, 0, 0}}, {"to", {100, 0, 0}}};
    const auto hit = physics.Raycast(Json::array({query}))[0];
    EXPECT_EQ(hit["blueprint"], "sphere");
    EXPECT_FLOAT_EQ(hit["distance"].get<float>(), 0);
    EXPECT_EQ(hit["normal"], Json::array({-1, 0, 0}));
}

TEST(ScriptPhysicsTest, KinematicVelocityPublishesPoseAndExplicitTeleportKeepsIdentity)
{
    auto blueprint = GameplayBlueprint("body"); blueprint["physics"] = Body("kinematic");
    Json states = Json::array({blueprint}), declarations{{"body", blueprint}};
    states[0]["velocity"] = {60, 0, 0};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 1.0f / 60);
    EXPECT_NEAR(states[0]["position"][0].get<float>(), 1, 0.001f);
    states[0]["position"] = {1000, 0, 0}; states[0]["velocity"] = {0, 0, 0};
    physics.Step(states, declarations, 0);
    EXPECT_NEAR(states[0]["position"][0].get<float>(), 1000, 0.001f);
}

TEST(ScriptPhysicsTest, MassControlsImpulseAndRotationPreservesBlueprintMatrix)
{
    auto blueprint = GameplayBlueprint("body"); blueprint["physics"] = Body("dynamic");
    blueprint["physics"]["mass"] = 2; blueprint["rotation"] = {23, -47, 61};
    Json states = Json::array({blueprint});
    states[0]["impulse"] = {200, 0, 0};
    ScriptPhysicsWorld physics;
    physics.Step(states, {{"body", blueprint}}, 0);
    EXPECT_NEAR(states[0]["bodyVelocity"][0].get<float>(), 100, 0.001f);
    EXPECT_TRUE(ScriptTransformMatrix(states[0]).isApprox(ScriptTransformMatrix(blueprint), 1e-5f));
}

TEST(ScriptPhysicsTest, SleepingContactsRemainUntilTeleportAndDisable)
{
    auto floor = GameplayBlueprint("floor"), ball = GameplayBlueprint("ball");
    floor["physics"] = Body("static"); floor["position"] = {0, -50, 0};
    floor["physics"]["halfExtents"] = {1000, 50, 1000};
    ball["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 20}};
    ball["position"] = {0, 100, 0};
    Json states = Json::array({floor, ball}), declarations{{"floor", floor}, {"ball", ball}};
    ScriptPhysicsWorld physics;
    for (int frame = 0; frame < 300; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_EQ(physics.Contacts().size(), 1u);
    for (int frame = 0; frame < 60; ++frame)
    {
        physics.Step(states, declarations, 1.0f / 60);
        ASSERT_EQ(physics.Contacts().size(), 1u);
    }
    states[0]["position"] = {5000, -50, 0};
    const float previousY = states[1]["position"][1].get<float>();
    physics.Step(states, declarations, 1.0f / 60);
    EXPECT_TRUE(physics.Contacts().empty());
    EXPECT_LT(states[1]["position"][1].get<float>(), previousY);
    states[1]["enabled"] = false;
    physics.Step(states, declarations, 0);
    Json query{{"id", 1}, {"owner", "none"}, {"from", {0, 1000, 0}}, {"to", {0, -1000, 0}}};
    EXPECT_FALSE(physics.Raycast(Json::array({query}))[0].contains("blueprint"));
}

TEST(ScriptPhysicsTest, WorldsShareRegistrationButKeepBodiesAndLifetimeIndependent)
{
    auto blueprint = GameplayBlueprint("body"); blueprint["physics"] = Body("dynamic");
    Json declarations{{"body", blueprint}}, a = Json::array({blueprint}), b = a;
    ScriptPhysicsWorld first;
    first.Step(a, declarations, 1.0f / 60);
    {
        ScriptPhysicsWorld second;
        second.Step(b, declarations, 0);
        EXPECT_FLOAT_EQ(b[0]["position"][1].get<float>(), 0);
    }
    first.Step(a, declarations, 1.0f / 60);
    EXPECT_LT(a[0]["position"][1].get<float>(), b[0]["position"][1].get<float>());
}

TEST(ScriptPhysicsTest, CapsuleCharacterLandsMovesStopsAtWallAndJumpsOnce)
{
    auto floor = GameplayBlueprint("floor"), character = GameplayBlueprint("player"), wall = GameplayBlueprint("wall");
    floor["physics"] = Body("static"); floor["position"] = {0, -50, 0};
    floor["physics"]["halfExtents"] = {2000, 50, 2000};
    wall["physics"] = Body("static"); wall["physics"]["halfExtents"] = {20, 500, 1000};
    wall["position"] = {250, 500, 0};
    character["physics"] = {{"motion", "character"}, {"shape", "capsule"},
        {"radius", 30}, {"halfHeight", 60}, {"stepHeight", 20}, {"jumpSpeed", 500}};
    character["position"] = {0, 200, 0};
    Json states = Json::array({floor, character, wall});
    Json declarations{{"floor", floor}, {"player", character}, {"wall", wall}};
    ScriptPhysicsWorld physics;
    for (int frame = 0; frame < 100; ++frame) physics.Step(states, declarations, 1.0f / 60);
    ASSERT_TRUE(states[1]["grounded"].get<bool>());
    EXPECT_NEAR(states[1]["position"][1].get<float>(), 90, 1);
    states[1]["walkVelocity"] = {300, 0, 0};
    for (int frame = 0; frame < 120; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_GT(states[1]["position"][0].get<float>(), 190);
    EXPECT_LT(states[1]["position"][0].get<float>(), 202);
    states[1]["walkVelocity"] = {0, 0, 0}; states[1]["jump"] = true;
    physics.Step(states, declarations, 0); // LateTick 指令保留到下一有效时步。
    for (int frame = 0; frame < 10; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_GT(states[1]["position"][1].get<float>(), 140);
    EXPECT_FALSE(states[1]["grounded"].get<bool>());
    for (int frame = 0; frame < 150; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_TRUE(states[1]["grounded"].get<bool>());
    EXPECT_NEAR(states[1]["position"][1].get<float>(), 90, 1);
}

TEST(ScriptPhysicsTest, CharacterTeleportRefreshesGroundAndProxyParticipatesInQueries)
{
    auto floor = GameplayBlueprint("floor"), player = GameplayBlueprint("player"), sensor = GameplayBlueprint("sensor");
    floor["physics"] = Body("static"); floor["position"] = {0, -50, 0};
    floor["physics"]["halfExtents"] = {2000, 50, 2000};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["position"] = {0, 90, 0};
    sensor["physics"] = Body("static"); sensor["physics"]["trigger"] = true;
    sensor["position"] = {0, 90, 0};
    Json states = Json::array({player, floor, sensor});
    Json declarations{{"floor", floor}, {"player", player}, {"sensor", sensor}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 0);
    EXPECT_TRUE(states[0]["grounded"].get<bool>());
    EXPECT_EQ(physics.Triggers(), Json::array({Json::array({"player", "sensor"})}));
    Json query{{"id", 1}, {"owner", "none"}, {"from", {-200, 90, 0}}, {"to", {200, 90, 0}}};
    EXPECT_EQ(physics.Raycast(Json::array({query}))[0]["blueprint"], "player");
    states[0]["position"] = {500, 500, 0}; states[0]["jump"] = true;
    physics.Step(states, declarations, 0);
    EXPECT_FALSE(states[0]["grounded"].get<bool>());
    EXPECT_TRUE(physics.Triggers().empty());
    physics.Step(states, declarations, 1.0f / 60);
    EXPECT_LT(states[0]["position"][1].get<float>(), 500);
}

TEST(ScriptPhysicsTest, CharacterYawOnlyPreservesAchievedVelocity)
{
    auto floor = GameplayBlueprint("floor"), player = GameplayBlueprint("player");
    floor["physics"] = Body("static"); floor["position"] = {0, -50, 0};
    floor["physics"]["halfExtents"] = {2000, 50, 2000};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["position"] = {0, 90, 0}; player["walkVelocity"] = {0, 0, -200};
    Json states = Json::array({player, floor});
    Json declarations{{"player", player}, {"floor", floor}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 1.0f / 60);
    ASSERT_NEAR(states[0]["bodyVelocity"][2].get<float>(), -200, 0.01f);
    const auto position = states[0]["position"];
    states[0]["rotation"] = {0, 180, 0};
    states[0]["walkVelocity"] = {0, 0, 200};
    physics.Step(states, declarations, 1.0f / 120);
    // 每帧都按帧 dt 推进；纯转向不是传送，实际速度应保持新方向而不是被清零。
    EXPECT_GT(states[0]["position"][2].get<float>(), position[2].get<float>());
    EXPECT_NEAR(states[0]["bodyVelocity"][2].get<float>(), 200, 0.5f);
    EXPECT_TRUE(states[0]["grounded"].get<bool>());
}

TEST(ScriptPhysicsTest, CharacterPositionTeleportClearsAchievedVelocity)
{
    auto floor = GameplayBlueprint("floor"), player = GameplayBlueprint("player");
    floor["physics"] = Body("static"); floor["position"] = {0, -50, 0};
    floor["physics"]["halfExtents"] = {2000, 50, 2000};
    player["physics"] = {{"motion", "character"}, {"shape", "capsule"}, {"radius", 30}, {"halfHeight", 60}};
    player["position"] = {0, 90, 0}; player["walkVelocity"] = {0, 0, -200};
    Json states = Json::array({player, floor});
    Json declarations{{"player", player}, {"floor", floor}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 1.0f / 60);
    ASSERT_LT(states[0]["bodyVelocity"][2].get<float>(), -199);
    states[0]["position"] = {500, 500, 0};
    // 零时步同步传送：不推进时间，但必须清掉上一帧的实际速度与地面状态。
    physics.Step(states, declarations, 0);
    EXPECT_EQ(states[0]["position"], Json::array({500, 500, 0}));
    EXPECT_EQ(states[0]["bodyVelocity"], Json::array({0, 0, 0}));
    EXPECT_FALSE(states[0]["grounded"].get<bool>());
}

TEST(ScriptPhysicsTest, CollisionLayersFilterResponseContactsAndRayQueries)
{
    auto floor = GameplayBlueprint("floor"), ball = GameplayBlueprint("ball");
    floor["position"] = {0, -50, 0}; floor["physics"] = Body("static");
    floor["physics"]["halfExtents"] = {1000, 50, 1000};
    floor["physics"]["collisionLayer"] = 3; floor["physics"]["collisionMask"] = 1;
    ball["position"] = {0, 100, 0};
    ball["physics"] = {{"shape", "sphere"}, {"radius", 20}, {"motion", "dynamic"}, {"collisionMask", 1}};
    Json states = Json::array({floor, ball}), declarations{{"floor", floor}, {"ball", ball}};
    ScriptPhysicsWorld physics;
    for (int frame = 0; frame < 60; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_LT(states[1]["position"][1].get<float>(), -350);
    EXPECT_TRUE(physics.Contacts().empty());
    Json query{{"id", 1}, {"owner", "ball"}, {"from", {0, 100, 0}}, {"to", {0, -100, 0}}, {"collisionMask", 1}};
    EXPECT_FALSE(physics.Raycast(Json::array({query}))[0].contains("blueprint"));
    query["collisionMask"] = 8;
    EXPECT_EQ(physics.Raycast(Json::array({query}))[0]["blueprint"], "floor");
}

TEST(ScriptPhysicsTest, CharacterFilteringHonorsOtherBodyMask)
{
    auto floor = GameplayBlueprint("floor"), player = GameplayBlueprint("player");
    floor["position"] = {0, -50, 0}; floor["physics"] = Body("static");
    floor["physics"]["halfExtents"] = {1000, 50, 1000}; floor["physics"]["collisionMask"] = 1;
    player["position"] = {0, 100, 0};
    player["physics"] = {{"shape", "capsule"}, {"radius", 20}, {"halfHeight", 40},
        {"motion", "character"}, {"collisionLayer", 1}};
    Json states = Json::array({floor, player}), declarations{{"floor", floor}, {"player", player}};
    ScriptPhysicsWorld physics;
    for (int frame = 0; frame < 60; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_LT(states[1]["position"][1].get<float>(), -350);
    EXPECT_FALSE(states[1]["grounded"].get<bool>());
    EXPECT_TRUE(physics.Contacts().empty());
}

TEST(ScriptPhysicsTest, SphereSweepUsesProbeRadiusMasksAndTriggerPolicy)
{
    auto wall = GameplayBlueprint("wall"), sensor = GameplayBlueprint("sensor");
    wall["position"] = {200, 0, 0}; wall["physics"] = Body("static");
    wall["physics"]["collisionLayer"] = 2;
    sensor["position"] = {100, 0, 0}; sensor["physics"] = Body("static"); sensor["physics"]["trigger"] = true;
    Json states = Json::array({wall, sensor});
    ScriptPhysicsWorld physics;
    physics.Step(states, {{"wall", wall}, {"sensor", sensor}}, 0);
    Json query{{"id", 1}, {"owner", "none"}, {"from", {0, 0, 0}}, {"to", {400, 0, 0}}, {"radius", 20}};
    auto hit = physics.Raycast(Json::array({query}))[0];
    EXPECT_EQ(hit["blueprint"], "wall");
    EXPECT_NEAR(hit["distance"].get<float>(), 130, 0.1f);
    EXPECT_NEAR(hit["normal"][0].get<float>(), -1, 0.001f);
    query["includeTriggers"] = true;
    EXPECT_EQ(physics.Raycast(Json::array({query}))[0]["blueprint"], "sensor");
    query["collisionMask"] = 2;
    EXPECT_FALSE(physics.Raycast(Json::array({query}))[0].contains("blueprint"));
    query["collisionMask"] = 4; query["from"] = {200, 0, 0}; query["to"] = {300, 0, 0};
    hit = physics.Raycast(Json::array({query}))[0];
    EXPECT_EQ(hit["blueprint"], "wall"); EXPECT_LE(hit["distance"].get<float>(), 0.001f);
    EXPECT_TRUE(std::isfinite(hit["normal"][0].get<float>()));
}

TEST(ScriptPhysicsTest, CompoundCollidersUseComponentOffsetsAndRebuildOnTransformChange)
{
    auto compound = GameplayBlueprint("compound");
    compound["components"] = Json::array({{{"id", "child"}, {"type", "scene"}, {"position", {200, 0, 0}},
        {"collision", {{"shape", "sphere"}, {"radius", 25}}}}});
    compound["physics"] = {{"shape", "compound"}};
    Json states = Json::array({compound}), declarations{{"compound", compound}};
    ScriptPhysicsWorld physics;
    physics.Step(states, declarations, 0);
    Json query{{"id", 1}, {"owner", "none"}, {"from", {0, 0, 0}}, {"to", {500, 0, 0}}};
    EXPECT_NEAR(physics.Raycast(Json::array({query}))[0]["distance"].get<float>(), 175, 0.1f);
    states[0]["components"][0]["position"] = {300, 0, 0};
    physics.Step(states, declarations, 0);
    EXPECT_NEAR(physics.Raycast(Json::array({query}))[0]["distance"].get<float>(), 275, 0.1f);
}

TEST(ScriptPhysicsTest, StaticMeshCollisionUsesFormalTrianglesAndComponentTransformChanges)
{
    auto floor = GameplayBlueprint("floor"), ball = GameplayBlueprint("ball");
    floor["components"] = Json::array({{{"id", "mesh"}, {"type", "mesh"}, {"modelPath", "Common/Meshes/SM_test.json"}}});
    floor["physics"] = {{"shape", "mesh"}, {"component", "mesh"}};
    ball["position"] = {0, 100, 0}; ball["physics"] = {{"shape", "sphere"}, {"radius", 20}, {"motion", "dynamic"}};
    Json states = Json::array({floor, ball}), declarations{{"floor", floor}, {"ball", ball}};
    ScriptPhysicsWorld physics;
    physics.SetMeshGeometry({{"floor/mesh", {{-1000, 0, -1000}, {1000, 0, 1000}, {1000, 0, -1000},
        {-1000, 0, -1000}, {-1000, 0, 1000}, {1000, 0, 1000}}}});
    for (int frame = 0; frame < 120; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_NEAR(states[1]["position"][1].get<float>(), 20, 0.5f);
    EXPECT_EQ(physics.Contacts().size(), 1u);
    states[0]["components"][0]["position"] = {3000, 0, 0};
    const float height = states[1]["position"][1].get<float>();
    for (int frame = 0; frame < 30; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_LT(states[1]["position"][1].get<float>(), height - 50);
}

TEST(ScriptPhysicsTest, FixedJointHoldsBodyAndReleasesWhenTargetDestroyed)
{
    auto anchor = GameplayBlueprint("anchor"), ball = GameplayBlueprint("ball");
    anchor["physics"] = Body("static"); anchor["physics"]["collisionMask"] = 0;
    ball["position"] = {0, 200, 0}; ball["physics"] = Body("dynamic"); ball["physics"]["collisionMask"] = 0;
    ball["physics"]["constraints"] = Json::array({{{"id", "lock"}, {"type", "fixed"}, {"target", "anchor"},
        {"anchor", {0, 200, 0}}}});
    Json states = Json::array({anchor, ball}), declarations{{"anchor", anchor}, {"ball", ball}};
    ScriptPhysicsWorld physics;
    for (int frame = 0; frame < 120; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_NEAR(states[1]["position"][1].get<float>(), 200, 0.2f);
    states[0]["destroyed"] = true;
    for (int frame = 0; frame < 60; ++frame) physics.Step(states, declarations, 1.0f / 60);
    EXPECT_LT(states[1]["position"][1].get<float>(), -200);
}

TEST(ScriptPhysicsTest, HingeJointMaintainsAnchorDistanceWithinLimits)
{
    auto anchor = GameplayBlueprint("anchor"), pendulum = GameplayBlueprint("pendulum");
    anchor["physics"] = Body("static"); anchor["physics"]["collisionMask"] = 0;
    pendulum["position"] = {100, 0, 0}; pendulum["physics"] = Body("dynamic");
    pendulum["physics"]["collisionMask"] = 0;
    pendulum["physics"]["constraints"] = Json::array({{{"id", "hinge"}, {"type", "hinge"}, {"target", "anchor"},
        {"anchor", {0, 0, 0}}, {"axis", {0, 0, 1}}, {"minAngle", -45}, {"maxAngle", 45}}});
    Json states = Json::array({anchor, pendulum}), declarations{{"anchor", anchor}, {"pendulum", pendulum}};
    ScriptPhysicsWorld physics;
    for (int frame = 0; frame < 120; ++frame) physics.Step(states, declarations, 1.0f / 60);
    const float x = states[1]["position"][0].get<float>(), y = states[1]["position"][1].get<float>();
    EXPECT_NEAR(std::sqrt(x * x + y * y), 100, 1);
    EXPECT_LT(y, -5); EXPECT_GT(x, 65);
}

TEST(ScriptPhysicsTest, ThousandBodiesRemainFiniteAndSettleWithRecordedTimings)
{
    auto floor = GameplayBlueprint("floor");
    floor["position"] = {0, -50, 0}; floor["physics"] = Body("static");
    floor["physics"]["halfExtents"] = {5000, 50, 5000};
    Json states = Json::array({floor}), declarations{{"floor", floor}};
    for (int index = 0; index < 1000; ++index)
    {
        auto ball = GameplayBlueprint("ball" + std::to_string(index));
        ball["position"] = {(index % 40 - 20) * 100, 100 + index % 5 * 20, (index / 40 - 12) * 100};
        ball["physics"] = {{"shape", "sphere"}, {"radius", 20}, {"motion", "dynamic"}};
        declarations[ball["id"].get<std::string>()] = ball; states.push_back(std::move(ball));
    }
    ScriptPhysicsWorld physics;
    double totalMs = 0, maxMs = 0;
    for (int frame = 0; frame < 120; ++frame)
    {
        const auto start = std::chrono::steady_clock::now();
        physics.Step(states, declarations, 1.0f / 60);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        totalMs += elapsed; maxMs = std::max(maxMs, elapsed);
    }
    for (size_t index = 1; index < states.size(); ++index)
    {
        ASSERT_TRUE(std::isfinite(states[index]["position"][1].get<float>()));
        EXPECT_NEAR(states[index]["position"][1].get<float>(), 20, 1);
    }
    EXPECT_EQ(physics.Contacts().size(), 1000u);
    testing::Test::RecordProperty("bodies", 1000);
    testing::Test::RecordProperty("frames", 120);
    testing::Test::RecordProperty("total_ms", std::to_string(totalMs));
    testing::Test::RecordProperty("mean_ms", std::to_string(totalMs / 120));
    testing::Test::RecordProperty("max_ms", std::to_string(maxMs));
}
#endif
} // namespace
