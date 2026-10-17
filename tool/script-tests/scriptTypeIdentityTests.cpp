#include <gtest/gtest.h>
#include <limits>
#include "script/scriptCompleteDeclaration.h"
#include "script/scriptRuntime.h"
#include "world/world.h"
#include "engine/diagnosticsSubsystem.h"
#include "input/inputSubsystem.h"
#include "sceneNode.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

TEST(ScriptTypeIdentity, MatchesRegisteredSimpleAndFullAssemblyNamesWithoutRequiringCommaWhitespace)
{
    const Json type{{"name", "Gameplay.PlayerBlueprint"}, {"assemblyName", "Gameplay"},
        {"assemblyQualifiedName", "Gameplay.PlayerBlueprint, Gameplay, Version=1.0.0.0, Culture=neutral, PublicKeyToken=null"}};
    EXPECT_TRUE(ScriptTypeNameMatches(type, "Gameplay.PlayerBlueprint"));
    EXPECT_TRUE(ScriptTypeNameMatches(type, "Gameplay.PlayerBlueprint, Gameplay"));
    EXPECT_TRUE(ScriptTypeNameMatches(type, "Gameplay.PlayerBlueprint,Gameplay"));
    EXPECT_TRUE(ScriptTypeNameMatches(type,
        "Gameplay.PlayerBlueprint, Gameplay,Version=1.0.0.0,Culture=neutral, PublicKeyToken=null"));
}

TEST(ScriptTypeIdentity, RejectsOtherAssembliesVersionsAndUnregisteredIdentityFields)
{
    const Json type{{"name", "Gameplay.PlayerBlueprint"}, {"assemblyName", "Gameplay"},
        {"assemblyQualifiedName", "Gameplay.PlayerBlueprint, Gameplay, Version=1.0.0.0, Culture=neutral, PublicKeyToken=null"}};
    EXPECT_FALSE(ScriptTypeNameMatches(type, "Gameplay.PlayerBlueprint, VulkanLearn.Runtime"));
    EXPECT_FALSE(ScriptTypeNameMatches(type,
        "Gameplay.PlayerBlueprint, Gameplay, Version=2.0.0.0, Culture=neutral, PublicKeyToken=null"));
    EXPECT_FALSE(ScriptTypeNameMatches(type, "Gameplay.PlayerBlueprint, Gameplay, Unknown=true"));
    EXPECT_FALSE(ScriptTypeNameMatches(type, "Gameplay.OtherBlueprint, Gameplay"));
}

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
Json IdentityBlueprint(const std::string& className)
{
    return {{"type", "blueprintInstance"}, {"id", "probe"}, {"name", "Probe"}, {"class", className},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", Json::object()}, {"components", Json::array()}};
}

class ScriptQualifiedTypeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        runtime.Initialize(true, VL_SCRIPT_OUTPUT_DIRECTORY,
            std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll");
        world = std::make_shared<World>(1, "qualified-types");
        world->SetCamera(std::make_shared<Camera>());
        runtime.SynchronizeWorld(world);
    }
    void TearDown() override { runtime.Shutdown(); }
    std::string Qualified(const Json& catalog, const std::string& name) const
    {
        for (const auto& type : catalog)
            if (type.at("name") == name) return type.at("assemblyQualifiedName");
        throw std::runtime_error("Missing qualified type fixture");
    }
    void Play(const Json& blueprint)
    {
        runtime.PrepareWorld(*world, {{"objects", Json::array({blueprint})}});
        std::string error;
        ASSERT_TRUE(runtime.Control(*world, ScriptPlayState::Playing, error)) << error;
    }
    ScriptRuntimeSubsystem runtime;
    std::shared_ptr<World> world;
    DiagnosticsSubsystem diagnostics;
};

TEST_F(ScriptQualifiedTypeTest, SimpleQualifiedBlueprintCompletesDefaultsAndBindsEditableOverrides)
{
    auto blueprint = IdentityBlueprint("ScriptFixtures.TypedPropertiesBlueprint, ScriptFixtures");
    blueprint["properties"]["Offset"] = {9, 8, 7};
    ASSERT_NO_FATAL_FAILURE(Play(blueprint));
    const auto& completed = world->GetScriptBlueprints().at(0);
    EXPECT_EQ(completed["properties"]["Planar"], Json::array({1, 2}));
    EXPECT_EQ(completed["properties"]["PrivateOffset"], Json::array({1, 1, 1}));
    EXPECT_EQ(world->GetScriptBlueprintStates().at(0)["position"], Json::array({10, 10, 16}));
    EXPECT_FALSE(world->GetScriptBlueprintStates().at(0)["failed"].get<bool>());
}

TEST_F(ScriptQualifiedTypeTest, FullyQualifiedBlueprintCannotBypassInlineInstancePermission)
{
    auto blueprint = IdentityBlueprint(Qualified(runtime.Classes(), "ScriptFixtures.TypedPropertiesBlueprint"));
    blueprint["properties"]["PrivateOffset"] = {9, 8, 7};
    EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::invalid_argument);
}

TEST_F(ScriptQualifiedTypeTest, FullyQualifiedBlueprintAndComponentExecuteWithCompletedDefaults)
{
    auto blueprint = IdentityBlueprint(Qualified(runtime.Classes(), "ScriptFixtures.PassiveBlueprint"));
    blueprint["scriptComponents"] = Json::array({{{"id", "counter"},
        {"class", Qualified(runtime.ComponentClasses(), "ScriptFixtures.CounterComponent")},
        {"properties", {{"Speed", 60}}}}});
    ASSERT_NO_FATAL_FAILURE(Play(blueprint));
    const auto& completed = world->GetScriptBlueprints().at(0)["scriptComponents"][0];
    EXPECT_EQ(completed["properties"]["Orientation"], Json::array({0, 0, 0, 1}));
    runtime.Tick(world, 1.0f / 60, {}, diagnostics);
    const auto& state = world->GetScriptBlueprintStates().at(0);
    EXPECT_FALSE(state["failed"].get<bool>());
    EXPECT_FALSE(state["scriptComponents"][0]["failed"].get<bool>());
    EXPECT_NEAR(state["position"][0].get<float>(), 1, 1e-5f);
    EXPECT_EQ(state["scriptComponents"][0]["properties"]["Ticks"], 1);
}

TEST_F(ScriptQualifiedTypeTest, QualifiedComponentCannotBypassInlineInstancePermission)
{
    auto blueprint = IdentityBlueprint("ScriptFixtures.PassiveBlueprint");
    blueprint["scriptComponents"] = Json::array({{{"id", "counter"},
        {"class", "ScriptFixtures.CounterComponent,ScriptFixtures"}, {"properties", {{"Ticks", 5}}}}});
    EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::invalid_argument);
}

TEST_F(ScriptQualifiedTypeTest, QualifiedDeclarationsSurvivePlayingAndPausedMigrationWithoutRewritingAuthorClasses)
{
    const auto rootClass = Qualified(runtime.Classes(), "ScriptFixtures.PassiveBlueprint");
    const auto componentClass = Qualified(runtime.ComponentClasses(), "ScriptFixtures.CounterComponent");
    auto blueprint = IdentityBlueprint(rootClass);
    blueprint["scriptComponents"] = Json::array({{{"id", "counter"}, {"class", componentClass},
        {"properties", {{"Speed", 60}}}}});
    ASSERT_NO_FATAL_FAILURE(Play(blueprint));
    runtime.Tick(world, 1.0f / 60, {}, diagnostics);
    const auto position = world->GetScriptBlueprintStates()[0]["position"];
    std::string error;
    for (const auto state : {ScriptPlayState::Playing, ScriptPlayState::Paused})
    {
        ASSERT_TRUE(runtime.Control(*world, state, error)) << error;
        const auto previous = world->GetScriptContext();
        ASSERT_TRUE(runtime.ReloadAssembly(*world, error)) << error;
        EXPECT_NE(world->GetScriptContext(), previous);
        EXPECT_EQ(runtime.View(world->GetGeneration()).state, state);
        EXPECT_EQ(world->GetScriptBlueprintStates()[0]["position"], position);
        EXPECT_EQ(world->GetScriptBlueprints()[0]["class"], rootClass);
        EXPECT_EQ(world->GetScriptBlueprints()[0]["scriptComponents"][0]["class"], componentClass);
        EXPECT_EQ(world->GetScriptBlueprintStates()[0]["scriptComponents"][0]["class"],
            "ScriptFixtures.CounterComponent");
    }
}

TEST_F(ScriptQualifiedTypeTest, NativeSchemaRejectsWrongAssemblyForBlueprintAndComponent)
{
    auto blueprint = IdentityBlueprint("ScriptFixtures.PassiveBlueprint, VulkanLearn.Runtime");
    EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::runtime_error);
    blueprint["class"] = "ScriptFixtures.PassiveBlueprint";
    blueprint["scriptComponents"] = Json::array({{{"id", "counter"},
        {"class", "ScriptFixtures.CounterComponent, VulkanLearn.Runtime"}, {"properties", Json::object()}}});
    EXPECT_THROW(runtime.ValidateBlueprint(blueprint), std::runtime_error);
}

TEST(ScriptTypeIdentityManaged, DirectPrepareRejectsWrongAssemblyWithoutLoadingAnExternalGeneration)
{
    ScriptHost host;
    host.Initialize(VL_SCRIPT_OUTPUT_DIRECTORY);
    const auto loaded = host.Invoke({{"op", "initialize"}, {"assembly",
        std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/ScriptFixtures.dll"}});
    const auto generation = loaded.at("result").at("generation");
    const auto before = host.Invoke({{"op", "assembly_status"}, {"collect", false}}).at("result").at("loaded");
    auto blueprint = IdentityBlueprint("ScriptFixtures.PassiveBlueprint, VulkanLearn.Runtime");
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const uint64_t context = std::numeric_limits<uint64_t>::max() - uint64_t(attempt);
        try
        {
            host.Invoke({{"op", "prepare"}, {"context", context}, {"generation", generation},
                {"blueprints", Json::array({blueprint})}, {"templates", Json::object()}});
            ADD_FAILURE() << "Unregistered assembly identity was accepted";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_NE(std::string(error.what()).find(attempt == 0 ? "Unknown Blueprint class" :
                "registered Component class"), std::string::npos) << error.what();
        }
        // 失败注入意外成功时也只清理由本用例保留的上下文，避免污染后续程序集回收验收。
        try { host.Invoke({{"op", "release"}, {"context", context}, {"reason", 0}}); }
        catch (const std::runtime_error&) {}
        blueprint["class"] = "ScriptFixtures.PassiveBlueprint";
        blueprint["scriptComponents"] = Json::array({{{"id", "counter"},
            {"class", "ScriptFixtures.CounterComponent, VulkanLearn.Runtime"}, {"properties", Json::object()}}});
    }
    EXPECT_EQ(host.Invoke({{"op", "assembly_status"}, {"collect", false}}).at("result").at("loaded"), before);
    host.Invoke({{"op", "unload"}, {"generation", generation}});
}
#endif
} // namespace
