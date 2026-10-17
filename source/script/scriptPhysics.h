#pragma once

#include <memory>
#include <map>
#include <string>
#include <vector>
#include <Eigen/Core>
#include <nlohmann/json.hpp>

namespace VL
{
// GT 独占的 Jolt CPU 物理世界；输入为已验证的脚本声明/状态，输出仍是纯值 JSON。
// 不访问 renderer、文件或托管对象；每次 Play 重建，Stop/World 换代释放。
class ScriptPhysicsWorld
{
public:
    ScriptPhysicsWorld();
    ~ScriptPhysicsWorld();
    void Step(nlohmann::json& states, const nlohmann::json& declarations, float dt);
    // 引擎固定步入口；advance=false 只同步指令/接触，不累计时间。
    void StepFixed(nlohmann::json& states, const nlohmann::json& declarations, bool advance = true);
    nlohmann::json Contacts() const;
    nlohmann::json Triggers() const;
    nlohmann::json Raycast(const nlohmann::json& requests) const;
    void SetMeshGeometry(const std::map<std::string, std::vector<Eigen::Vector3f>>& triangles);
private:
    void StepInternal(nlohmann::json& states, const nlohmann::json& declarations, int steps);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace VL
