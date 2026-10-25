#include <gtest/gtest.h>
#include "script/scriptRuntime.h"
#include "scene/validation/sceneJsonValidator.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"
#include "sceneNode.h"
#include "script/scriptBlueprintAsset.h"
#include "editor/scene/scriptBlueprintPlacement.h"
#include <fstream>
#include <chrono>

namespace
{
using namespace VL;
using Json = nlohmann::json;

Json Blueprint(const std::string& id, float speed = 45)
{
    return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", "Gameplay.RotatorBlueprint"},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", {{"DegreesPerSecond", speed}}}, {"components", Json::array()}};
}

// 每例独占临时资源根，文件变化用于验证重新加载语义，不依赖正式资源或 Vulkan。
class ScriptBlueprintAssetTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-blueprint-asset-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Common/Blueprints");
        asset = {{"type", "blueprint"}, {"name", "Rotator"}, {"class", "Gameplay.RotatorBlueprint"},
            {"instanceEditable", {"DegreesPerSecond"}},
            {"properties", {{"DegreesPerSecond", 45}}}};
        Write();
        instance = Blueprint("instance");
        instance.erase("class");
        instance.erase("components");
        instance["blueprintPath"] = "Common/Blueprints/BP_rotator.json";
        instance["properties"] = Json::object();
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void Write() { std::ofstream(root / "Common/Blueprints/BP_rotator.json") << asset.dump(); }
    std::filesystem::path root;
    Json asset, instance;
};

TEST_F(ScriptBlueprintAssetTest, SparseInstanceInheritsChangedAssetDefaults)
{
    const auto original = instance;
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["properties"]["DegreesPerSecond"], 45);
    asset["properties"]["DegreesPerSecond"] = 120;
    Write();
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["properties"]["DegreesPerSecond"], 120);
    EXPECT_EQ(instance, original);
}

TEST_F(ScriptBlueprintAssetTest, LegacyAssetTypeAndInstanceReferenceAreRejected)
{
    auto legacy = asset;
    legacy["type"] = "actorAsset";
    EXPECT_THROW(ValidateScriptBlueprintAsset(legacy), std::invalid_argument);
    auto oldType = Blueprint("old");
    oldType["type"] = "actor";
    EXPECT_THROW(ValidateScriptBlueprint(oldType), std::invalid_argument);
    auto oldReference = Blueprint("old-reference");
    oldReference["actorPath"] = "Common/Blueprints/ACT_rotator.json";
    EXPECT_THROW(ValidateScriptBlueprint(oldReference), std::invalid_argument);
}

TEST_F(ScriptBlueprintAssetTest, BlueprintTypeUsesTheSameSparseOverridesAndSpawnTemplateResolution)
{
    const std::string path = "Common/Blueprints/BP_rotator.json";
    std::filesystem::create_directories(root / "Common/Blueprints");
    asset["type"] = "blueprint";
    std::ofstream(root / path) << asset.dump();
    instance["blueprintPath"] = path;
    instance["properties"]["DegreesPerSecond"] = 90;
    const auto authored = instance;
    const auto scene = ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}},
        "Maps/SC_test/SC_test.json");
    EXPECT_EQ(scene["objects"][0]["class"], "Gameplay.RotatorBlueprint");
    EXPECT_EQ(scene["objects"][0]["properties"]["DegreesPerSecond"], 90);
    EXPECT_EQ(scene["objects"][1]["__templatePath"], path);
    EXPECT_EQ(scene["objects"][1]["properties"]["DegreesPerSecond"], 45);
    EXPECT_EQ(instance, authored);
    instance["properties"]["Category"] = "hidden";
    EXPECT_THROW(ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}},
        "Maps/SC_test/SC_test.json"), std::invalid_argument);
}

TEST_F(ScriptBlueprintAssetTest, ExplicitOverrideWinsUntilKeyIsRemoved)
{
    instance["properties"]["DegreesPerSecond"] = 90;
    asset["properties"]["DegreesPerSecond"] = 120;
    Write();
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["properties"]["DegreesPerSecond"], 90);
    instance["properties"].erase("DegreesPerSecond");
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["properties"]["DegreesPerSecond"], 120);
}

TEST_F(ScriptBlueprintAssetTest, ComponentOverridesMergeOnlyExposedFieldsAndPreserveSparseSource)
{
    asset["components"] = Json::array({
        {{"id", "pivot"}, {"type", "scene"}, {"position", {1, 2, 3}}},
        {{"id", "camera"}, {"type", "camera"}, {"parent", "pivot"}, {"fov", 60}, {"near_clip", 1}, {"far_clip", 1000}}
    });
    asset["componentInstanceEditable"] = {{"pivot", {"position"}}, {"camera", {"fov", "far_clip"}}};
    Write();
    instance["componentOverrides"] = {{"pivot", {{"position", {10, 20, 30}}}}, {"camera", {{"fov", 80}}}};
    const auto source = instance;
    const auto resolved = ResolveScriptBlueprint(root, instance);
    EXPECT_EQ(resolved["components"][0]["position"], Json::array({10, 20, 30}));
    EXPECT_EQ(resolved["components"][1]["fov"], 80);
    EXPECT_EQ(resolved["components"][1]["near_clip"], 1);
    EXPECT_EQ(resolved["components"][1]["parent"], "pivot");
    EXPECT_FALSE(resolved.contains("componentOverrides"));
    EXPECT_EQ(instance, source);
    const auto scene = ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}}, "Maps/SC_test/SC_test.json");
    EXPECT_EQ(scene["objects"][0]["components"], resolved["components"]);
    EXPECT_EQ(scene["objects"][1]["components"], asset["components"]);
    EXPECT_EQ(instance, source);
}

TEST_F(ScriptBlueprintAssetTest, SpringArmExtendedFieldsSupportSparseInstanceOverrides)
{
    asset["components"] = Json::array({{{"id", "arm"}, {"type", "springArm"}}});
    // 完整权限超过旧的 16 字段上限；实际白名单仍由共享类型表确定。
    asset["componentInstanceEditable"] = {{"arm", ScriptComponentInstanceFields(asset["components"][0])}};
    Write();
    instance["componentOverrides"] = {{"arm", {{"targetOffset", {0, 40, 0}}, {"rotationOffset", {10, 0, 0}},
        {"enableCameraLag", true}, {"cameraLagSpeed", 4}, {"useCameraLagSubstepping", false},
        {"cameraLagMaxTimeStep", 0.01}, {"inheritYaw", false}}}};
    const auto resolved = ResolveScriptBlueprint(root, instance);
    EXPECT_EQ(resolved["components"][0]["targetOffset"], Json::array({0, 40, 0}));
    EXPECT_FALSE(resolved["components"][0]["inheritYaw"].get<bool>());
    EXPECT_FALSE(resolved["components"][0]["useCameraLagSubstepping"].get<bool>());
    EXPECT_FALSE(instance["componentOverrides"]["arm"].contains("targetArmLength"));
}

TEST_F(ScriptBlueprintAssetTest, ComponentOverridesFollowChangedUnmodifiedDefaultsAndResetByRemovingKey)
{
    asset["components"] = Json::array({{{"id", "pivot"}, {"type", "scene"}, {"position", {1, 2, 3}}}});
    asset["componentInstanceEditable"] = {{"pivot", {"position", "rotation"}}};
    Write();
    instance["componentOverrides"] = {{"pivot", {{"position", {10, 20, 30}}}}};
    asset["components"][0]["position"] = {4, 5, 6};
    asset["components"][0]["rotation"] = {0, 90, 0};
    Write();
    const auto resolved = ResolveScriptBlueprint(root, instance);
    EXPECT_EQ(resolved["components"][0]["position"], Json::array({10, 20, 30}));
    EXPECT_EQ(resolved["components"][0]["rotation"], Json::array({0, 90, 0}));
    instance.erase("componentOverrides");
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["components"][0]["position"], Json::array({4, 5, 6}));
}

TEST_F(ScriptBlueprintAssetTest, ComponentPermissionWithdrawalRejectsPreviouslyAuthoredOverrides)
{
    asset["components"] = Json::array({{{"id", "pivot"}, {"type", "scene"}}});
    asset["componentInstanceEditable"] = {{"pivot", {"position"}}}; Write();
    instance["componentOverrides"] = {{"pivot", {{"position", {10, 20, 30}}}}};
    ASSERT_NO_THROW(ResolveScriptBlueprint(root, instance));
    asset.erase("componentInstanceEditable"); Write();
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
    EXPECT_THROW(ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}}, "SC_test.json"),
        std::invalid_argument);
}

TEST_F(ScriptBlueprintAssetTest, ComponentPermissionsRejectUnknownIdsStructuralAndWrongTypeFields)
{
    asset["components"] = Json::array({{{"id", "pivot"}, {"type", "scene"}}});
    for (const Json& permissions : {
        Json{{"missing", {"position"}}}, Json{{"pivot", {"parent"}}}, Json{{"pivot", {"id"}}},
        Json{{"pivot", {"type"}}}, Json{{"pivot", {"modelPath"}}},
        Json{{"pivot", {"position", "position"}}}, Json{{"pivot", "position"}}})
    {
        asset["componentInstanceEditable"] = permissions;
        EXPECT_THROW(ValidateScriptBlueprintAsset(asset), std::invalid_argument) << permissions;
    }
}

TEST_F(ScriptBlueprintAssetTest, ComponentOverridesRejectUnknownUnauthorizedAndInvalidFinalComponent)
{
    asset["components"] = Json::array({{{"id", "camera"}, {"type", "camera"}, {"near_clip", 1}, {"far_clip", 100}}});
    asset["componentInstanceEditable"] = {{"camera", {"near_clip", "far_clip"}}}; Write();
    for (const Json& overrides : {
        Json{{"missing", {{"near_clip", 2}}}}, Json{{"camera", {{"fov", 80}}}},
        Json{{"camera", {{"parent", "other"}}}}, Json{{"camera", {{"near_clip", 200}}}},
        Json{{"camera", {{"near_clip", true}}}}})
    {
        instance["componentOverrides"] = overrides;
        EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument) << overrides;
    }
    instance["componentOverrides"] = {{"camera", {{"near_clip", 200}, {"far_clip", 400}}}};
    EXPECT_NO_THROW(ResolveScriptBlueprint(root, instance));
}

TEST_F(ScriptBlueprintAssetTest, InlineComponentsCannotAuthorAssetOverridesOrPermissions)
{
    auto inlineBlueprint = Blueprint("inline");
    inlineBlueprint["componentOverrides"] = {{"root", {{"position", {1, 2, 3}}}}};
    EXPECT_THROW(ValidateScriptBlueprint(inlineBlueprint), std::invalid_argument);
    inlineBlueprint.erase("componentOverrides");
    inlineBlueprint["componentInstanceEditable"] = {{"root", {"position"}}};
    EXPECT_THROW(ValidateScriptBlueprint(inlineBlueprint), std::invalid_argument);
    instance["__componentInstanceEditable"] = {{"root", {"position"}}};
    const auto resolved = ResolveScriptBlueprint(root, instance);
    EXPECT_TRUE(resolved["__componentInstanceEditable"].empty());
}

TEST_F(ScriptBlueprintAssetTest, NonExposedInstanceOverrideIsRejectedBeforeBothMergeRoutes)
{
    asset.erase("instanceEditable"); Write();
    instance["properties"]["DegreesPerSecond"] = 90;
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
    EXPECT_THROW(ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}}, "SC_test.json"),
        std::invalid_argument);
    instance["properties"] = Json::object();
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["properties"]["DegreesPerSecond"], 45);
}

TEST_F(ScriptBlueprintAssetTest, SceneCannotSupplyItsOwnInstancePermissionMetadata)
{
    auto inlineBlueprint = Blueprint("inline");
    inlineBlueprint["__instanceEditable"] = {"Category"};
    const auto resolved = ResolveScriptBlueprintScene(root, {{"objects", Json::array({inlineBlueprint})}}, "SC_test.json");
    EXPECT_FALSE(resolved["objects"][0].contains("__instanceEditable"));
}

TEST_F(ScriptBlueprintAssetTest, AmbiguousInstanceAndRecursiveAssetAreRejected)
{
    instance["class"] = "Gameplay.Other";
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
    instance.erase("class");
    instance["components"] = Json::array();
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
    instance.erase("components");
    asset["blueprintPath"] = "Common/Blueprints/BP_rotator.json";
    Write();
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
}

TEST_F(ScriptBlueprintAssetTest, RejectsMissingEscapedAndGeneratedReferences)
{
    for (const auto* path : {"../BP_escape.json", "Common/Blueprints/missing.json", "C:/outside.json"})
    {
        instance["blueprintPath"] = path;
        EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::exception) << path;
    }
    std::filesystem::create_directories(root / "Generated");
    std::ofstream(root / "Generated/BP_generated.json") << asset.dump();
    instance["blueprintPath"] = "Generated/BP_generated.json";
    EXPECT_THROW(ResolveScriptBlueprint(root, instance, "Maps/SC_test/SC_test.json"), std::invalid_argument);
}

TEST_F(ScriptBlueprintAssetTest, RejectsDuplicateKeysAndExcessiveJsonDepth)
{
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << R"({"type":"blueprint","type":"scene"})";
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
    std::ofstream(root / "Common/Blueprints/BP_rotator.json") << std::string(66, '[') + "0" + std::string(66, ']');
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
}

TEST(ScriptScene, PlacementUsesViewportCameraAndGroundPlane)
{
    Camera camera;
    camera.SetCamera(Eigen::Vector3f(100, 500, 1000), Eigen::Vector3f(100, 0, 0), Eigen::Vector3f::UnitY());
    camera.SetProjection(60, 16.0f/9, 1, 10000);
    const Eigen::Matrix4f matrix = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    const auto center = ScriptBlueprintPlacement(matrix, 0.5f, 0.5f);
    EXPECT_NEAR(center.x(), 100, 0.02f);
    EXPECT_NEAR(center.y(), 0, 0.02f);
    EXPECT_NEAR(center.z(), 0, 0.1f);
    EXPECT_GT(ScriptBlueprintPlacement(matrix, 0.7f, 0.5f).x(), center.x());
    EXPECT_GT(ScriptBlueprintPlacement(matrix, 0.5f, 0.7f).z(), center.z());
    EXPECT_THROW(ScriptBlueprintPlacement(matrix, 1, 0.5f), std::invalid_argument);
}

TEST(ScriptScene, PlacementRejectsParallelBehindAndSingularRays)
{
    Camera camera;
    camera.SetProjection(60, 1, 1, 10000);
    camera.SetCamera(Eigen::Vector3f(0, 100, 100), Eigen::Vector3f(0, 100, 0), Eigen::Vector3f::UnitY());
    Eigen::Matrix4f matrix = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    EXPECT_THROW(ScriptBlueprintPlacement(matrix, 0.5f, 0.5f), std::invalid_argument);
    EXPECT_THROW(ScriptBlueprintPlacement(matrix, 0.5f, 0.1f), std::invalid_argument);
    EXPECT_THROW(ScriptBlueprintPlacement(Eigen::Matrix4f::Zero(), 0.5f, 0.5f), std::invalid_argument);
}

TEST(ScriptScene, RejectsDuplicatePersistentIdentity)
{
    auto a = Blueprint("a"), b = Blueprint("a");
    b["name"] = "Another display label";
    Json scene{{"worldUnit", "cm"}, {"description", "Blueprint identity fixture"},
        {"objects", Json::array({a, b,
            {{"name", "Camera"}, {"type", "camera"}, {"position", {0, 0, 10}},
                {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
                {"near_clip", 1}, {"far_clip", 100}, {"fov", 60}}})}};
    scene["objects"][2]["exposure"] = DescribeCameraExposure(CameraExposure{});
    scene["objects"].erase(1);
    ASSERT_NO_THROW(ValidateSceneAssetJson(scene));
    scene["objects"].push_back(b);
    EXPECT_THROW(ValidateSceneAssetJson(scene), std::invalid_argument);
}

TEST(ScriptScene, RejectsUnsupportedComponents)
{
    auto blueprint = Blueprint("a");
    blueprint["components"] = Json::array({{{"type", "terrain"}}});
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
}

TEST(ScriptScene, DistinctIdsCanShareDisplayName)
{
    auto a = Blueprint("a"), b = Blueprint("b");
    a["name"] = b["name"] = "Shared label";
    Json scene{{"worldUnit", "cm"}, {"description", "Blueprint display fixture"},
        {"objects", Json::array({a, b,
            {{"name", "Camera"}, {"type", "camera"}, {"position", {0, 0, 10}},
                {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
                {"near_clip", 1}, {"far_clip", 100}, {"fov", 60}}})}};
    scene["objects"][2]["exposure"] = DescribeCameraExposure(CameraExposure{});
    EXPECT_NO_THROW(ValidateSceneAssetJson(scene));
}

TEST(ScriptScene, MeshComponentRetainsRootTransformAndMaterialOverride)
{
    auto blueprint = Blueprint("a");
    blueprint["position"] = {2, 3, 4};
    blueprint["components"] = Json::array({{{"type", "mesh"}, {"modelPath", "SM_test.json"},
        {"materialOverride", "MI_test.json"}}});
    const auto mesh = ScriptBlueprintMeshObject(blueprint);
    EXPECT_EQ(mesh.at("name"), "__blueprint/a");
    EXPECT_EQ(mesh.at("position"), blueprint.at("position"));
    EXPECT_EQ(mesh.at("materialOverride"), "MI_test.json");
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
TEST(ScriptStartup, MissingHostReportsStableCodeWithoutPublishingRuntime)
{
    ScriptRuntimeSubsystem runtime;
    const auto missing = std::filesystem::temp_directory_path() /
        ("vl-missing-host-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        runtime.Initialize(true, missing.string(), (missing / "Gameplay.dll").string());
        FAIL() << "Missing host must reject initialization";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_EQ(std::string(error.what()).find("Script.HostMissing:"), 0u);
        EXPECT_NE(std::string(error.what()).find("hostfxr.dll"), std::string::npos);
    }
    EXPECT_FALSE(runtime.View(0).available);
    EXPECT_TRUE(runtime.Classes().empty());
}

TEST(ScriptStartup, MissingGameplayReportsStructuredFailureAndAllowsRetry)
{
    ScriptRuntimeSubsystem runtime;
    const auto missing = std::filesystem::path(VL_SCRIPT_OUTPUT_DIRECTORY) /
        ("MissingGameplay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dll");
    try
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY, missing.string());
        FAIL() << "Missing Gameplay assembly must reject initialization";
    }
    catch (const std::runtime_error& error)
    {
        const auto failure = Json::parse(error.what());
        EXPECT_FALSE(failure.at("ok").get<bool>());
        EXPECT_EQ(failure.at("operation"), "initialize");
        EXPECT_EQ(failure.at("action"), "RejectRequest");
        EXPECT_FALSE(failure.at("exceptionType").get<std::string>().empty());
        EXPECT_NE(failure.at("error").get<std::string>().find(missing.filename().string()), std::string::npos);
    }
    EXPECT_FALSE(runtime.View(0).available);
    EXPECT_TRUE(runtime.Classes().empty());
    ASSERT_NO_THROW(runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
        std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/Gameplay.dll"));
    EXPECT_TRUE(runtime.View(0).available);
    EXPECT_FALSE(runtime.Classes().empty());
}

class ScriptRuntimeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "scene");
        runtime.SynchronizeWorld(world);
    }
    void TearDown() override { runtime.Shutdown(); }
    void Prepare(Json blueprints)
    {
        for (const auto& blueprint : blueprints)
        {
            WorldMeshObject mesh;
            mesh.sceneObjectIdentity = ScriptBlueprintMeshIdentity(blueprint.at("id"));
            world->AddMeshObject(mesh.sceneObjectIdentity, mesh);
        }
        runtime.PrepareWorld(*world, {{"objects", blueprints}});
    }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptRuntimeTest, TwoInstancesUseIndependentSceneProperties)
{
    Prepare(Json::array({Blueprint("slow", 30), Blueprint("fast", 90)}));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/slow").sceneRotation.y(), 30);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/fast").sceneRotation.y(), 90);
}

TEST_F(ScriptRuntimeTest, TransformPublishesIntoNewSnapshotWithoutChangingFrozenFrame)
{
    Prepare(Json::array({Blueprint("a", 90)}));
    world->SetCamera(std::make_shared<Camera>());
    WorldSnapshotBuilder builder;
    const auto before = builder.Build(*world, {});
    ASSERT_TRUE(before.IsSuccess());
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    runtime.Tick(world, 1, {}, diagnostics);
    const auto after = builder.Build(*world, {});
    ASSERT_TRUE(after.IsSuccess());
    ASSERT_EQ(after.Value().meshDraws.size(), 1u);
    SceneNode expected;
    expected.SetTransform(Eigen::Vector3f::Zero(), Eigen::Vector3f(0, 90, 0), Eigen::Vector3f::Ones());
    EXPECT_TRUE(after.Value().meshDraws[0].model.isApprox(expected.GetModelMatrix(), 1e-5f));
    EXPECT_TRUE(before.Value().meshDraws[0].model.isApprox(Eigen::Matrix4f::Identity()));
}

TEST_F(ScriptRuntimeTest, SnapshotFreezesScaledAndOrdinaryAnimationTimeSeparately)
{
    Prepare(Json::array({Blueprint("a", 90)}));
    WorldMeshObject ordinary;
    ordinary.sceneObjectIdentity = "OrdinaryCharacter";
    world->AddMeshObject("OrdinaryCharacter", ordinary);
    world->SetCamera(std::make_shared<Camera>());
    std::string error;
    ASSERT_TRUE(runtime.SetTimeScale(0.25f, error)) << error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    runtime.Tick(world, 0.04f, {}, diagnostics);

    WorldSnapshotBuildDesc desc;
    desc.simulationDeltaSeconds = 0.04f;
    desc.gameSimulationDeltaSeconds = runtime.ContextTime().at("deltaTime").get<float>();
    desc.hairSimulationDeltaSeconds = desc.gameSimulationDeltaSeconds;
    WorldSnapshotBuilder builder;
    const auto frozen = builder.Build(*world, desc);
    ASSERT_TRUE(frozen.IsSuccess());
    ASSERT_EQ(frozen.Value().meshDraws.size(), 2u);
    EXPECT_FLOAT_EQ(frozen.Value().hairSimulationDeltaSeconds, 0.01f);
    for (const MeshDrawSnapshot& draw : frozen.Value().meshDraws)
    {
        const float expected = draw.sceneObjectIdentity == "__blueprint/a" ? 0.01f : 0.04f;
        EXPECT_FLOAT_EQ(draw.simulationDeltaSeconds, expected);
    }

    ASSERT_TRUE(runtime.SetTimeScale(1.0f, error)) << error;
    runtime.Tick(world, 0.08f, {}, diagnostics);
    desc.simulationDeltaSeconds = 0.08f;
    desc.gameSimulationDeltaSeconds = runtime.ContextTime().at("deltaTime").get<float>();
    desc.hairSimulationDeltaSeconds = desc.gameSimulationDeltaSeconds;
    const auto next = builder.Build(*world, desc);
    ASSERT_TRUE(next.IsSuccess());
    EXPECT_FLOAT_EQ(next.Value().hairSimulationDeltaSeconds, 0.08f);
    EXPECT_FLOAT_EQ(frozen.Value().hairSimulationDeltaSeconds, 0.01f);
    for (const MeshDrawSnapshot& draw : frozen.Value().meshDraws)
    {
        const float expected = draw.sceneObjectIdentity == "__blueprint/a" ? 0.01f : 0.04f;
        EXPECT_FLOAT_EQ(draw.simulationDeltaSeconds, expected);
    }
}

TEST_F(ScriptRuntimeTest, SnapshotFreezesPausedGameplayWithoutStoppingOrdinaryAnimation)
{
    Prepare(Json::array({Blueprint("a", 90)}));
    WorldMeshObject ordinary;
    ordinary.sceneObjectIdentity = "OrdinaryCharacter";
    world->AddMeshObject("OrdinaryCharacter", ordinary);
    world->SetCamera(std::make_shared<Camera>());
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    runtime.Tick(world, 0.04f, {}, diagnostics);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error)) << error;
    runtime.Tick(world, 0.04f, {}, diagnostics);

    WorldSnapshotBuildDesc desc;
    desc.simulationDeltaSeconds = 0.04f;
    desc.gameSimulationDeltaSeconds = runtime.ContextTime().at("deltaTime").get<float>();
    desc.hairSimulationDeltaSeconds = desc.gameSimulationDeltaSeconds;
    WorldSnapshotBuilder builder;
    const auto frozen = builder.Build(*world, desc);
    ASSERT_TRUE(frozen.IsSuccess());
    ASSERT_EQ(frozen.Value().meshDraws.size(), 2u);
    EXPECT_FLOAT_EQ(frozen.Value().hairSimulationDeltaSeconds, 0.0f);
    for (const MeshDrawSnapshot& draw : frozen.Value().meshDraws)
    {
        const float expected = draw.sceneObjectIdentity == "__blueprint/a" ? 0.0f : 0.04f;
        EXPECT_FLOAT_EQ(draw.simulationDeltaSeconds, expected);
    }
}

TEST_F(ScriptRuntimeTest, PauseFreezesAndStopRestoresAuthoredTransform)
{
    Prepare(Json::array({Blueprint("a", 90)}));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    runtime.Tick(world, 1, {}, diagnostics);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Paused, error));
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/a").sceneRotation.y(), 90);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/a").sceneRotation.y(), 0);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/a").sceneRotation.y(), 90);
}

TEST_F(ScriptRuntimeTest, InvalidCandidateLeavesPlayingWorldUnchanged)
{
    Prepare(Json::array({Blueprint("a", 90)}));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    World candidate(2, "bad");
    auto bad = Blueprint("b"); bad["class"] = "Gameplay.Missing";
    EXPECT_THROW(runtime.PrepareWorld(candidate, {{"objects", Json::array({bad})}}), std::runtime_error);
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/a").sceneRotation.y(), 90);
    EXPECT_EQ(runtime.View(1).state, ScriptPlayState::Playing);
}

TEST_F(ScriptRuntimeTest, ReplacedWorldStopsOldCallbacksAndInvalidatesContext)
{
    Prepare(Json::array({Blueprint("a")}));
    auto old = world->GetScriptContext();
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    old = world->GetScriptContext();
    auto replacement = std::make_shared<World>(2, "new");
    runtime.SynchronizeWorld(replacement);
    EXPECT_THROW(old->Call("tick", {{"dt", 1}, {"input", {0, 0, 0}}}), std::runtime_error);
    EXPECT_EQ(runtime.View(2).state, ScriptPlayState::Editing);
}

TEST_F(ScriptRuntimeTest, UnknownExposedPropertyRejectsCandidate)
{
    auto blueprint = Blueprint("a"); blueprint["properties"]["Missing"] = 1;
    EXPECT_THROW(Prepare(Json::array({blueprint})), std::runtime_error);
}

TEST_F(ScriptRuntimeTest, ExposeWithoutInstancePermissionDoesNotPermitInlineOverride)
{
    auto blueprint = Blueprint("a");
    blueprint["properties"]["Category"] = "Scene override";
    EXPECT_THROW(Prepare(Json::array({blueprint})), std::invalid_argument);
    blueprint["properties"].erase("Category");
    EXPECT_NO_THROW(Prepare(Json::array({blueprint})));
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["Category"], "Prop");
}

TEST_F(ScriptRuntimeTest, ValidationAcceptsOmittedEnabledAndRejectsWrongPropertyType)
{
    EXPECT_NO_THROW(runtime.ValidateBlueprint(Blueprint("a")));
    auto blueprint = Blueprint("a");
    blueprint["properties"]["DegreesPerSecond"] = "fast";
    EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::runtime_error);
}

TEST_F(ScriptRuntimeTest, TypedPropertiesBindVectorsEnumsAndStableReferences)
{
    auto blueprint = Blueprint("typed");
    blueprint["class"] = "ScriptFixtures.TypedPropertiesBlueprint";
    blueprint["properties"] = {{"Offset", {10, 20, 30}}, {"Planar", {2, 3}}, {"Weights", {0, 0, 0, 4}},
        {"Mode", "Fly"}, {"Target", "target"}, {"Asset", "Common/Blueprints/BP_child.json"}};
    auto target = Blueprint("target");
    target["position"] = {100, 0, 0};
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint, target})}});
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"], Json::array({112, 123, 34}));
    EXPECT_EQ(world->GetScriptBlueprints()[0]["properties"]["Offset"], Json::array({10, 20, 30}));
}

TEST_F(ScriptRuntimeTest, TypedPropertyCatalogPublishesArraysAndEnumNames)
{
    for (const auto& type : runtime.Classes())
        if (type["name"] == "ScriptFixtures.TypedPropertiesBlueprint")
        {
            Json fields;
            for (const auto& field : type["properties"]) fields[field["name"].get<std::string>()] = field;
            EXPECT_EQ(fields["Offset"]["type"], "vector3");
            EXPECT_EQ(fields["Offset"]["value"], Json::array({3, 4, 5}));
            EXPECT_EQ(fields["Weights"]["value"], Json::array({6, 7, 8, 9}));
            EXPECT_EQ(fields["Mode"]["options"], Json::array({"Walk", "Fly"}));
            EXPECT_EQ(fields["Mode"]["value"], "Walk");
            EXPECT_EQ(fields["Target"]["value"], "");
            EXPECT_EQ(fields["Target"]["type"], "blueprintReference");
            EXPECT_EQ(fields["Asset"]["type"], "asset");
            return;
        }
    FAIL() << "Typed property fixture was not discovered";
}

TEST_F(ScriptRuntimeTest, TypedPropertiesRejectMalformedValuesBeforeCandidatePublication)
{
    auto blueprint = Blueprint("typed");
    blueprint["class"] = "ScriptFixtures.TypedPropertiesBlueprint";
    for (const auto& invalid : Json::array({
        {{"Offset", {1, 2}}}, {{"Planar", {1, "bad"}}}, {{"Weights", {1, 2, 3, 1e100}}},
        {{"Mode", 1}}, {{"Mode", "walk"}}, {{"Mode", "Walk, Fly"}},
        {{"Asset", "../secret.json"}}, {{"Asset", "C:/secret.json"}}, {{"Asset", "Common//asset.json"}},
        {{"Target", "blueprintInstance|target"}}
    }))
    {
        blueprint["properties"] = invalid;
        EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::exception) << invalid.dump();
    }
    EXPECT_TRUE(world->GetScriptBlueprints().empty());
}

TEST_F(ScriptRuntimeTest, VectorInstancePermissionStillRequiresExplicitExposure)
{
    auto blueprint = Blueprint("typed");
    blueprint["class"] = "ScriptFixtures.TypedPropertiesBlueprint";
    blueprint["properties"] = {{"PrivateOffset", {1, 2, 3}}};
    EXPECT_THROW(runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}}), std::invalid_argument);
}

TEST_F(ScriptRuntimeTest, LogicalBlueprintUsesInputAndDoesNotOverwriteAuthorData)
{
    auto blueprint = Blueprint("player");
    blueprint["class"] = "Gameplay.PlayerBlueprint";
    blueprint["properties"] = {{"MoveSpeed", 100}};
    runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}});
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    InputActionState input;
    input.moveForward = true;
    runtime.Tick(world, 0.5f, input, diagnostics);
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"][2], -50);
    EXPECT_EQ(world->GetScriptBlueprints()[0]["position"][2], 0);
    EXPECT_TRUE(world->GetMeshObjects().empty());
}

TEST_F(ScriptRuntimeTest, CallbackFailureRestoresTransformAndOtherBlueprintStillTicks)
{
    auto blueprint = Blueprint("bad");
    blueprint["class"] = "ScriptFixtures.LifecycleBlueprint";
    blueprint["properties"] = {{"FailTick", true}};
    Prepare(Json::array({blueprint, Blueprint("good", 90)}));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"][0], 11);
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["failed"], true);
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"][0], 11);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/good").sceneRotation.y(), 180);
}

TEST_F(ScriptRuntimeTest, CreateFailureRejectsCandidateAndPreservesActiveContext)
{
    Prepare(Json::array({Blueprint("good", 90)}));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    auto blueprint = Blueprint("bad");
    blueprint["class"] = "ScriptFixtures.LifecycleBlueprint";
    blueprint["properties"] = {{"FailCreate", true}};
    EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::runtime_error);
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_FLOAT_EQ(world->GetMeshObjects().at("__blueprint/good").sceneRotation.y(), 90);
}

TEST_F(ScriptRuntimeTest, AssetPreviewAppliesVisualsWithoutCreatingManagedInstance)
{
    World preview{2, "Maps/SC_model_preview/SC_model_preview.json"};
    auto blueprint = Blueprint("~preview.test");
    blueprint["class"] = "ScriptFixtures.LifecycleBlueprint";
    blueprint["properties"] = {{"FailCreate", true}};
    blueprint["position"] = {100, 20, 0};
    WorldMeshObject mesh;
    mesh.sceneObjectIdentity = "__blueprint/~preview.test";
    preview.AddMeshObject(mesh.sceneObjectIdentity, mesh);
    ASSERT_NO_THROW(runtime.PrepareWorld(preview, {{"objects", Json::array({blueprint})}}));
    EXPECT_EQ(preview.GetScriptContext(), nullptr);
    EXPECT_FLOAT_EQ(preview.GetMeshObjects().begin()->second.model(0, 3), 100);
    EXPECT_EQ(preview.GetScriptBlueprints()[0]["id"], blueprint["id"]);
}

TEST_F(ScriptRuntimeTest, AssetPreviewRejectsGameplayStart)
{
    World preview{2, "Maps/SC_model_preview/SC_model_preview.json"};
    runtime.PrepareWorld(preview, {{"objects", Json::array({Blueprint("~preview.test")})}});
    std::string error;
    EXPECT_FALSE(runtime.Control(preview, ScriptPlayState::Playing, error));
    EXPECT_EQ(preview.GetScriptContext(), nullptr);
    EXPECT_NE(error.find("preview"), std::string::npos);
}

TEST_F(ScriptRuntimeTest, DestroyHidesMeshAndStopRestoresIt)
{
    auto blueprint = Blueprint("a");
    blueprint["class"] = "ScriptFixtures.LifecycleBlueprint";
    blueprint["properties"] = {{"SelfDestroy", true}};
    Prepare(Json::array({blueprint}));
    std::string error;
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error));
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_FALSE(world->GetMeshObjects().at("__blueprint/a").visible);
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"][0], 111);
    runtime.Tick(world, 1, {}, diagnostics);
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"][0], 111);
    ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    EXPECT_TRUE(world->GetMeshObjects().at("__blueprint/a").visible);
}
#endif
} // namespace
