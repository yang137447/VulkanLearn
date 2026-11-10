#pragma once

#include "world/world.h"

namespace VL
{
// 预分配所有 World 容器，正式提交仅交换；不复制/替换执行上下文与游戏时间。
struct ScriptTemplateExtension
{
    std::unordered_map<std::string, WorldMeshObject> meshes;
    std::unordered_map<std::string, WindResponseProfile> windProfiles;
    nlohmann::json templates;
    nlohmann::json digests;
    nlohmann::json resolved;
    std::unordered_map<std::string, Eigen::Matrix4f> matrices;
    std::unordered_map<std::string, std::vector<WorldMeshObject>> componentTemplates;
    std::unordered_map<std::string, std::string> structures;
};
}
