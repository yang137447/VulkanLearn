#include "editor/ui/scriptComponentControls.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

// TressFX 组件的作者入口是纯 UI 逻辑：选项、默认父级、Details 字段可见性。
// 运行时绑定与组件校验属于 source/script/**（T4a），这里只锁 UI 契约。
namespace
{
using VL::EditorUi::NewScriptComponent;
using VL::EditorUi::ScriptComponentMeshParent;
using VL::EditorUi::ScriptComponentOptions;

nlohmann::json Component(const std::string& id, const std::string& type,
    const std::string& parent = {}, const std::string& modelPath = {})
{
    nlohmann::json node{{"id", id}, {"type", type}};
    if (!parent.empty()) node["parent"] = parent;
    if (!modelPath.empty()) node["modelPath"] = modelPath;
    return node;
}

const char* TressfxOption = "value=\"tressfx\"";
}

TEST(TressfxComponentUi, OptionsExposeTressfxOnlyWhereAuthoringCanBindIt)
{
    // 蓝图资产页需要 TressFX 入口；Scene 侧的通用组件面板没有“挂到哪个角色”的上下文。
    const auto blueprint = ScriptComponentOptions(true);
    EXPECT_NE(blueprint.find(TressfxOption), std::string::npos);
    const auto scene = ScriptComponentOptions();
    EXPECT_EQ(scene.find(TressfxOption), std::string::npos);
}

TEST(TressfxComponentUi, NewComponentDefaultsToAutomaticWeights)
{
    const auto node = NewScriptComponent("tressfx");
    EXPECT_EQ(node.at("type"), "tressfx");
    EXPECT_EQ(node.value("weights", ""), "auto");
    EXPECT_FALSE(node.contains("intensityUnit"));
}

TEST(TressfxComponentUi, MeshParentFollowsSelectionAndNearestAncestor)
{
    const nlohmann::json components = nlohmann::json::array({
        Component("body", "mesh", {}, "SM_body.json"),
        Component("head", "mesh", "body", "SM_head.json"),
        Component("hair", "tressfx", "head", "SM_hair.json"),
        Component("pivot", "scene")});
    EXPECT_EQ(ScriptComponentMeshParent(components, "body"), "body");
    EXPECT_EQ(ScriptComponentMeshParent(components, "head"), "head");
    // 选中已有毛发时，绑定来源仍是它最近的 mesh 祖先。
    EXPECT_EQ(ScriptComponentMeshParent(components, "hair"), "head");
    EXPECT_EQ(ScriptComponentMeshParent(components, "pivot"), "");
    EXPECT_EQ(ScriptComponentMeshParent(components, ""), "");
}

TEST(TressfxComponentUi, DetailsExposeHairFieldsAndHideTransformAndCollision)
{
    VL::EditorUi::EditorSceneActorSnapshot actor;
    actor.scriptComponents = nlohmann::json::array({
        Component("body", "mesh", {}, "SM_body.json"),
        Component("lens", "camera", "body"),
        Component("hair", "tressfx", "body", "SM_hair.json")});
    const auto markup = VL::EditorUi::ScriptComponentControls(actor, {}, 0, "bp-", true);

    // 毛发字段：资产、回退骨、权重来源。
    EXPECT_NE(markup.find("bp-component-2-modelPath-0"), std::string::npos);
    EXPECT_NE(markup.find("bp-component-2-rootBone-0"), std::string::npos);
    EXPECT_NE(markup.find("bp-component-2-weights-0"), std::string::npos);
    // 局部 TRS 与图元碰撞不适用：与父 mesh 同一对象空间，碰撞由描述符拥有。
    EXPECT_EQ(markup.find("bp-component-2-position-0"), std::string::npos);
    EXPECT_EQ(markup.find("bp-component-2-rotation-0"), std::string::npos);
    EXPECT_EQ(markup.find("bp-component-2-scale-0"), std::string::npos);
    EXPECT_EQ(markup.find("bp-component-2-collisionShape-0"), std::string::npos);
    // mesh 组件仍保留这些控件，说明上面是类型分支而不是整体删除。
    EXPECT_NE(markup.find("bp-component-0-position-0"), std::string::npos);
    EXPECT_NE(markup.find("bp-component-0-collisionShape-0"), std::string::npos);
    // 实例白名单不暴露毛发字段。
    EXPECT_EQ(markup.find("bp-component-instance-2-rootBone"), std::string::npos);
    EXPECT_EQ(markup.find("bp-component-instance-2-weights"), std::string::npos);

    // 父级下拉只列 mesh（以及当前父级），不会把相机当成绑定来源。
    const auto selectStart = markup.find("bp-component-2-parent-0");
    ASSERT_NE(selectStart, std::string::npos);
    const auto selectEnd = markup.find("</select>", selectStart);
    ASSERT_NE(selectEnd, std::string::npos);
    const auto parentSelect = markup.substr(selectStart, selectEnd - selectStart);
    EXPECT_NE(parentSelect.find("value=\"body\""), std::string::npos);
    EXPECT_EQ(parentSelect.find("value=\"lens\""), std::string::npos);
    // 毛发没有 "Actor" 空父级：空父级在加载期没有绑定来源。
    EXPECT_EQ(parentSelect.find("value=\"\""), std::string::npos);
}
