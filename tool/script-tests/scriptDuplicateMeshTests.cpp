#include <gtest/gtest.h>

#include <string>

#include "world/world.h"

namespace
{

VL::WorldMeshObject MakeMesh(const std::string& identity)
{
    VL::WorldMeshObject object;
    object.sceneObjectIdentity = identity;
    object.debugName = identity;
    object.model = Eigen::Matrix4f::Identity();
    return object;
}

nlohmann::json MakeBlueprintActor(float componentYaw)
{
    nlohmann::json actor;
    actor["id"] = "dup";
    actor["type"] = "blueprintInstance";
    actor["enabled"] = true;
    actor["position"] = { 0.0f, 0.0f, 0.0f };
    actor["rotation"] = { 0.0f, 0.0f, 0.0f };
    actor["scale"] = { 1.0f, 1.0f, 1.0f };
    actor["components"] = nlohmann::json::array({
        { { "id", "gun" }, { "type", "mesh" },
          { "position", { 0.0f, 0.0f, 0.0f } },
          { "rotation", { 0.0f, componentYaw, 0.0f } },
          { "scale", { 1.0f, 1.0f, 1.0f } } } });
    return actor;
}

// 回归：蓝图实例带 mesh 组件时，加载期可能残留 actor 级兼容网格
// (__blueprint/<id>)，它与组件级网格 (__blueprint/<id>/<component>) 会被画成
// 两份、夹角等于 actor 与组件旋转之差。组件是唯一权威，残留必须被移除。
TEST(ScriptBlueprintMeshes, MeshComponentRemovesActorLevelDuplicate)
{
    VL::World world(1, "Maps/SC_gun_outline_demo/SC_gun_outline_demo.json");
    world.AddMeshObject("__blueprint/dup", MakeMesh("__blueprint/dup"));
    world.AddMeshObject("__blueprint/dup/gun", MakeMesh("__blueprint/dup/gun"));

    world.ApplyScriptBlueprintVisuals(MakeBlueprintActor(25.0f));

    const auto& meshes = world.GetMeshObjects();
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_NE(meshes.find("__blueprint/dup/gun"), meshes.end());
    EXPECT_EQ(meshes.find("__blueprint/dup"), meshes.end());
}

// 容器入口的收敛：两条构建路径（脚本世界/编辑器预览）插入顺序不同，
// 都必须只留下组件级网格。
TEST(ScriptBlueprintMeshes, AddMeshObjectKeepsComponentWhenActorLevelComesFirst)
{
    VL::World world(1, "scene");
    world.AddMeshObject("__blueprint/dup", MakeMesh("__blueprint/dup"));
    world.AddMeshObject("__blueprint/dup/gun", MakeMesh("__blueprint/dup/gun"));
    EXPECT_EQ(world.GetMeshObjects().size(), 1u);
    EXPECT_NE(world.GetMeshObjects().find("__blueprint/dup/gun"), world.GetMeshObjects().end());
}

TEST(ScriptBlueprintMeshes, AddMeshObjectDropsLateActorLevelDuplicate)
{
    VL::World world(1, "scene");
    world.AddMeshObject("__blueprint/dup/gun", MakeMesh("__blueprint/dup/gun"));
    world.AddMeshObject("__blueprint/dup", MakeMesh("__blueprint/dup"));
    EXPECT_EQ(world.GetMeshObjects().size(), 1u);
    EXPECT_NE(world.GetMeshObjects().find("__blueprint/dup/gun"), world.GetMeshObjects().end());
}

// 收敛只能作用于蓝图身份；普通场景 mesh（非 __blueprint/ 前缀）必须原样保留，
// 包括与蓝图 id 同名的对象。
TEST(ScriptBlueprintMeshes, AddMeshObjectNeverDropsNonBlueprintMeshes)
{
    VL::World world(1, "scene");
    world.AddMeshObject("ground", MakeMesh("ground"));
    world.AddMeshObject("__blueprint/dup/gun", MakeMesh("__blueprint/dup/gun"));
    world.AddMeshObject("dup", MakeMesh("dup"));
    world.AddMeshObject("__blueprint/dup", MakeMesh("__blueprint/dup"));

    const auto& meshes = world.GetMeshObjects();
    EXPECT_NE(meshes.find("ground"), meshes.end());
    EXPECT_NE(meshes.find("dup"), meshes.end());
    EXPECT_NE(meshes.find("__blueprint/dup/gun"), meshes.end());
    EXPECT_EQ(meshes.find("__blueprint/dup"), meshes.end());
    EXPECT_EQ(meshes.size(), 3u);
}

} // namespace
