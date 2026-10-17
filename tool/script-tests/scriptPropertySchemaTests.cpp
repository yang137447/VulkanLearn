#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include "script/scriptCompleteDeclaration.h"

namespace
{
using namespace VL;
using Json = nlohmann::json;

Json Number(const std::string& name, int value)
{
    return {{"name", name}, {"type", "int"}, {"value", value}};
}

Json Stats()
{
    return {{"name", "Stats"}, {"type", "object"}, {"typeName", "Gameplay.Stats"},
        {"value", {{"Damage", 10}, {"Cooldown", 2}}},
        {"fields", Json::array({Number("Damage", 10), Number("Cooldown", 2)})},
        {"instanceEditable", true}};
}

Json Settings()
{
    auto element = Stats();
    element.erase("name");
    const Json points{{"name", "Points"}, {"type", "array"}, {"typeName", "Gameplay.Stats[]"},
        {"element", element}, {"value", Json::array({element.at("value")})}};
    const Json slots{{"name", "Slots"}, {"type", "dictionary"}, {"typeName", "Gameplay.Slots"},
        {"element", element}, {"value", {{"old", element.at("value")}}}};
    return {{"name", "Settings"}, {"type", "object"}, {"typeName", "Gameplay.Settings"},
        {"fields", Json::array({Stats(), points, slots})},
        {"value", {{"Stats", Stats().at("value")}, {"Points", points.at("value")}, {"Slots", slots.at("value")}}},
        {"instanceEditable", true}};
}

TEST(ScriptPropertySchemaTest, ObjectPatchRetainsDefaultsAndUnknownFieldsAreRejected)
{
    const auto schema = Settings();
    const auto merged = MergeScriptPropertyValue(schema, schema.at("value"), {{"Stats", {{"Damage", 25}}}});
    EXPECT_EQ(merged.at("Stats"), (Json{{"Damage", 25}, {"Cooldown", 2}}));
    EXPECT_EQ(merged.at("Points"), schema.at("value").at("Points"));
    EXPECT_THROW(MergeScriptPropertyValue(schema, schema.at("value"), {{"Stats", {{"Missing", 1}}}}),
        std::runtime_error);
}

TEST(ScriptPropertySchemaTest, DictionaryAndArrayReplaceOldElementsAndCompleteNewObjectValues)
{
    const auto schema = Settings();
    const Json patch{{"Slots", {{"new/key.with.dot", {{"Damage", 31}}}}},
        {"Points", Json::array({{{"Damage", 32}}})}};
    const auto merged = MergeScriptPropertyValue(schema, schema.at("value"), patch);
    EXPECT_EQ(merged.at("Slots").size(), 1u);
    EXPECT_FALSE(merged.at("Slots").contains("old"));
    EXPECT_EQ(merged.at("Slots").at("new/key.with.dot"), (Json{{"Damage", 31}, {"Cooldown", 2}}));
    EXPECT_EQ(merged.at("Points").at(0), (Json{{"Damage", 32}, {"Cooldown", 2}}));
}

TEST(ScriptPropertySchemaTest, ConsecutiveObjectLeafEditsRetainSparseSiblings)
{
    const auto schema = Settings();
    auto authored = ApplyScriptPropertyEdit(schema, Json(), schema.at("value"),
        {{"path", {"Stats", "Damage"}}, {"value", 42}});
    EXPECT_EQ(authored, (Json{{"Stats", {{"Damage", 42}}}}));
    const auto effective = MergeScriptPropertyValue(schema, schema.at("value"), authored);
    authored = ApplyScriptPropertyEdit(schema, authored, effective,
        {{"path", {"Stats", "Cooldown"}}, {"value", 7}});
    EXPECT_EQ(authored, (Json{{"Stats", {{"Damage", 42}, {"Cooldown", 7}}}}));
    EXPECT_FALSE(authored.contains("Points"));
}

TEST(ScriptPropertySchemaTest, CollectionLeafEditSavesWholeCollectionWithLiteralDictionaryKeys)
{
    const auto schema = Settings();
    auto effective = schema.at("value");
    effective["Slots"] = {{"a/b.c", {{"Damage", 11}, {"Cooldown", 3}}},
        {"", {{"Damage", 12}, {"Cooldown", 4}}}};
    const auto authored = ApplyScriptPropertyEdit(schema, Json(), effective,
        {{"path", {"Slots", "a/b.c", "Damage"}}, {"value", 90}});
    EXPECT_EQ(authored.at("Slots").size(), 2u);
    EXPECT_EQ(authored.at("Slots").at("a/b.c").at("Damage"), 90);
    EXPECT_EQ(authored.at("Slots").at("").at("Damage"), 12);
    EXPECT_FALSE(authored.contains("Stats"));
}

TEST(ScriptPropertySchemaTest, ResetObjectLeafPrunesOnlyEmptyAncestors)
{
    const auto schema = Settings();
    const Json authored{{"Stats", {{"Damage", 42}, {"Cooldown", 7}}}};
    auto result = ApplyScriptPropertyEdit(schema, authored, schema.at("value"),
        {{"operation", "reset"}, {"path", {"Stats", "Damage"}}});
    EXPECT_EQ(result, (Json{{"Stats", {{"Cooldown", 7}}}}));
    result = ApplyScriptPropertyEdit(schema, result, schema.at("value"),
        {{"operation", "reset"}, {"path", {"Stats", "Cooldown"}}});
    EXPECT_TRUE(result.is_null());
}

TEST(ScriptPropertySchemaTest, ResetCannotDescendIntoAtomicCollections)
{
    const auto schema = Settings();
    EXPECT_THROW(ApplyScriptPropertyEdit(schema, schema.at("value"), schema.at("value"),
        {{"operation", "reset"}, {"path", {"Slots", "old", "Damage"}}}), std::invalid_argument);
    const auto result = ApplyScriptPropertyEdit(schema, schema.at("value"), schema.at("value"),
        {{"operation", "reset"}, {"path", {"Slots"}}});
    EXPECT_FALSE(result.contains("Slots"));
    EXPECT_TRUE(result.contains("Stats"));
}

TEST(ScriptPropertySchemaTest, CollectionCommandsUseLatestEffectiveValueAndRejectInvalidIndices)
{
    const auto schema = Settings();
    auto authored = ApplyScriptPropertyEdit(schema, Json(), schema.at("value"),
        {{"operation", "append"}, {"path", {"Points"}}});
    auto effective = MergeScriptPropertyValue(schema, schema.at("value"), authored);
    EXPECT_EQ(effective.at("Points").size(), 2u);
    authored = ApplyScriptPropertyEdit(schema, authored, effective,
        {{"operation", "remove"}, {"path", {"Points"}}, {"index", 0}});
    EXPECT_EQ(authored.at("Points").size(), 1u);
    EXPECT_THROW(ApplyScriptPropertyEdit(schema, authored, effective,
        {{"operation", "remove"}, {"path", {"Points"}}, {"index", -1}}), std::invalid_argument);
    EXPECT_THROW(ApplyScriptPropertyEdit(schema, authored, effective,
        {{"operation", "remove"}, {"path", {"Points"}}, {"index", 0.5}}), std::invalid_argument);
}

TEST(ScriptPropertySchemaTest, RawTreeAcceptsContainersAndRejectsNullAndContainerOverflow)
{
    EXPECT_NO_THROW(ValidateScriptPropertyTree(Json{{"data", Json::array({1, true, "text", Json::object()})}}));
    EXPECT_THROW(ValidateScriptPropertyTree(Json{{"data", nullptr}}), std::invalid_argument);
    Json values = Json::array();
    for (int index = 0; index < 1025; ++index) values.push_back(index);
    EXPECT_THROW(ValidateScriptPropertyTree(values), std::invalid_argument);
}

TEST(ScriptPropertySchemaTest, TypedTreeEnforcesTotalNodeAndDepthBudgets)
{
    const Json number{{"type", "number"}, {"value", 0}};
    const Json row{{"type", "array"}, {"element", number}, {"value", Json::array()}};
    Json schema{{"type", "array"}, {"element", row}, {"value", Json::array()}};
    Json value = Json::array();
    for (int index = 0; index < 8; ++index)
        value.push_back(std::vector<int>(512, 0));
    EXPECT_THROW(ValidateScriptPropertyValue(schema, value), std::runtime_error);
    schema = number;
    value = 0;
    for (int depth = 0; depth < 17; ++depth)
    {
        schema = Json{{"type", "array"}, {"element", schema}, {"value", Json::array()}};
        value = Json::array({value});
    }
    EXPECT_THROW(ValidateScriptPropertyValue(schema, value), std::runtime_error);
}

class ScriptPropertyAssetLayersTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-data-layers-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Common/Blueprints");
        const Json configured{{"Stats", {{"Damage", 20}, {"Cooldown", 9}}},
            {"Slots", {{"old", {{"Damage", 30}, {"Cooldown", 8}}}}}};
        asset = {{"type", "blueprint"}, {"name", "Data"}, {"class", "Gameplay.Data"},
            {"properties", {{"Settings", configured}}}, {"instanceEditable", {"Settings"}},
            {"scriptComponents", Json::array({{{"id", "data"}, {"class", "Gameplay.DataComponent"},
                {"properties", {{"Settings", configured}}}, {"instanceEditable", {"Settings"}}}})}};
        std::ofstream(root / "Common/Blueprints/BP_data.json") << asset.dump();
        instance = {{"type", "blueprintInstance"}, {"id", "data"}, {"name", "Data"},
            {"blueprintPath", "Common/Blueprints/BP_data.json"},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
            {"properties", {{"Settings", {{"Stats", {{"Damage", 50}}}, {"Slots", Json::object()}}}}},
            {"scriptComponentOverrides", {{"data", {{"Settings", {{"Stats", {{"Damage", 60}}},
                {"Slots", Json::object()}}}}}}}};
        catalog = Json::array({{{"name", "Gameplay.Data"}, {"properties", Json::array({Settings()})}}});
        components = Json::array({{{"name", "Gameplay.DataComponent"}, {"properties", Json::array({Settings()})}}});
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void Check(const Json& resolved)
    {
        const auto completed = CompleteBlueprintDeclaration(resolved, catalog, components);
        EXPECT_EQ(completed.at("properties").at("Settings").at("Stats"), (Json{{"Damage", 50}, {"Cooldown", 9}}));
        EXPECT_TRUE(completed.at("properties").at("Settings").at("Slots").empty());
        const auto& value = completed.at("scriptComponents").at(0).at("properties").at("Settings");
        EXPECT_EQ(value.at("Stats"), (Json{{"Damage", 60}, {"Cooldown", 9}}));
        EXPECT_TRUE(value.at("Slots").empty());
    }
    std::filesystem::path root;
    Json instance, asset, catalog, components;
};

TEST_F(ScriptPropertyAssetLayersTest, SingleAssetResolutionRetainsTypedAssetLayerAndSparseSource)
{
    const auto original = instance;
    Check(ResolveScriptBlueprint(root, instance));
    EXPECT_EQ(instance, original);
}

TEST_F(ScriptPropertyAssetLayersTest, WholeSceneResolutionUsesSameTypedLayerRules)
{
    const auto scene = ResolveScriptBlueprintScene(root, {{"objects", Json::array({instance})}}, "Maps/SC_data/SC_data.json");
    Check(scene.at("objects").at(0));
}

TEST_F(ScriptPropertyAssetLayersTest, InlineInputCannotInjectDefaultLayersOrPermissions)
{
    Json inlineNode{{"type", "blueprintInstance"}, {"id", "data"}, {"name", "Data"},
        {"class", "Gameplay.Data"}, {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"properties", Json::object()}, {"__propertyDefaults", {{"Settings", {{"Stats", {{"Damage", 999}}}}}}},
        {"__templatePath", "injected"}, {"__instanceEditable", {"Settings"}}, {"__scriptComponentAsset", true}};
    const auto resolved = ResolveScriptBlueprint(root, inlineNode);
    EXPECT_FALSE(resolved.contains("__propertyDefaults"));
    EXPECT_FALSE(resolved.contains("__templatePath"));
    EXPECT_FALSE(resolved.contains("__instanceEditable"));
    EXPECT_FALSE(resolved.contains("__scriptComponentAsset"));
    const auto completed = CompleteBlueprintDeclaration(resolved, catalog, components);
    EXPECT_EQ(completed.at("properties").at("Settings").at("Stats").at("Damage"), 10);
    // 单独解析与整场景加载一样，不允许内部模板标记跳过 inline 实例权限。
    auto restricted = catalog;
    restricted[0]["properties"][0]["instanceEditable"] = false;
    inlineNode["properties"]["Settings"] = {{"Stats", {{"Damage", 88}}}};
    EXPECT_THROW(CompleteBlueprintDeclaration(ResolveScriptBlueprint(root, inlineNode), restricted, components),
        std::invalid_argument);
}
}
