#include "script/scriptComponents.h"
#include "script/scriptScene.h"
#include "world/world.h"

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

TEST(ScriptTressfxComponent, RequiresExplicitRootBoneAfterDefaultsMoveToAsset)
{
    auto components = Components();
    components[2].erase("rootBone");
    EXPECT_THROW(VL::ValidateScriptComponents(components), std::invalid_argument);
}

TEST(ScriptTressfxComponent, PreservesSimulationOverrideCollisionAndMaterialOnExpansion)
{
    auto blueprint = Blueprint();
    auto& hair = blueprint["components"][2];
    hair["simulation"] = {{"mode", "off"}, {"overrides", {{"damping", 0.05f}}}};
    hair["hairCollision"] = {{"capsules", nlohmann::json::array()}};
    hair["materialOverride"] = "Maps/SC_x/Materials/MI_hair.json";
    EXPECT_NO_THROW(VL::ValidateScriptComponents(blueprint.at("components")));
    const auto object = VL::ScriptBlueprintMeshObject(blueprint, 2);
    EXPECT_EQ(object.at("tressfx").at("simulation"), hair.at("simulation"));
    EXPECT_EQ(object.at("tressfx").at("hairCollision"), hair.at("hairCollision"));
    EXPECT_EQ(object.at("tressfx").at("skinning").at("source"), "__blueprint/Hero/body");
    EXPECT_EQ(object.at("tressfx").at("skinning").at("weights"), "mesh");
    EXPECT_EQ(object.at("materialOverride"), hair.at("materialOverride"));
    const auto oldKey = VL::ScriptComponentMeshTemplateKey(blueprint, hair);
    const auto oldStructure = VL::ScriptComponentMeshStructureEntry(blueprint, hair);
    hair["simulation"]["mode"] = "on";
    EXPECT_NE(oldKey, VL::ScriptComponentMeshTemplateKey(blueprint, hair));
    EXPECT_NE(oldStructure, VL::ScriptComponentMeshStructureEntry(blueprint, hair));
}

TEST(ScriptTressfxComponent, RejectsForbiddenSimulationOverrideBeforeExpansion)
{
    auto components = Components();
    components[2]["simulation"] = {{"mode", "inherit"}, {"overrides", {{"enabled", false}}}};
    EXPECT_THROW(VL::ValidateScriptComponents(components), std::invalid_argument);
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
    unknown[2]["unexpected"] = "Maps/SC_x/Materials/MI_hair.json";
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
    ASSERT_EQ(hairEntry.size(), 8u);
    EXPECT_EQ(hairEntry[3].get<std::string>(), "__blueprint/Hero/body");
}

TEST(ScriptTressfxComponent, SpawnCloneRebasesPerPlacementHairKeyAndPlacementBinding)
{
    // 运行时 spawn 克隆模板网格对象：GPU 毛发的键必须换成本实例身份（否则借用
    // 模板的 RenderableObject），绑定来源必须重写成新演员的父 mesh 组件；普通
    // 共享网格保持原键，不因 spawn 复制 GPU 几何。
    VL::World world(9, "spawn-clone-test");
    VL::WorldMeshObject hair;
    hair.sceneObjectIdentity = "__blueprint/~template.0/hair";
    hair.debugName = hair.sceneObjectIdentity;
    hair.meshKey = "cache|0|hair|instance=__blueprint/~template.0/hair";
    hair.materialKey = "mat";
    hair.materialInstanceKey = "mi";
    hair.placementBinding.tressfx = VL::Hair::ResolveTressfxBehavior(
        TressfxSimulationConfig(),
        VL::Hair::ReadTressfxPlacementBehavior({
            {"simulation", {{"mode", "on"}, {"overrides",
                {{"backend", "gpu"}, {"damping", 0.06f}}}}},
            {"skinning", {{"source", "__blueprint/~template.0/body"},
                {"rootBone", "Head_M"}, {"weights", "mesh"}}},
            {"hairCollision", {{"capsules", nlohmann::json::array({
                {{"a", {0, 0, 0}}, {"b", {0, 1, 0}}, {"radius", 0.5f}}})}}}
        }, "spawn-template"));
    world.AddMeshObject(hair.debugName, hair);

    VL::WorldMeshObject body;
    body.sceneObjectIdentity = "__blueprint/~template.0/body";
    body.debugName = body.sceneObjectIdentity;
    body.meshKey = "cache|0|body";
    world.AddMeshObject(body.debugName, body);

    nlohmann::json actor = {
        {"id", "~spawn.1"},
        {"components", nlohmann::json::array({
            {{"id", "body"}, {"type", "mesh"},
                {"modelPath", "Maps/SC_x/Meshes/SM_body.json"}},
            {{"id", "hair"}, {"type", "tressfx"}, {"parent", "body"},
                {"modelPath", "Maps/SC_x/Meshes/TF_hair.json"},
                {"rootBone", "Head_M"}, {"weights", "auto"}}})}};
    world.CloneScriptBlueprintMeshes("~template.0", actor);

    const auto& objects = world.GetMeshObjects();
    const auto hairIt = objects.find("__blueprint/~spawn.1/hair");
    ASSERT_NE(hairIt, objects.end());
    EXPECT_EQ(hairIt->second.meshKey,
        "cache|0|hair|instance=__blueprint/~spawn.1/hair");
    EXPECT_EQ(hairIt->second.placementTemplateMeshKey,
        "cache|0|hair|instance=__blueprint/~template.0/hair");
    EXPECT_EQ(hairIt->second.placementBinding.kind,
        VL::PlacementBinding::Kind::TressfxSkinning);
    EXPECT_EQ(hairIt->second.placementBinding.sourceSceneObject,
        "__blueprint/~spawn.1/body");
    EXPECT_EQ(hairIt->second.placementBinding.rootBone, "Head_M");
    EXPECT_EQ(hairIt->second.placementBinding.weights, "mesh");
    const auto& effective = hairIt->second.placementBinding.tressfx;
    ASSERT_TRUE(effective.has_value());
    EXPECT_TRUE(effective->enabled);
    EXPECT_EQ(effective->backend, TressfxSimulationBackend::Gpu);
    EXPECT_FLOAT_EQ(effective->damping, 0.06f);
    ASSERT_TRUE(effective->skinning.has_value());
    EXPECT_EQ(effective->skinning->source, "__blueprint/~spawn.1/body");
    EXPECT_EQ(effective->skinning->rootBone, "Head_M");
    EXPECT_TRUE(effective->skinning->rootWeightsAuthored);
    EXPECT_EQ(effective->skinning->rootWeights, TressfxRootWeightSource::Mesh);
    ASSERT_EQ(effective->capsules.size(), 1u);
    EXPECT_FLOAT_EQ(effective->capsules[0].radius, 0.5f);
    const auto& original = objects.at("__blueprint/~template.0/hair");
    ASSERT_TRUE(original.placementBinding.tressfx->skinning.has_value());
    EXPECT_EQ(original.placementBinding.tressfx->skinning->source,
        "__blueprint/~template.0/body");

    const auto bodyIt = objects.find("__blueprint/~spawn.1/body");
    ASSERT_NE(bodyIt, objects.end());
    EXPECT_EQ(bodyIt->second.meshKey, "cache|0|body");
    EXPECT_TRUE(bodyIt->second.placementTemplateMeshKey.empty());
    EXPECT_TRUE(bodyIt->second.placementBinding.IsEmpty());
}
