#include <gtest/gtest.h>
#include <algorithm>
#include "script/scriptHost.h"

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
using namespace VL;
using Json = nlohmann::json;

class ScriptCustomDataTest : public testing::Test
{
protected:
    void SetUp() override
    {
        host.Initialize(VL_SCRIPT_OUTPUT_DIRECTORY);
        catalog = Load("ScriptFixtures.dll");
    }
    void TearDown() override
    {
        for (const auto context : contexts)
            try { Call(context, "release", {{"reason", 0}}); } catch (...) {}
        for (const auto generation : generations)
            try { host.Invoke({{"op", "unload"}, {"generation", generation}}); } catch (...) {}
    }
    Json Load(const std::string& path)
    {
        auto result = host.Invoke({{"op", "initialize"},
            {"assembly", std::string(VL_SCRIPT_OUTPUT_DIRECTORY) + "/" + path}}).at("result");
        generations.push_back(result.at("generation"));
        return result;
    }
    Json Blueprint(const std::string& type, const std::string& id = "data")
    {
        return {{"id", id}, {"name", id}, {"class", type}, {"properties", Json::object()},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"components", Json::array()}};
    }
    uint64_t Prepare(const Json& blueprints, uint64_t generation = 0)
    {
        const auto context = ++nextContext;
        host.Invoke({{"op", "prepare"}, {"context", context},
            {"generation", generation ? generation : catalog.at("generation").get<uint64_t>()},
            {"blueprints", blueprints}});
        contexts.push_back(context);
        return context;
    }
    Json Call(uint64_t context, const char* operation, Json fields = Json::object())
    {
        fields["op"] = operation; fields["context"] = context;
        return host.Invoke(std::move(fields)).at("result");
    }
    Json Tick(uint64_t context)
    {
        return Call(context, "tick", {{"dt", 0.1}, {"input", {0, 0, 0}}});
    }
    Json Capture(uint64_t context, const Json& states)
    {
        auto snapshot = Call(context, "migration_capture");
        snapshot["states"] = states;
        return snapshot;
    }
    Json MigratedData(uint64_t context, const char* propertyName)
    {
        auto snapshot = Call(context, "migration_capture");
        const auto& properties = snapshot.at("instances").at(0).at("properties");
        const auto property = std::find_if(properties.begin(), properties.end(),
            [propertyName](const Json& candidate)
            {
                return candidate.at("name") == propertyName;
            });
        if (property == properties.end())
        {
            ADD_FAILURE() << "Missing migrated property: " << propertyName;
            return Json::object();
        }
        return property->at("value");
    }
    Json Property(const std::string& type, const std::string& name)
    {
        for (const auto& item : catalog.at("classes"))
            if (item.at("name") == type)
                for (const auto& property : item.at("properties"))
                    if (property.at("name") == name) return property;
        return nullptr;
    }
    void ExpectPrepareError(Json blueprint, const std::string& path)
    {
        try { Prepare(Json::array({blueprint})); FAIL() << "Expected data validation failure"; }
        catch (const std::exception& error) { EXPECT_NE(std::string(error.what()).find(path), std::string::npos) << error.what(); }
    }
    ScriptHost host;
    Json catalog;
    std::vector<uint64_t> contexts, generations;
    inline static uint64_t nextContext = 900000;
};

TEST_F(ScriptCustomDataTest, CatalogDescribesNestedObjectsContainersAndMaterializedDefaults)
{
    const auto settings = Property("ScriptFixtures.CustomDataBlueprint", "Settings");
    ASSERT_EQ(settings.at("type"), "object");
    EXPECT_EQ(settings.at("typeName"), "ScriptFixtures.DataSettings");
    EXPECT_EQ(settings.at("value").at("Stats").at("Damage"), 12);
    EXPECT_EQ(settings.at("value").at("Stats").at("Offset"), Json::array({1, 2, 3}));
    EXPECT_FALSE(settings.at("value").contains("Ignored"));
    EXPECT_FALSE(settings.at("value").contains("IgnoredField"));
    EXPECT_EQ(Property("ScriptFixtures.CustomDataBlueprint", "Empty").at("value"), settings.at("value"));
    EXPECT_EQ(Property("ScriptFixtures.CustomDataBlueprint", "EmptyList").at("value"), Json::array());
    EXPECT_EQ(Property("ScriptFixtures.CustomDataBlueprint", "EmptyDictionary").at("value"), Json::object());
    for (const auto& field : settings.at("fields"))
        if (field.at("name") == "Points")
        {
            EXPECT_EQ(field.at("type"), "array");
            EXPECT_EQ(field.at("element").at("type"), "vector3");
        }
}

TEST_F(ScriptCustomDataTest, SparseObjectOverridesMergeDeclaredDefaultsAndContainersReplace)
{
    auto blueprint = Blueprint("ScriptFixtures.CustomDataBlueprint");
    blueprint["properties"]["Settings"] = {{"Stats", {{"Damage", 42}}},
        {"Points", Json::array({Json::array({9, 8, 7})})}, {"Slots", {{"base", {{"Damage", 23}}}}}};
    const auto context = Prepare(Json::array({blueprint}));
    const auto states = Call(context, "start");
    EXPECT_EQ(states.at(0).at("position"), Json::array({42, 9, 23}));
    const auto value = MigratedData(context, "Settings");
    EXPECT_EQ(value.at("Stats").at("Offset"), Json::array({1, 2, 3}));
    EXPECT_EQ(value.at("Weights"), Json::array({1, 2}));
    EXPECT_EQ(value.at("Points").size(), 1);
    EXPECT_FALSE(value.at("Slots").contains("removed"));
    EXPECT_EQ(value.at("Slots").at("base").at("Offset"), Json::array({1, 2, 3}));
}

TEST_F(ScriptCustomDataTest, DirectHostAndDynamicComponentsCopySharedDefaultReferencesPerInstance)
{
    auto first = Blueprint("ScriptFixtures.CustomDataBlueprint", "first");
    first["name"] = "mutate";
    auto second = Blueprint("ScriptFixtures.CustomDataBlueprint", "second");
    const auto context = Prepare(Json::array({first, second}));
    const auto states = Call(context, "start");
    EXPECT_EQ(states.at(0).at("position"), Json::array({12, 4, 12}));
    EXPECT_EQ(states.at(1).at("position"), Json::array({12, 4, 12}));
    const auto dynamic = Prepare(Json::array({Blueprint("ScriptFixtures.DynamicDataBlueprint")}));
    Call(dynamic, "start");
    const auto tick = Tick(dynamic);
    const auto& components = tick.at(0).at("scriptComponents");
    ASSERT_EQ(components.size(), 3);
    EXPECT_EQ(components.at(0).at("properties").at("Observed"), 4);
    EXPECT_EQ(components.at(1).at("properties").at("Observed"), 4);
    EXPECT_EQ(components.at(0).at("properties").at("Settings").at("Points").at(0), Json::array({4, 5, 6}));
    EXPECT_EQ(components.at(2).at("properties").at("Observed"), 13);
    EXPECT_EQ(components.at(2).at("properties").at("Settings").at("Stats").at("Offset"), Json::array({9, 8, 7}));
}

TEST_F(ScriptCustomDataTest, RejectsUnknownNullAndInvalidNestedLeafValuesWithPaths)
{
    for (const auto& entry : std::vector<std::pair<Json, std::string>>{
        {{{"Stats", {{"Missing", 3}}}}, "Settings.Stats.Missing"},
        {{{"Stats", nullptr}}, "Settings.Stats"},
        {{{"Orientation", {0, 0, 0, 2}}}, "Settings.Orientation"},
        {{{"Kind", "Unknown"}}, "Settings.Kind"},
        {{{"Asset", "../outside.json"}}, "Settings.Asset"},
        {{{"Points", Json::array({Json::array({1, 2})})}}, "Settings.Points[0]"}})
    {
        auto blueprint = Blueprint("ScriptFixtures.CustomDataBlueprint");
        blueprint["properties"]["Settings"] = entry.first;
        ExpectPrepareError(blueprint, entry.second);
    }
}

TEST_F(ScriptCustomDataTest, EnforcesContainerAndCompletePropertyNodeBudgets)
{
    auto blueprint = Blueprint("ScriptFixtures.CustomDataBlueprint");
    blueprint["properties"]["Entries"] = Json::array();
    for (int index = 0; index < 1025; ++index) blueprint["properties"]["Entries"].push_back(Json::object());
    ExpectPrepareError(blueprint, "Entries");
    blueprint["properties"].clear();
    blueprint["properties"]["Grid"] = Json::array();
    for (int row = 0; row < 8; ++row)
    {
        auto values = Json::array();
        for (int column = 0; column < 512; ++column) values.push_back(column);
        blueprint["properties"]["Grid"].push_back(std::move(values));
    }
    ExpectPrepareError(blueprint, "4096 nodes");
    blueprint["properties"].clear();
    auto entries = Json::array();
    for (int index = 0; index < 1024; ++index) entries.push_back(Json::object());
    blueprint["properties"]["Entries"] = std::move(entries);
    EXPECT_NO_THROW(Prepare(Json::array({blueprint})));
}

TEST_F(ScriptCustomDataTest, InvalidMutableRuntimeDataDoesNotStopOrdinaryFramesAndRejectsMigration)
{
    for (int mode : {0, 1, 2})
    {
        auto blueprint = Blueprint("ScriptFixtures.RuntimeInvalidDataBlueprint");
        blueprint["scriptComponents"] = Json::array({{{"id", "data"},
            {"class", "ScriptFixtures.RuntimeInvalidDataComponent"}, {"properties", {{"InvalidMode", mode}}}}});
        const auto context = Prepare(Json::array({blueprint}));
        Call(context, "start");
        auto states = Tick(context);
        EXPECT_FALSE(states.at(0).at("failed"));
        EXPECT_FALSE(states.at(0).at("scriptComponents").at(0).at("failed"));
        states = Tick(context);
        EXPECT_NEAR(states.at(0).at("position").at(0).get<float>(), 0.2f, 1e-5f);
        EXPECT_THROW(Call(context, "migration_capture"), std::exception);
        EXPECT_FALSE(Tick(context).at(0).at("failed"));
    }
}

TEST_F(ScriptCustomDataTest, RejectsRecursiveUnsupportedAndPolymorphicMigrationData)
{
    for (const char* name : {"UnsupportedMigrationBlueprint", "NonStringDictionaryMigrationBlueprint",
        "DelegateMigrationBlueprint", "EngineObjectMigrationBlueprint", "MultidimensionalMigrationBlueprint",
        "PolymorphicMigrationBlueprint", "DeepMigrationBlueprint"})
    {
        const auto context = Prepare(Json::array({Blueprint(std::string("ScriptFixtures.") + name)}));
        Call(context, "start");
        try { Call(context, "migration_capture"); FAIL() << name; }
        catch (const std::exception& error) { EXPECT_NE(std::string(error.what()).find(".Data"), std::string::npos) << error.what(); }
    }
}

TEST_F(ScriptCustomDataTest, RejectsDuplicateBlueprintIdentityBeforePublishingContext)
{
    const auto blueprint = Blueprint("ScriptFixtures.CustomDataBlueprint");
    EXPECT_THROW(Prepare(Json::array({blueprint, blueprint})), std::exception);
}

TEST_F(ScriptCustomDataTest, MigrationCopiesNestedValuesAndAllowsAddedFieldsToUseNewDefaults)
{
    const auto oldCatalog = Load("reload-v1/ReloadFixtures.dll");
    const auto blueprint = Blueprint("ReloadFixtures.DataMigrationBlueprint");
    const auto previous = Prepare(Json::array({blueprint}), oldCatalog.at("generation"));
    Call(previous, "start");
    const auto states = Tick(previous);
    const auto snapshot = Capture(previous, states);
    const auto nextCatalog = Load("reload-v2/ReloadFixtures.dll");
    const auto candidate = Prepare(Json::array({blueprint}), nextCatalog.at("generation"));
    Call(candidate, "migration_restore", {{"snapshot", snapshot}});
    auto restored = MigratedData(candidate, "Data");
    EXPECT_EQ(restored.at("Counter"), 21);
    EXPECT_EQ(restored.at("Points").at(0), Json::array({2, 3, 4}));
    EXPECT_EQ(restored.at("Counts").at("initial"), 5);
    EXPECT_EQ(restored.at("Added"), 77);
    Tick(candidate);
    EXPECT_EQ(MigratedData(candidate, "Data").at("Counter"), 22);
    EXPECT_EQ(MigratedData(previous, "Data").at("Counter"), 21);
}

TEST_F(ScriptCustomDataTest, MigrationRejectsNestedFloatToDoubleChangeAndKeepsOldData)
{
    const auto oldCatalog = Load("reload-v1/ReloadFixtures.dll");
    const auto blueprint = Blueprint("ReloadFixtures.ChangedNestedMigrationBlueprint");
    const auto previous = Prepare(Json::array({blueprint}), oldCatalog.at("generation"));
    const auto states = Call(previous, "start");
    const auto snapshot = Capture(previous, states);
    const auto nextCatalog = Load("reload-v2/ReloadFixtures.dll");
    const auto candidate = Prepare(Json::array({blueprint}), nextCatalog.at("generation"));
    EXPECT_THROW(Call(candidate, "migration_restore", {{"snapshot", snapshot}}), std::exception);
    EXPECT_EQ(MigratedData(previous, "Data").at(0).at("Number"), 1);
}

TEST_F(ScriptCustomDataTest, NestedDataSchemaCachesDoNotKeepRetiredGameplayAssemblyAlive)
{
    const auto nestedCatalog = Load("reload-v1/ReloadFixtures.dll");
    const auto generation = nestedCatalog.at("generation").get<uint64_t>();
    const auto context = Prepare(Json::array({Blueprint("ReloadFixtures.DataMigrationBlueprint")}), generation);
    Call(context, "start");
    Tick(context);
    EXPECT_NO_THROW(MigratedData(context, "Data"));
    Call(context, "release", {{"reason", 0}});
    host.Invoke({{"op", "unload"}, {"generation", generation}});
    const auto status = host.Invoke({{"op", "assembly_status"}, {"collect", true}}).at("result");
    EXPECT_EQ(status.at("retiredAlive"), 0);
}

using ScriptCallbackMaskTest = ScriptCustomDataTest;
using ScriptDeltaTest = ScriptCustomDataTest;

TEST_F(ScriptDeltaTest, IdleDeltaIsEmptyAndExplicitMigrationExportRemainsComplete)
{
    const auto context = Prepare(Json::array({Blueprint("ScriptFixtures.PassiveBlueprint")}));
    ASSERT_EQ(Call(context, "start").size(), 1u);
    EXPECT_TRUE(Call(context, "tick", {{"dt", 0.1}, {"input", {0, 0, 0}}, {"delta", true}}).empty());
    EXPECT_EQ(Call(context, "migration_export").size(), 1u);
}

TEST_F(ScriptDeltaTest, MovementDeltaContainsOnlyIdentityAndChangedPosition)
{
    const auto context = Prepare(Json::array({Blueprint("ScriptFixtures.DeltaMovementBlueprint")}));
    Call(context, "start");
    const auto changes = Call(context, "tick", {{"dt", 0.1}, {"input", {0, 0, 0}}, {"delta", true}});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].size(), 2u);
    EXPECT_TRUE(changes[0].contains("id"));
    EXPECT_NEAR(changes[0]["position"][0].get<float>(), 0.1f, 1e-6f);
}

TEST_F(ScriptDeltaTest, PhysicsSynchronizationAdvancesDeltaBaselineBeforeCallback)
{
    const auto context = Prepare(Json::array({Blueprint("ScriptFixtures.DeltaResetBlueprint")}));
    auto state = Call(context, "start");
    state[0]["position"] = {10, 0, 0};
    const auto changes = Call(context, "tick", {{"states", state}, {"dt", 0.1}, {"input", {0, 0, 0}}, {"delta", true}});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0]["position"], Json::array({0, 0, 0}));
}

TEST_F(ScriptDeltaTest, EqualOneShotImpulsesAreDeliveredAgainButNotReplayedByIdlePhase)
{
    auto blueprint = Blueprint("ScriptFixtures.DeltaImpulseBlueprint");
    blueprint["physics"] = {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 10}, {"mass", 1}};
    const auto context = Prepare(Json::array({blueprint}));
    Call(context, "start");
    for (int index = 0; index < 2; ++index)
    {
        const auto changes = Call(context, "tick",
            {{"dt", 0.1}, {"input", {0, 0, 0}}, {"deferLate", true}, {"delta", true}});
        ASSERT_EQ(changes.size(), 1u);
        EXPECT_EQ(changes[0]["impulse"], Json::array({1, 0, 0}));
        // LateTick 的零步同步阶段不得把已交付的一次性冲量再播放一次。
        EXPECT_TRUE(Call(context, "late", {{"states", Json::array()}, {"dt", 0.1},
            {"contacts", Json::array()}, {"triggers", Json::array()}, {"rayHits", Json::array()},
            {"delta", true}}).empty());
    }
}

bool HasCallbackPhase(const Json& callbacks, const char* phase)
{
    return std::any_of(callbacks.begin(), callbacks.end(),
        [phase](const Json& callback) { return callback.at("phase") == phase; });
}

Json WithCallbackComponent(Json blueprint, const std::string& type)
{
    blueprint["scriptComponents"] = Json::array({{{"id", "logic"}, {"class", type},
        {"properties", Json::object()}}});
    return blueprint;
}

TEST_F(ScriptCallbackMaskTest, UnoverriddenFrameCallbacksDoNotProduceTimings)
{
    const auto context = Prepare(Json::array({WithCallbackComponent(
        Blueprint("ScriptFixtures.EmptyFrameBlueprint"), "ScriptFixtures.EmptyFrameComponent")}));
    Call(context, "start");
    Tick(context);
    const auto snapshot = Call(context, "runtime_snapshot");
    const auto& blueprint = snapshot.at("blueprints").at(0);
    const auto& component = blueprint.at("components").at(0);
    for (const auto* phase : {"Tick", "LateTick"})
    {
        EXPECT_FALSE(HasCallbackPhase(blueprint.at("callbacks"), phase));
        EXPECT_FALSE(HasCallbackPhase(component.at("callbacks"), phase));
    }
    EXPECT_TRUE(HasCallbackPhase(blueprint.at("callbacks"), "BeginPlay"));
    EXPECT_TRUE(HasCallbackPhase(component.at("callbacks"), "BeginPlay"));
}

TEST_F(ScriptCallbackMaskTest, InheritedAndHiddenOverridesPreserveVirtualDispatchForBlueprintsAndComponents)
{
    for (const auto* name : {"Inherited", "Hidden"})
        for (const bool component : {false, true})
        {
            auto blueprint = Blueprint(component ? "ScriptFixtures.EmptyFrameBlueprint" :
                std::string("ScriptFixtures.") + name + "FrameBlueprint");
            if (component) blueprint = WithCallbackComponent(std::move(blueprint),
                std::string("ScriptFixtures.") + name + "FrameComponent");
            const auto context = Prepare(Json::array({blueprint}));
            Call(context, "start");
            const auto states = Tick(context);
            EXPECT_FALSE(states.at(0).at("failed"));
            EXPECT_EQ(states.at(0).at("position"), Json::array({1, 0, 1}));
            const auto snapshot = Call(context, "runtime_snapshot");
            const auto& owner = snapshot.at("blueprints").at(0);
            const auto& callbacks = component ? owner.at("components").at(0).at("callbacks") : owner.at("callbacks");
            for (const auto* phase : {"Tick", "LateTick"}) EXPECT_TRUE(HasCallbackPhase(callbacks, phase));
        }
}

TEST_F(ScriptCallbackMaskTest, UserAuthoredEmptyOverridesAreStillInvokedAndTimed)
{
    const auto context = Prepare(Json::array({WithCallbackComponent(
        Blueprint("ScriptFixtures.ExplicitEmptyFrameBlueprint"), "ScriptFixtures.ExplicitEmptyFrameComponent")}));
    Call(context, "start");
    Tick(context);
    const auto snapshot = Call(context, "runtime_snapshot");
    const auto& blueprint = snapshot.at("blueprints").at(0);
    for (const auto* phase : {"Tick", "LateTick"})
    {
        EXPECT_TRUE(HasCallbackPhase(blueprint.at("callbacks"), phase));
        EXPECT_TRUE(HasCallbackPhase(blueprint.at("components").at(0).at("callbacks"), phase));
    }
}

TEST_F(ScriptCallbackMaskTest, TimerAndEventCallbacksRunWithoutFrameOverrides)
{
    const auto context = Prepare(Json::array({WithCallbackComponent(
        Blueprint("ScriptFixtures.FrameServicesBlueprint"), "ScriptFixtures.FrameServicesComponent")}));
    Call(context, "start");
    const auto states = Tick(context);
    const auto& owner = states.at(0);
    EXPECT_EQ(owner.at("position"), Json::array({1, 1, 0}));
    EXPECT_EQ(owner.at("scriptComponents").at(0).at("properties").at("TimerCalls"), 1);
    EXPECT_EQ(owner.at("scriptComponents").at(0).at("properties").at("EventCalls"), 1);
}

TEST_F(ScriptCallbackMaskTest, EmptyFrameComponentStillEndsAndCleansServicesWhenDisabled)
{
    const auto context = Prepare(Json::array({WithCallbackComponent(
        Blueprint("ScriptFixtures.DisableEmptyFrameComponentBlueprint"), "ScriptFixtures.EmptyFrameLifecycleComponent")}));
    Call(context, "start");
    const auto states = Tick(context);
    const auto& component = states.at(0).at("scriptComponents").at(0);
    EXPECT_FALSE(component.at("started"));
    EXPECT_EQ(component.at("properties").at("Begins"), 1);
    EXPECT_EQ(component.at("properties").at("Ends"), 1);
    const auto snapshot = Call(context, "runtime_snapshot");
    EXPECT_EQ(snapshot.at("pending").at("timers"), 0);
    EXPECT_EQ(snapshot.at("pending").at("subscriptions"), 0);
    EXPECT_TRUE(HasCallbackPhase(snapshot.at("blueprints").at(0).at("components").at(0).at("callbacks"), "EndPlay"));
}

TEST_F(ScriptCallbackMaskTest, InheritedThrowingCallbacksKeepRollbackAndFailureIsolation)
{
    for (const bool component : {false, true})
    {
        auto blueprint = Blueprint(component ? "ScriptFixtures.EmptyFrameBlueprint" : "ScriptFixtures.ThrowingFrameBlueprint");
        if (component) blueprint = WithCallbackComponent(std::move(blueprint), "ScriptFixtures.ThrowingFrameComponent");
        const auto context = Prepare(Json::array({blueprint}));
        Call(context, "start");
        const auto states = Tick(context);
        EXPECT_EQ(states.at(0).at("failed"), !component);
        EXPECT_EQ(states.at(0).at("position"), Json::array({0, 0, 0}));
        if (component) EXPECT_TRUE(states.at(0).at("scriptComponents").at(0).at("failed"));
        const auto snapshot = Call(context, "runtime_snapshot");
        EXPECT_EQ(snapshot.at("pending").at("timers"), 0);
    }
}
}
#endif
