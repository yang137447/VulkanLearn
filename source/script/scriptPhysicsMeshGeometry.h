#pragma once

#include <map>
#include "script/scriptComponents.h"
#include "world/world.h"

namespace VL
{
// Play/异步资产提交时从 World 冻结根局部厘米三角；Jolt 不读 renderer/importer，不在 Step 重新复制。
// template ID 与实例 ID 使用同一身份规则，Spawn 通过模板身份复用这份只读形状数据。
inline std::map<std::string, std::vector<Eigen::Vector3f>> BuildScriptPhysicsMeshGeometry(const World& world)
{
    std::map<std::string, std::vector<Eigen::Vector3f>> result;
    auto declarations = world.GetScriptBlueprints();
    for (const auto& entry : world.GetScriptTemplates().items()) declarations.push_back(entry.value());
    for (const auto& actor : declarations)
    {
        const auto physics = actor.value("physics", nlohmann::json::object());
        if (physics.value("shape", "") != "mesh") continue;
        const auto target = physics.at("component").get<std::string>();
        const auto id = actor.at("id").get<std::string>();
        const Eigen::Matrix4f inverseRoot = ScriptTransformMatrix(actor).inverse();
        const auto nodes = actor.value("components", nlohmann::json::array());
        for (size_t component = 0; component < nodes.size(); ++component)
        {
            const auto& node = nodes[component];
            if (ScriptComponentId(node) != target || node.at("type") != "mesh") continue;
            const auto identity = ScriptMeshIdentity(id, node);
            Eigen::Matrix4f removeLivePose = Eigen::Matrix4f::Identity();
            for (const auto& state : world.GetScriptBlueprintStates())
                if (state.at("id") == id)
                {
                    const auto liveNodes = state.value("components", nlohmann::json::array());
                    for (size_t live = 0; live < liveNodes.size(); ++live)
                        if (ScriptComponentId(liveNodes[live]) == target)
                            // 已在 Play 移动的 mesh 必须先去掉 live 组件位姿，再恢复作者组件位姿。
                            removeLivePose = ScriptComponentMatrix(actor, component) *
                                ScriptComponentMatrix(state, live).inverse();
                }
            auto& triangles = result[id + "/" + target];
            for (const auto& entry : world.GetMeshObjects())
            {
                const auto& mesh = entry.second;
                if (mesh.sceneObjectIdentity != identity || !mesh.collisionTriangles) continue;
                const Eigen::Matrix4f local = inverseRoot * removeLivePose * mesh.model;
                triangles.reserve(triangles.size() + mesh.collisionTriangles->size());
                for (const auto& point : *mesh.collisionTriangles)
                    triangles.push_back((local * point.homogeneous()).head<3>());
            }
        }
    }
    return result;
}
} // namespace VL
