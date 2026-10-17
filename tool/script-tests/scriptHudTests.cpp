#include <gtest/gtest.h>
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "sceneNode.h"
#include "input/inputSubsystem.h"
#include "engine/diagnosticsSubsystem.h"

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
using namespace VL;
using Json = nlohmann::json;

class ScriptHudTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "Maps/SC_hud/SC_hud.json");
        world->SetCamera(std::make_shared<Camera>());
    }
    void TearDown() override { runtime.Shutdown(); }
    Json Blueprint(const std::string& id = "owner", const std::string& type = "ScriptFixtures.HudProbeBlueprint")
    {
        return {{"type", "blueprintInstance"}, {"id", id}, {"name", id}, {"class", type},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"properties", Json::object()}, {"components", Json::array()}};
    }
    Json Component(const std::string& id)
    {
        return {{"id", id}, {"class", "ScriptFixtures.HudProbeComponent"}, {"properties", Json::object()}};
    }
    void Prepare(const Json& declarations)
    {
        runtime.PrepareWorld(*world, {{"objects", declarations}});
        runtime.SynchronizeWorld(world);
    }
    void Control(ScriptPlayState state)
    {
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, state, error)) << error;
    }
    void Tick() { runtime.Tick(world, 1.0f / 60, {}, diagnostics); }
    std::vector<ScriptHudLine> Lines() { return runtime.View(world->GetGeneration()).hudLines; }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptHudTest, ValuesReachFrozenViewAndPauseKeepsThemUntilStop)
{
    auto blueprint = Blueprint();
    blueprint["properties"]["Mode"] = 1;
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    const auto frozen = runtime.View(1);
    ASSERT_EQ(frozen.hudLines.size(), 1u);
    EXPECT_EQ(frozen.hudLines[0].blueprintId, "owner");
    EXPECT_EQ(frozen.hudLines[0].key, "status");
    EXPECT_EQ(frozen.hudLines[0].text, "owner");
    Tick();
    ASSERT_EQ(Lines().size(), 1u);
    EXPECT_EQ(Lines()[0].text, "updated");
    EXPECT_EQ(frozen.hudLines[0].text, "owner");
    Control(ScriptPlayState::Paused);
    Tick();
    EXPECT_EQ(Lines()[0].text, "updated");
    Control(ScriptPlayState::Editing);
    EXPECT_TRUE(Lines().empty());
}

TEST_F(ScriptHudTest, SharedKeysRemainIsolatedBetweenBlueprintAndComponents)
{
    auto blueprint = Blueprint();
    blueprint["scriptComponents"] = Json::array({Component("first"), Component("second")});
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    const auto lines = Lines();
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].componentId, "");
    EXPECT_EQ(lines[0].text, "owner");
    EXPECT_EQ(lines[1].componentId, "first");
    EXPECT_EQ(lines[1].text, "first");
    EXPECT_EQ(lines[2].componentId, "second");
    EXPECT_EQ(lines[2].text, "second");
}

TEST_F(ScriptHudTest, RemoveOnlyDeletesTheCurrentPublisherKey)
{
    auto blueprint = Blueprint();
    blueprint["properties"]["Mode"] = 2;
    blueprint["scriptComponents"] = Json::array({Component("first")});
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    Tick();
    const auto lines = Lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].componentId, "first");
}

TEST_F(ScriptHudTest, DisabledOrRemovedComponentReleasesItsLinesWithoutClearingSiblingValues)
{
    for (int mode : {4, 5})
    {
        auto blueprint = Blueprint();
        blueprint["properties"]["Mode"] = mode;
        blueprint["scriptComponents"] = Json::array({Component("first"), Component("second")});
        Prepare(Json::array({blueprint}));
        Control(ScriptPlayState::Playing);
        Tick();
        const auto lines = Lines();
        ASSERT_EQ(lines.size(), 2u);
        EXPECT_EQ(lines[0].componentId, "");
        EXPECT_EQ(lines[1].componentId, "second");
        Control(ScriptPlayState::Editing);
    }
}

TEST_F(ScriptHudTest, ComponentFailureDiscardsChangesAndOnlyClearsItsOwnPublishedLines)
{
    auto blueprint = Blueprint();
    auto bad = Component("first");
    bad["properties"]["FailTick"] = true;
    blueprint["scriptComponents"] = Json::array({bad, Component("second")});
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    Tick();
    const auto lines = Lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text, "owner");
    EXPECT_EQ(lines[1].text, "second");
}

TEST_F(ScriptHudTest, OwnerFailureOrDestroyClearsAllOwnedLines)
{
    for (int mode : {3, 6})
    {
        auto blueprint = Blueprint();
        blueprint["properties"]["Mode"] = mode;
        blueprint["scriptComponents"] = Json::array({Component("first")});
        Prepare(Json::array({blueprint}));
        Control(ScriptPlayState::Playing);
        Tick();
        EXPECT_TRUE(Lines().empty());
        Control(ScriptPlayState::Editing);
    }
}

TEST_F(ScriptHudTest, CreateAndForeignOwnerCallsCannotPublishHud)
{
    EXPECT_THROW(Prepare(Json::array({Blueprint("create", "ScriptFixtures.HudCreateAccessBlueprint")})), std::exception);
    Prepare(Json::array({Blueprint("first", "ScriptFixtures.HudForeignAccessBlueprint"),
        Blueprint("second", "ScriptFixtures.HudForeignAccessBlueprint")}));
    Control(ScriptPlayState::Playing);
    EXPECT_TRUE(Lines().empty());
    EXPECT_EQ(world->GetScriptBlueprintStates()[1]["position"], Json::array({1, 0, 0}));
}

TEST_F(ScriptHudTest, PriorPlayHudServiceRejectsUseFromNewContext)
{
    auto blueprint = Blueprint("owner", "ScriptFixtures.HudStaleAccessBlueprint");
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    Control(ScriptPlayState::Editing);
    blueprint["properties"]["UseSaved"] = true;
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    EXPECT_TRUE(Lines().empty());
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"], Json::array({1, 0, 0}));
}

TEST_F(ScriptHudTest, WorkerCannotUseHudWhileOwningGameThreadCallbackIsActive)
{
    Prepare(Json::array({Blueprint("owner", "ScriptFixtures.HudThreadAccessBlueprint")}));
    Control(ScriptPlayState::Playing);
    EXPECT_TRUE(Lines().empty());
    EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"], Json::array({1, 0, 0}));
}

TEST_F(ScriptHudTest, WorldReplacementClearsOldHudAndRejectsOldContextCalls)
{
    Prepare(Json::array({Blueprint()}));
    Control(ScriptPlayState::Playing);
    const auto old = world->GetScriptContext();
    const auto frozen = runtime.View(1);
    ASSERT_EQ(frozen.hudLines.size(), 1u);
    world = std::make_shared<World>(2, "Maps/SC_next/SC_next.json");
    runtime.SynchronizeWorld(world);
    EXPECT_TRUE(Lines().empty());
    EXPECT_EQ(frozen.hudLines[0].text, "owner");
    EXPECT_THROW(old->Call("runtime_snapshot"), std::exception);
}

TEST_F(ScriptHudTest, InvalidKeyAndOversizedTextFailOnlyThePublishingOwner)
{
    for (const auto& value : std::vector<std::pair<std::string, std::string>>{
        {"", "text"}, {std::string(65, 'k'), "text"}, {"key", std::string(1025, 't')}})
    {
        auto bad = Blueprint("bad", "ScriptFixtures.HudInvalidBlueprint");
        bad["properties"] = {{"Key", value.first}, {"Text", value.second}};
        Prepare(Json::array({bad, Blueprint("good")}));
        Control(ScriptPlayState::Playing);
        const auto lines = Lines();
        ASSERT_EQ(lines.size(), 1u);
        EXPECT_EQ(lines[0].blueprintId, "good");
        Control(ScriptPlayState::Editing);
    }
}

TEST_F(ScriptHudTest, PublisherAndBlueprintBudgetsLeaveOtherOwnersUsable)
{
    auto bad = Blueprint("bad", "ScriptFixtures.HudInvalidBlueprint");
    bad["properties"]["Lines"] = 9;
    Prepare(Json::array({bad, Blueprint("good")}));
    Control(ScriptPlayState::Playing);
    ASSERT_EQ(Lines().size(), 1u);
    EXPECT_EQ(Lines()[0].blueprintId, "good");
    Control(ScriptPlayState::Editing);

    auto full = Blueprint("full", "ScriptFixtures.HudInvalidBlueprint");
    full["properties"]["Lines"] = 8;
    auto components = Json::array();
    for (int index = 0; index < 4; ++index)
    {
        auto component = Component("logic-" + std::to_string(index));
        component["properties"]["Lines"] = 8;
        components.push_back(component);
    }
    full["scriptComponents"] = components;
    Prepare(Json::array({full, Blueprint("good")}));
    Control(ScriptPlayState::Playing);
    EXPECT_EQ(Lines().size(), 33u);
}

TEST_F(ScriptHudTest, ContextBudgetRejectsTheOverflowingOwnerWithoutDroppingPreviousLines)
{
    auto blueprints = Json::array();
    for (int index = 0; index < 17; ++index)
    {
        auto blueprint = Blueprint("owner-" + std::to_string(index), "ScriptFixtures.HudInvalidBlueprint");
        blueprint["properties"]["Lines"] = 8;
        blueprints.push_back(blueprint);
    }
    Prepare(blueprints);
    Control(ScriptPlayState::Playing);
    EXPECT_EQ(Lines().size(), 128u);
}

TEST_F(ScriptHudTest, ProductionAmmoSampleUpdatesFromInputWithoutNativeGameplayCode)
{
    auto blueprint = Blueprint("ammo", "Gameplay.HudStateBlueprint");
    blueprint["properties"]["StartingAmmo"] = 2;
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    ASSERT_EQ(Lines().size(), 1u);
    EXPECT_EQ(Lines()[0].text, "Ammo: 2");
    InputActionState input;
    input.gameplay = {{"actions", {{"Fire", {{"held", true}, {"pressed", true}, {"released", false}}}}},
        {"axes", Json::object()}};
    runtime.Tick(world, 1.0f / 60, input, diagnostics);
    EXPECT_EQ(Lines()[0].text, "Ammo: 1");
}

TEST_F(ScriptHudTest, PausedReloadRebuildsHudFromExplicitMigratedGameplayState)
{
    auto blueprint = Blueprint("ammo", "Gameplay.HudStateBlueprint");
    blueprint["properties"]["StartingAmmo"] = 2;
    Prepare(Json::array({blueprint}));
    Control(ScriptPlayState::Playing);
    InputActionState input;
    input.gameplay = {{"actions", {{"Fire", {{"held", true}, {"pressed", true}, {"released", false}}}}},
        {"axes", Json::object()}};
    runtime.Tick(world, 1.0f / 60, input, diagnostics);
    Control(ScriptPlayState::Paused);
    const auto old = world->GetScriptContext();
    std::string error;
    ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
    EXPECT_EQ(runtime.View(1).state, ScriptPlayState::Paused);
    ASSERT_EQ(Lines().size(), 1u);
    EXPECT_EQ(Lines()[0].text, "Ammo: 1");
    EXPECT_THROW(old->Call("runtime_snapshot"), std::exception);
}
}
#endif
