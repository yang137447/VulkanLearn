#pragma once

#include <memory>
#include <map>
#include <string>
#include <vector>
#include <Eigen/Core>
#include <nlohmann/json.hpp>
#include "animation/footPlacement.h"

namespace VL
{
// GT 独占的 Jolt CPU 物理世界；脚本批次使用 JSON，动画同步探测只传世界空间纯值。
// 不访问 renderer、文件或托管对象；每次 Play 重建，Stop/World 换代释放。
class ScriptPhysicsWorld : public Animation::GroundProbe
{
public:
    ScriptPhysicsWorld();
    ~ScriptPhysicsWorld();
    // 每帧物理入口：dt 为帧间隔（已乘 timeScale）。角色按帧推进，刚体按内部 1/60 子步追赶；
    // dt=0 只同步状态/指令并收集接触，不移动任何实体。没有固定步入口。
    void Step(nlohmann::json& states, const nlohmann::json& declarations, float dt);
    nlohmann::json Contacts() const;
    nlohmann::json Triggers() const;
    nlohmann::json Raycast(const nlohmann::json& requests) const;
    Animation::FootGroundHit ProbeGround(const std::string& owner, const Eigen::Vector3f& origin,
        const Eigen::Vector3f& displacement) const override;
    void SetMeshGeometry(const std::map<std::string, std::vector<Eigen::Vector3f>>& triangles);
private:
    void StepInternal(nlohmann::json& states, const nlohmann::json& declarations, float dt, int substeps);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace VL
