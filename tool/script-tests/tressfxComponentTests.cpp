#include "script/scriptComponents.h"
#include "script/scriptScene.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{
nlohmann::json Components()
{
    return nlohmann::json::array({
        {{"id", "actor-root"}, {"type", "scene"}, {"position", {0, 0, 0}},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
        {{"id", "body"}, {"type", "mesh"}, {"parent", "actor-root"},
            {"modelPath", "Maps/SC_x/Meshes/SM_body.json"},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
        {{"id", "hair"}, {"type", "tressfx"}, {"parent", "body"},
            {"modelPath", "Maps/SC_x/Meshes/SM_hair.json"},
            {"rootBone", "Head_M"}, {"weights", "auto"},
            {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}}
    });
}

nlohmann::json Blueprint()
{
    return {{"id", "Hero"}, {"type", "blueprintInstance"}, {"class", "Gameplay.Hero"},
        {"properties", nlohmann::json::object()},
        {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}},
        {"components", Components()}};
}
} // namespace

TEST(ScriptTressfxComponent, AcceptsHairUnderMeshAncestor)
{
    EXPECT_NO_THROW(VL::ValidateScriptComponents(Components()));
}

TEST(ScriptTressfxComponent, RejectsHairWithoutMeshAncestor)
{
    auto components = Components();
    components[2]["parent"] = "actor-root";
    EXPECT_THROW(VL::ValidateScriptComponents(components), std::invalid_argument);
}

TEST(ScriptTressfxComponent, RejectsUnknownFieldAndBadWeightsAndMissingModel)
{
    auto unknown = Components();
    unknown[2]["materialOverride"] = "Maps/SC_x/Materials/MI_hair.json";
    EXPECT_THROW(VL::ValidateScriptComponents(unknown), std::invalid_argument);

    auto weights = Components();
    weights[2]["weights"] = "both";
    EXPECT_THROW(VL::ValidateScriptComponents(weights), std::invalid_argument);

    auto missing = Components();
    missing[2].erase("modelPath");
    EXPECT_THROW(VL::ValidateScriptComponents(missing), std::invalid_argument);

    auto emptyRoot = Components();
    emptyRoot[2]["rootBone"] = "";
    EXPECT_THROW(VL::ValidateScriptComponents(emptyRoot), std::invalid_argument);
}

TEST(ScriptTressfxComponent, ExpandsToMeshObjectWithPlacementBinding)
{
    const auto blueprint = Blueprint();
    const auto object = VL::ScriptBlueprintMeshObject(blueprint, 2);
    ASSERT_TRUE(object.at("type").is_string());
    EXPECT_EQ(object.at("type").get<std::string>(), "mesh");
    EXPECT_EQ(object.at("name").get<std::string>(), "__blueprint/Hero/hair");
    EXPECT_EQ(object.at("__tressfxSkinningSource").get<std::string>(), "__blueprint/Hero/body");
    EXPECT_EQ(object.at("__tressfxParentModelPath").get<std::string>(),
        "Maps/SC_x/Meshes/SM_body.json");
    EXPECT_EQ(object.at("__tressfxRootBone").get<std::string>(), "Head_M");
    EXPECT_EQ(object.at("__tressfxWeights").get<std::string>(), "auto");
    // 生成对象仍是单位局部 TRS：完整组件矩阵由候选 World 施加。
    EXPECT_EQ(object.at("position")[0].get<float>(), 0.0f);
}

TEST(ScriptTressfxComponent, TemplateKeyAndStructureCarryBindingSource)
{
    const auto blueprint = Blueprint();
    const auto& hair = blueprint.at("components")[2];
    const auto& body = blueprint.at("components")[1];
    const auto key = VL::ScriptComponentMeshTemplateKey(blueprint, hair);
    EXPECT_NE(key.find("__blueprint/Hero/body"), std::string::npos);

    // 同一个 SM 挂到另一个角色/另一个父组件时必须得到不同的模板键。
    auto other = blueprint;
    other["id"] = "Villain";
    other["components"][2]["parent"] = "body";
    EXPECT_NE(key, VL::ScriptComponentMeshTemplateKey(other, other.at("components")[2]));

    // mesh 组件保持既有三元素键/结构，不受 tressfx 语义影响。
    EXPECT_EQ(VL::ScriptComponentMeshTemplateKey(blueprint, body),
        body.at("modelPath").get<std::string>() + "\n");
    const auto meshEntry = VL::ScriptComponentMeshStructureEntry(blueprint, body);
    EXPECT_EQ(meshEntry.size(), 3u);
    const auto hairEntry = VL::ScriptComponentMeshStructureEntry(blueprint, hair);
    ASSERT_EQ(hairEntry.size(), 4u);
    EXPECT_EQ(hairEntry[3].get<std::string>(), "__blueprint/Hero/body");
}
