#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleModuleEdit.h"

namespace
{

using namespace VL;
using Json = nlohmann::json;

size_t ModuleIndex(const Json& emitter, const std::string& type)
{
    const Json& modules = emitter.at("modules");
    for (size_t index = 0; index < modules.size(); ++index)
    {
        if (modules.at(index).at("type") == type) return index;
    }
    return modules.size();
}

const Json& Module(const Json& emitter, const std::string& type)
{
    return emitter.at("modules").at(ModuleIndex(emitter, type));
}

Json FlatEmitter()
{
    return Json{
        {"name", "fire"},
        {"position", {1.0, 2.0, 3.0}},
        {"maxParticles", 42},
        {"spawnRate", 12.0},
        {"velocity", {{"min", {0.0, 1.0, 0.0}},
            {"max", {0.0, 2.0, 0.0}}}},
        {"gravity", {0.0, -9.0, 0.0}},
        {"collision", {{"type", "plane"}}},
        {"mesh", {{"path", "Meshes/SM_test.json"}}},
        {"shape", {{"type", "sphere"}, {"radius", 2.0}}},
        {"customFlag", 7}};
}

} // namespace

TEST(ParticleModuleEdit, SpecsExposeStableOrderAndFieldOwnership)
{
    const std::vector<ParticleModuleSpec>& specs = ParticleModuleSpecs();
    ASSERT_FALSE(specs.empty());
    EXPECT_EQ(specs.front().type, "emitter");
    EXPECT_EQ(specs.front().label, "Emitter");

    ASSERT_NE(FindParticleModuleSpec("emitter"), nullptr);
    ASSERT_NE(FindParticleModuleSpec("forces.drag"), nullptr);
    EXPECT_EQ(FindParticleModuleSpec("forces.drag")->label, "Drag");
    EXPECT_EQ(FindParticleModuleSpec("spawn.typo"), nullptr);

    EXPECT_EQ(ParticleModuleTypeForField("maxParticles"), "emitter");
    EXPECT_EQ(ParticleModuleTypeForField("mesh.path"), "renderer");
    EXPECT_EQ(ParticleModuleTypeForField("shape.radius"),
        "spawn.shapeSphere");
    EXPECT_EQ(ParticleModuleTypeForField("velocity.min"),
        "spawn.velocity");
    EXPECT_EQ(ParticleModuleTypeForField("collision.plane.normal"),
        "collision.analytic");
    EXPECT_EQ(ParticleModuleTypeForField("flipbook.fps"), "flipbook");
    EXPECT_EQ(ParticleModuleTypeForField("lodDistance"), "lod");
    EXPECT_EQ(ParticleModuleTypeForField("fixedBounds.center"), "bounds");
    EXPECT_EQ(ParticleModuleTypeForField("name"), std::string());
    EXPECT_EQ(ParticleModuleTypeForField("position"), std::string());
    EXPECT_EQ(ParticleModuleTypeForField("unknown"), std::string());
}

TEST(ParticleModuleEdit, EnsureMigratesFlatFieldsAndPreservesBaseFields)
{
    Json emitter = FlatEmitter();
    ASSERT_TRUE(EnsureParticleModules(emitter));

    EXPECT_EQ(emitter.at("name"), "fire");
    EXPECT_EQ(emitter.at("position"), Json({1.0, 2.0, 3.0}));
    EXPECT_EQ(emitter.at("customFlag"), 7);
    EXPECT_FALSE(emitter.contains("maxParticles"));
    EXPECT_FALSE(emitter.contains("spawnRate"));
    EXPECT_FALSE(emitter.contains("collision"));
    EXPECT_FALSE(emitter.contains("mesh"));
    EXPECT_FALSE(emitter.contains("shape"));

    ASSERT_LT(ModuleIndex(emitter, "emitter"),
        emitter.at("modules").size());
    EXPECT_EQ(Module(emitter, "emitter").at("fields").at("maxParticles"),
        42);
    EXPECT_EQ(Module(emitter, "spawn.rate").at("fields").at("spawnRate"),
        12.0);
    EXPECT_EQ(Module(emitter, "renderer").at("fields").at("mesh").at("path"),
        "Meshes/SM_test.json");
    EXPECT_EQ(
        Module(emitter, "spawn.shapeSphere").at("fields").at("shape")
            .at("radius"),
        2.0);
    EXPECT_EQ(
        Module(emitter, "collision.analytic").at("fields")
            .at("collision").at("type"),
        "plane");
    EXPECT_FALSE(emitter.contains("modules") &&
        ModuleIndex(emitter, "curves") < emitter.at("modules").size());

    const Json once = emitter;
    ASSERT_TRUE(EnsureParticleModules(emitter));
    EXPECT_EQ(emitter, once);
}

TEST(ParticleModuleEdit, EnsureRejectsMalformedInputWithoutMutation)
{
    Json nonObject = Json::array();
    const Json nonObjectBefore = nonObject;
    EXPECT_FALSE(EnsureParticleModules(nonObject));
    EXPECT_EQ(nonObject, nonObjectBefore);

    Json badModules = Json{{"name", "e"}, {"modules", 3}};
    const Json badModulesBefore = badModules;
    EXPECT_FALSE(EnsureParticleModules(badModules));
    EXPECT_EQ(badModules, badModulesBefore);
}

TEST(ParticleModuleEdit, ForcesModulesExposeSpecsAndFieldOwnership)
{
    const ParticleModuleSpec* curlNoise =
        FindParticleModuleSpec("forces.curlNoise");
    const ParticleModuleSpec* vortex =
        FindParticleModuleSpec("forces.vortex");
    const ParticleModuleSpec* attractor =
        FindParticleModuleSpec("forces.attractor");
    const ParticleModuleSpec* wind =
        FindParticleModuleSpec("forces.wind");
    ASSERT_NE(curlNoise, nullptr);
    ASSERT_NE(vortex, nullptr);
    ASSERT_NE(attractor, nullptr);
    ASSERT_NE(wind, nullptr);

    EXPECT_NE(
        std::find(curlNoise->fields.begin(), curlNoise->fields.end(),
            "curlNoise"),
        curlNoise->fields.end());
    EXPECT_NE(
        std::find(vortex->fields.begin(), vortex->fields.end(), "vortex"),
        vortex->fields.end());
    EXPECT_NE(
        std::find(attractor->fields.begin(), attractor->fields.end(),
            "attractor"),
        attractor->fields.end());
    EXPECT_NE(
        std::find(wind->fields.begin(), wind->fields.end(), "wind"),
        wind->fields.end());

    EXPECT_EQ(ParticleModuleTypeForField("curlNoise.strength"),
        "forces.curlNoise");
    EXPECT_EQ(ParticleModuleTypeForField("curlNoise.frequency"),
        "forces.curlNoise");
    EXPECT_EQ(ParticleModuleTypeForField("vortex.axis"),
        "forces.vortex");
    EXPECT_EQ(ParticleModuleTypeForField("attractor.position"),
        "forces.attractor");
    EXPECT_EQ(ParticleModuleTypeForField("wind"),
        "forces.wind");
}

TEST(ParticleModuleEdit, EnsureAndFlattenMigrateForcesFields)
{
    Json emitter = Json{
        {"name", "e"},
        {"curlNoise", {
            {"strength", 3.0},
            {"frequency", 0.25},
            {"speed", 1.5}}},
        {"vortex", {
            {"strength", 4.0},
            {"radius", 75.0},
            {"axis", {0.0, 1.0, 0.0}}}},
        {"attractor", {
            {"position", {1.0, 2.0, 3.0}},
            {"strength", -5.0},
            {"radius", 120.0}}},
        {"wind", {6.0, 0.0, -2.0}}};

    ASSERT_TRUE(EnsureParticleModules(emitter));
    EXPECT_FALSE(emitter.contains("curlNoise"));
    EXPECT_FALSE(emitter.contains("vortex"));
    EXPECT_FALSE(emitter.contains("attractor"));
    EXPECT_FALSE(emitter.contains("wind"));
    EXPECT_EQ(
        Module(emitter, "forces.curlNoise").at("fields")
            .at("curlNoise").at("frequency"),
        0.25);
    EXPECT_EQ(
        Module(emitter, "forces.vortex").at("fields")
            .at("vortex").at("radius"),
        75.0);
    EXPECT_EQ(
        Module(emitter, "forces.attractor").at("fields")
            .at("attractor").at("strength"),
        -5.0);
    EXPECT_EQ(
        Module(emitter, "forces.wind").at("fields").at("wind"),
        Json({6.0, 0.0, -2.0}));

    const Json flattened = FlattenParticleEmitter(emitter);
    EXPECT_FALSE(flattened.contains("modules"));
    EXPECT_EQ(flattened.at("curlNoise").at("speed"), 1.5);
    EXPECT_EQ(flattened.at("vortex").at("axis"),
        Json({0.0, 1.0, 0.0}));
    EXPECT_EQ(flattened.at("attractor").at("position"),
        Json({1.0, 2.0, 3.0}));
    EXPECT_EQ(flattened.at("wind"), Json({6.0, 0.0, -2.0}));
}

TEST(ParticleModuleEdit, SetFieldWritesForcesModuleFields)
{
    Json emitter = Json{{"name", "e"}};
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "curlNoise.frequency", Json(0.4), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "vortex.axis", Json({0.0, 0.0, 1.0}), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "attractor.position", Json({4.0, 5.0, 6.0}), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "wind", Json({1.0, 0.0, 0.0}), false));

    EXPECT_EQ(
        Module(emitter, "forces.curlNoise").at("fields")
            .at("curlNoise").at("frequency"),
        0.4);
    EXPECT_EQ(
        Module(emitter, "forces.vortex").at("fields")
            .at("vortex").at("axis"),
        Json({0.0, 0.0, 1.0}));
    EXPECT_EQ(
        Module(emitter, "forces.attractor").at("fields")
            .at("attractor").at("position"),
        Json({4.0, 5.0, 6.0}));
    EXPECT_EQ(
        Module(emitter, "forces.wind").at("fields").at("wind"),
        Json({1.0, 0.0, 0.0}));
}

TEST(ParticleModuleEdit, FlattenMergesEnabledModulesInOrder)
{
    Json emitter = Json{
        {"name", "e"},
        {"maxParticles", 8},
        {"modules", Json::array({
            Json{{"type", "emitter"},
                {"fields", {{"maxParticles", 16}}}},
            Json{{"type", "spawn.rate"},
                {"fields", {{"spawnRate", 3.0}, {"burst", false}}}},
            Json{{"type", "spawn.rate"},
                {"fields", {{"spawnRate", 4.0}}}},
            Json{{"type", "collision.analytic"},
                {"fields", {{"collision", {{"plane", {
                    {"normal", {0.0, 1.0, 0.0}},
                    {"offset", 2.0}}}}}}}},
            Json{{"type", "forces.gravity"},
                {"enabled", false},
                {"fields", {{"gravity", {0.0, -1.0, 0.0}}}}},
            Json{{"type", "collision.analytic"},
                {"fields", {{"collision", {{"plane", {
                    {"offset", 7.0}}}}}}}},
        })}};
    const Json before = emitter;

    const Json flattened = FlattenParticleEmitter(emitter);
    EXPECT_EQ(emitter, before);
    EXPECT_FALSE(flattened.contains("modules"));
    EXPECT_EQ(flattened.at("maxParticles"), 16);
    EXPECT_EQ(flattened.at("spawnRate"), 4.0);
    EXPECT_EQ(flattened.at("burst"), false);
    EXPECT_FALSE(flattened.contains("gravity"));
    EXPECT_EQ(flattened.at("collision").at("plane").at("offset"), 7.0);
    EXPECT_EQ(flattened.at("collision").at("plane").at("normal"),
        Json({0.0, 1.0, 0.0}));
}

TEST(ParticleModuleEdit, FlattenMalformedInputIsUnchanged)
{
    Json nonArray = Json{{"name", "e"}, {"modules", 1}};
    const Json nonArrayBefore = nonArray;
    EXPECT_EQ(FlattenParticleEmitter(nonArray), nonArrayBefore);
    EXPECT_EQ(nonArray, nonArrayBefore);

    Json badModule = Json{
        {"name", "e"},
        {"modules", Json::array({Json{ {"type", "emitter"}, {"fields", 2} }})}};
    const Json badModuleBefore = badModule;
    EXPECT_EQ(FlattenParticleEmitter(badModule), badModuleBefore);
    EXPECT_EQ(badModule, badModuleBefore);

    Json nonObject = Json::array({1, 2});
    const Json nonObjectBefore = nonObject;
    EXPECT_EQ(FlattenParticleEmitter(nonObject), nonObjectBefore);
    EXPECT_EQ(nonObject, nonObjectBefore);
}

TEST(ParticleModuleEdit, SetFieldWritesModulePathAndResetErasesIt)
{
    Json emitter = FlatEmitter();
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "spawnRate", Json(25.0), false));
    EXPECT_EQ(Module(emitter, "spawn.rate").at("fields").at("spawnRate"),
        25.0);
    EXPECT_FALSE(emitter.contains("spawnRate"));

    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "maxParticles", Json(), true));
    EXPECT_FALSE(Module(emitter, "emitter").at("fields")
        .contains("maxParticles"));

    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "name", Json("renamed"), false));
    EXPECT_EQ(emitter.at("name"), "renamed");

    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "custom.value", Json(3), false));
    EXPECT_EQ(emitter.at("custom").at("value"), 3);
}

TEST(ParticleModuleEdit, SetFieldCreatesMissingModuleForNestedPaths)
{
    Json emitter = Json{{"name", "e"}};
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "collision.plane.offset", Json(3.5), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "flipbook.columns", Json(4), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "mesh.path", Json("Meshes/SM_x.json"), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "shape.radius", Json(8.0), false));
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "velocity.min", Json({0.0, 1.0, 2.0}), false));

    EXPECT_EQ(
        Module(emitter, "collision.analytic").at("fields")
            .at("collision").at("plane").at("offset"),
        3.5);
    EXPECT_EQ(Module(emitter, "flipbook").at("fields")
        .at("flipbook").at("columns"), 4);
    EXPECT_EQ(Module(emitter, "renderer").at("fields")
        .at("mesh").at("path"), "Meshes/SM_x.json");
    EXPECT_EQ(Module(emitter, "spawn.shapeSphere").at("fields")
        .at("shape").at("radius"), 8.0);
    EXPECT_EQ(Module(emitter, "spawn.velocity").at("fields")
        .at("velocity").at("min"), Json({0.0, 1.0, 2.0}));

    const Json flattened = FlattenParticleEmitter(emitter);
    EXPECT_EQ(flattened.at("collision").at("plane").at("offset"), 3.5);
    EXPECT_EQ(flattened.at("flipbook").at("columns"), 4);
    EXPECT_EQ(flattened.at("mesh").at("path"), "Meshes/SM_x.json");
}

TEST(ParticleModuleEdit, SetFieldRejectsEmptyPathAndMalformedModules)
{
    Json emitter = Json{{"name", "e"}};
    const Json before = emitter;
    EXPECT_FALSE(SetParticleEmitterField(
        emitter, "", Json(1), false));
    EXPECT_FALSE(SetParticleEmitterField(
        emitter, "a..b", Json(1), false));
    EXPECT_EQ(emitter, before);

    Json bad = Json{{"name", "e"}, {"modules", 2}};
    const Json badBefore = bad;
    EXPECT_FALSE(SetParticleEmitterField(
        bad, "spawnRate", Json(1.0), false));
    EXPECT_EQ(bad, badBefore);
}

TEST(ParticleModuleEdit, AddRemoveMoveAndToggleModules)
{
    Json emitter = Json{
        {"name", "e"},
        {"modules", Json::array({
            Json{{"type", "emitter"},
                {"enabled", true}, {"fields", Json::object()}},
            Json{{"type", "renderer"},
                {"enabled", true}, {"fields", Json::object()}},
            Json{{"type", "spawn.rate"},
                {"enabled", true}, {"fields", Json::object()}},
        })}};

    size_t added = 99;
    ASSERT_TRUE(AddParticleModule(emitter, "forces.drag", added));
    EXPECT_EQ(added, 3u);
    EXPECT_EQ(emitter.at("modules").at(3).at("type"), "forces.drag");
    EXPECT_EQ(emitter.at("modules").at(3).at("enabled"), true);
    EXPECT_TRUE(emitter.at("modules").at(3).at("fields").empty());

    const Json beforeUnknown = emitter;
    size_t untouched = 42;
    EXPECT_FALSE(AddParticleModule(emitter, "forces.typo", untouched));
    EXPECT_EQ(untouched, 42u);
    EXPECT_EQ(emitter, beforeUnknown);

    ASSERT_TRUE(MoveParticleModule(emitter, 3, -1));
    EXPECT_EQ(emitter.at("modules").at(2).at("type"), "forces.drag");
    EXPECT_EQ(emitter.at("modules").at(3).at("type"), "spawn.rate");

    const Json beforeBadMove = emitter;
    EXPECT_FALSE(MoveParticleModule(emitter, 0, -1));
    EXPECT_FALSE(MoveParticleModule(emitter, 3, 1));
    EXPECT_FALSE(MoveParticleModule(emitter, 1, 2));
    EXPECT_EQ(emitter, beforeBadMove);

    ASSERT_TRUE(SetParticleModuleEnabled(emitter, 1, false));
    EXPECT_EQ(emitter.at("modules").at(1).at("enabled"), false);
    const Json beforeBadToggle = emitter;
    EXPECT_FALSE(SetParticleModuleEnabled(emitter, 99, true));
    EXPECT_EQ(emitter, beforeBadToggle);

    ASSERT_TRUE(RemoveParticleModule(emitter, 1));
    EXPECT_EQ(emitter.at("modules").size(), 3u);
    EXPECT_EQ(emitter.at("modules").at(1).at("type"), "forces.drag");

    const Json beforeBadRemove = emitter;
    EXPECT_FALSE(RemoveParticleModule(emitter, 3));
    EXPECT_EQ(emitter, beforeBadRemove);
}

TEST(ParticleModuleEdit, ModuleOperationsRejectLimitAndMalformedInput)
{
    Json full = Json{{"name", "e"}, {"modules", Json::array()}};
    for (size_t index = 0; index < 64; ++index)
    {
        full["modules"].push_back(Json{
            {"type", "emitter"},
            {"enabled", true},
            {"fields", Json::object()}});
    }
    const Json fullBefore = full;
    size_t index = 0;
    EXPECT_FALSE(AddParticleModule(full, "forces.drag", index));
    EXPECT_EQ(full, fullBefore);

    Json nonObject = Json::array();
    EXPECT_FALSE(AddParticleModule(nonObject, "emitter", index));
    EXPECT_FALSE(RemoveParticleModule(nonObject, 0));
    EXPECT_FALSE(MoveParticleModule(nonObject, 0, 1));
    EXPECT_FALSE(SetParticleModuleEnabled(nonObject, 0, true));

    Json badEntry = Json{
        {"name", "e"},
        {"modules", Json::array({Json{{"type", "emitter"},
            {"fields", 1}}})}};
    const Json badEntryBefore = badEntry;
    EXPECT_FALSE(RemoveParticleModule(badEntry, 0));
    EXPECT_FALSE(MoveParticleModule(badEntry, 0, 1));
    EXPECT_FALSE(SetParticleModuleEnabled(badEntry, 0, false));
    EXPECT_EQ(badEntry, badEntryBefore);
}

TEST(ParticleModuleEdit, LifetimeModuleExposesScaleKeysField)
{
    const ParticleModuleSpec* spec =
        FindParticleModuleSpec("update.lifetime");
    ASSERT_NE(spec, nullptr);
    EXPECT_NE(
        std::find(
            spec->fields.begin(),
            spec->fields.end(),
            "lifetimeScaleKeys"),
        spec->fields.end());
    EXPECT_EQ(
        ParticleModuleTypeForField("lifetimeScaleKeys"),
        "update.lifetime");
}

TEST(ParticleModuleEdit, EnsureAndFlattenMigrateLifetimeScaleKeys)
{
    const Json keys = Json::array({
        Json{{"time", 0.0}, {"scale", 0.0}},
        Json{{"time", 1.0}, {"scale", 2.0}}});
    Json emitter = Json{
        {"name", "fire"},
        {"lifetime", Json::array({0.5, 1.0})},
        {"lifetimeScaleKeys", keys}};

    ASSERT_TRUE(EnsureParticleModules(emitter));
    EXPECT_FALSE(emitter.contains("lifetimeScaleKeys"));
    const Json& module = Module(emitter, "update.lifetime");
    EXPECT_EQ(module.at("fields").at("lifetime").at(0), 0.5);
    EXPECT_EQ(module.at("fields").at("lifetimeScaleKeys"), keys);

    const Json flattened = FlattenParticleEmitter(emitter);
    EXPECT_EQ(flattened.at("lifetimeScaleKeys"), keys);
    EXPECT_FALSE(flattened.contains("modules"));
}

TEST(ParticleModuleEdit, SetFieldWritesLifetimeScaleKeysIntoModule)
{
    Json emitter = Json{{"name", "e"}};
    ASSERT_TRUE(EnsureParticleModules(emitter));

    const Json keys = Json::array({
        Json{{"time", 0.25}, {"scale", 1.5}},
        Json{{"time", 0.75}, {"scale", 0.0}}});
    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "lifetimeScaleKeys", keys, false));
    EXPECT_FALSE(emitter.contains("lifetimeScaleKeys"));
    EXPECT_EQ(
        Module(emitter, "update.lifetime").at("fields")
            .at("lifetimeScaleKeys"),
        keys);

    ASSERT_TRUE(SetParticleEmitterField(
        emitter, "lifetimeScaleKeys", Json(), true));
    EXPECT_FALSE(
        Module(emitter, "update.lifetime").at("fields")
            .contains("lifetimeScaleKeys"));
}
