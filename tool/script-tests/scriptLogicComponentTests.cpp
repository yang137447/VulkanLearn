#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "script/scriptRuntime.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptLogicComponents.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"
#include "world/world.h"
#include "sceneNode.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

Json LogicBlueprint()
{
    return {{"type", "blueprintInstance"}, {"id", "owner"}, {"name", "Owner"},
        {"class", "ScriptFixtures.ComponentControllerBlueprint"}, {"position", {0, 0, 0}},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"properties", Json::object()}, {"components", Json::array()},
        {"scriptComponents", Json::array({{{"id", "counter"}, {"class", "ScriptFixtures.CounterComponent"},
            {"properties", Json::object()}}})}};
}

TEST(ScriptLogicComponentAuthoring, RejectsDuplicateIdsUnknownFieldsAndOverrideStructure)
{
    auto blueprint = LogicBlueprint();
    ASSERT_NO_THROW(ValidateScriptBlueprint(blueprint));
    blueprint["scriptComponents"].push_back(blueprint["scriptComponents"][0]);
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint = LogicBlueprint(); blueprint["scriptComponents"][0]["parent"] = "root";
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
    blueprint = LogicBlueprint(); blueprint["scriptComponentOverrides"] = Json::object();
    EXPECT_THROW(ValidateScriptBlueprint(blueprint), std::invalid_argument);
}

TEST(ScriptLogicComponentAuthoring, BothResolutionPathsEnforceSparseAssetPermissions)
{
    const auto root = std::filesystem::temp_directory_path() / ("vl-logic-component-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "Common/Blueprints");
    auto asset = LogicBlueprint();
    asset["type"] = "blueprint"; asset.erase("id"); asset.erase("position");
    asset["scriptComponents"][0]["instanceEditable"] = {"Speed"};
    asset["scriptComponents"][0]["properties"]["Speed"] = 10;
    const auto path = root / "Common/Blueprints/BP_logic.json";
    std::ofstream(path) << asset.dump();
    auto instance = LogicBlueprint(); instance.erase("class"); instance.erase("scriptComponents"); instance.erase("components");
    instance["blueprintPath"] = "Common/Blueprints/BP_logic.json";
    instance["scriptComponentOverrides"] = {{"counter", {{"Speed", 20}}}};
    const auto authored = instance;
    EXPECT_EQ(ResolveScriptBlueprint(root, instance)["scriptComponents"][0]["properties"]["Speed"], 20);
    auto scene = ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}}, "Maps/SC_test/SC_test.json");
    EXPECT_EQ(scene["objects"][0]["scriptComponents"][0]["properties"]["Speed"], 20);
    EXPECT_EQ(instance, authored);
    asset["scriptComponents"][0]["instanceEditable"] = Json::array();
    std::ofstream(path) << asset.dump();
    EXPECT_THROW(ResolveScriptBlueprint(root, instance), std::invalid_argument);
    EXPECT_THROW(ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}}, "Maps/SC_test/SC_test.json"),
        std::invalid_argument);
    std::filesystem::remove_all(root);
}

TEST(ScriptLogicComponentAuthoring, QuaternionSchemaRequiresFiniteUnitXyzwValues)
{
    const Json catalog = Json::array({{{"name", "UnitQuaternion"}, {"properties", Json::array({
        {{"name", "Rotation"}, {"type", "quaternion"}, {"value", {0, 0, 0, 1}}, {"instanceEditable", true}}})}}});
    Json node{{"class", "UnitQuaternion"}, {"properties", {{"Rotation", {0, 0, 0, 1}}}}};
    EXPECT_NO_THROW(ValidateScriptPropertySchema(node, catalog));
    node["properties"]["Rotation"] = {0, 0, 0, 2};
    EXPECT_THROW(ValidateScriptPropertySchema(node, catalog), std::runtime_error);
    node["properties"]["Rotation"] = {0, 0, 1};
    EXPECT_THROW(ValidateScriptPropertySchema(node, catalog), std::runtime_error);
    node["properties"]["Rotation"] = {0, 0, 0.70710678, 0.70710678};
    EXPECT_NO_THROW(ValidateScriptPropertySchema(node, catalog));
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
class ScriptLogicComponentTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(3001, "logic-components");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }
    void Prepare(Json blueprint)
    {
        runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}});
        runtime.SynchronizeWorld(world);
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    void Tick() { runtime.Tick(world, 1.0f / 60, {}, diagnostics); }
    Json State() const { return world->GetScriptBlueprintStates().at(0); }
    Json ComponentState(const std::string& id = "counter") const
    {
        const auto state = State();
        for (const auto& component : state.at("scriptComponents"))
            if (component.at("id") == id) return component;
        return nullptr;
    }
    ScriptRuntimeSubsystem runtime;
    DiagnosticsSubsystem diagnostics;
    std::shared_ptr<World> world;
};

TEST_F(ScriptLogicComponentTest, RunsIndependentLifecycleTickAndQuaternionDefaults)
{
    auto blueprint = LogicBlueprint();
    blueprint["scriptComponents"][0]["properties"]["Speed"] = 60;
    Prepare(blueprint); Tick();
    ASSERT_FALSE(State()["failed"]);
    auto component = ComponentState();
    EXPECT_FALSE(component["failed"]);
    EXPECT_EQ(component["properties"]["Begins"], 1);
    EXPECT_EQ(component["properties"]["Ticks"], 1);
    EXPECT_EQ(component["properties"]["Orientation"], Json::array({0, 0, 0, 1}));
    EXPECT_NEAR(State()["position"][0].get<float>(), 1, 1e-4f);
    // 生命周期次数读取完整诊断；生产 UI 明细按 10 Hz 采样，不能假定每帧同步。
    const auto metrics = world->GetScriptContext()->Call("runtime_snapshot").at("result")
        .at("blueprints").at(0).at("components").at(0);
    bool hasTickTiming = false;
    for (const auto& timing : metrics.at("callbacks"))
        if (timing.at("phase") == "Tick") hasTickTiming = timing.at("calls").get<int>() == 1;
    EXPECT_TRUE(hasTickTiming);
}

TEST_F(ScriptLogicComponentTest, FailedComponentRollsBackOwnerTransformAndOtherComponentsContinue)
{
    auto blueprint = LogicBlueprint();
    blueprint["scriptComponents"][0]["properties"]["FailTick"] = true;
    auto healthy = blueprint["scriptComponents"][0]; healthy["id"] = "healthy";
    healthy["properties"]["FailTick"] = false;
    blueprint["scriptComponents"].push_back(healthy);
    Prepare(blueprint); Tick(); Tick();
    EXPECT_FALSE(State()["failed"]);
    EXPECT_TRUE(ComponentState()["failed"]);
    EXPECT_EQ(ComponentState()["properties"]["Ends"], 1);
    EXPECT_EQ(ComponentState("healthy")["properties"]["Ticks"], 2);
    EXPECT_NEAR(State()["position"][0].get<float>(), 20.0f / 60, 1e-4f);
    const auto performance = runtime.View(world->GetGeneration()).performance;
    bool componentError = false;
    for (const auto& metric : performance.at("blueprints").at(0).at("components"))
        if (metric.at("id") == "counter")
            componentError = metric.at("failed") &&
                metric.at("lastException").get<std::string>().find("component-tick-failed") != std::string::npos;
    EXPECT_TRUE(componentError);
    EXPECT_EQ(performance.at("pending").at("subscriptions"), 1);
    EXPECT_EQ(performance.at("pending").at("timers"), 1);
}

TEST_F(ScriptLogicComponentTest, TimerFailureIsAttributedToComponentAndKeepsOtherServices)
{
    auto blueprint = LogicBlueprint();
    blueprint["scriptComponents"][0]["properties"]["FailTimer"] = true;
    auto healthy = blueprint["scriptComponents"][0]; healthy["id"] = "healthy";
    healthy["properties"]["FailTimer"] = false;
    blueprint["scriptComponents"].push_back(healthy);
    Prepare(blueprint); Tick();
    EXPECT_FALSE(State()["failed"]);
    EXPECT_TRUE(ComponentState()["failed"]);
    EXPECT_EQ(ComponentState()["properties"]["Ends"], 1);
    EXPECT_EQ(ComponentState("healthy")["properties"]["Ticks"], 1);
    EXPECT_NEAR(State()["position"][0].get<float>(), 10.0f / 60, 1e-4f);
    const auto performance = runtime.View(world->GetGeneration()).performance;
    EXPECT_EQ(performance.at("pending").at("timers"), 1);
    EXPECT_EQ(performance.at("pending").at("subscriptions"), 1);
}

TEST_F(ScriptLogicComponentTest, DisableEndsAndReenableBeginsOnlyThatComponent)
{
    auto blueprint = LogicBlueprint(); blueprint["properties"]["Mode"] = 1;
    Prepare(blueprint); Tick();
    EXPECT_EQ(ComponentState()["properties"]["Ends"], 1);
    EXPECT_EQ(ComponentState()["properties"]["Ticks"], 0);
    Tick();
    EXPECT_EQ(ComponentState()["properties"]["Begins"], 2);
    EXPECT_EQ(ComponentState()["properties"]["Ticks"], 1);
}

TEST_F(ScriptLogicComponentTest, DeferredAddRemoveReusesIdWithNewHandleAndRejectsStaleAccess)
{
    auto blueprint = LogicBlueprint(); blueprint["properties"]["Mode"] = 2;
    Prepare(blueprint); Tick();
    ASSERT_FALSE(ComponentState("runtime").is_null());
    auto generation = ComponentState("runtime")["generation"];
    Tick(); EXPECT_TRUE(ComponentState("runtime").is_null());
    Tick(); EXPECT_FALSE(State()["failed"]);
    EXPECT_NE(ComponentState("runtime")["generation"], generation);
}

TEST_F(ScriptLogicComponentTest, FailedOwnerDiscardsComponentCommands)
{
    auto blueprint = LogicBlueprint(); blueprint["properties"]["Mode"] = 3;
    Prepare(blueprint); Tick();
    EXPECT_TRUE(State()["failed"]);
    EXPECT_TRUE(ComponentState("rolled-back").is_null());
    EXPECT_EQ(ComponentState()["properties"]["Ends"], 1);
}

TEST_F(ScriptLogicComponentTest, DestroyAndOwnerFailureEndStartedComponents)
{
    for (int mode : {4, 5})
    {
        auto blueprint = LogicBlueprint(); blueprint["properties"]["Mode"] = mode;
        Prepare(blueprint); Tick(); Tick(); Tick();
        EXPECT_EQ(ComponentState()["properties"]["Ends"], 1);
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Editing, error));
    }
}

TEST_F(ScriptLogicComponentTest, OnCreateFailureRejectsCandidate)
{
    auto blueprint = LogicBlueprint();
    blueprint["scriptComponents"][0]["properties"]["FailCreate"] = true;
    EXPECT_THROW(runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}}), std::exception);
}

TEST_F(ScriptLogicComponentTest, InlineUnexposedPropertiesAndInvalidQuaternionsAreRejected)
{
    auto blueprint = LogicBlueprint();
    blueprint["scriptComponents"][0]["properties"]["Ticks"] = 99;
    EXPECT_THROW(runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}}), std::exception);
    blueprint = LogicBlueprint();
    blueprint["scriptComponents"][0]["properties"]["Orientation"] = {0, 0, 0, 2};
    EXPECT_THROW(runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}}), std::exception);
}
#endif
}
