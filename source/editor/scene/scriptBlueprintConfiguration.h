#pragma once

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace VL::Editor
{
// 组件碰撞只描述根 compound 的局部形状，不引入独立刚体或第二套过滤层。
inline void EditScriptComponentCollision(nlohmann::json& node, const nlohmann::json& request)
{
    const auto field = request.at("field").get<std::string>();
    const auto& value = request.at("value");
    if (field == "collisionShape" && value == "none") { node.erase("collision"); return; }
    if (!node.contains("collision"))
        node["collision"] = {{"shape", "box"}, {"halfExtents", {50, 50, 50}}};
    auto& collision = node.at("collision");
    if (field == "collisionShape")
    {
        // 下拉框可能延迟回报同值；不能用重复 shape 事件覆盖用户刚输入的尺寸。
        if (collision.at("shape") == value) return;
        collision["shape"] = value;
        if (value == "box")
        {
            collision["halfExtents"] = {50, 50, 50};
            collision.erase("radius"); collision.erase("halfHeight");
        }
        else
        {
            collision.erase("halfExtents");
            collision["radius"] = 30;
            if (value == "capsule") collision["halfHeight"] = 60;
            else collision.erase("halfHeight");
        }
    }
    else if (field == "collisionHalfExtents")
        collision.at("halfExtents").at(request.at("axis").get<size_t>()) = value;
    else if (field == "collisionRadius") collision["radius"] = value;
    else if (field == "collisionHalfHeight") collision["halfHeight"] = value;
    else throw std::invalid_argument("Unsupported component collision edit");
}

// UI 仅在自己的值副本上合成字段请求；GT 再验证最终候选。这里不发布文档或写盘。
inline void EditScriptPhysics(nlohmann::json& actor, const nlohmann::json& request)
{
    const auto field = request.at("field").get<std::string>();
    const auto& value = request.at("value");
    if (field == "motion" && value == "none")
    {
        actor.erase("physics");
        if (actor.contains("components"))
            for (auto& node : actor.at("components")) node.erase("collision");
        return;
    }
    if (!actor.contains("physics"))
        actor["physics"] = {{"motion", "static"}, {"shape", "box"}, {"halfExtents", {50, 50, 50}}};
    auto& physics = actor.at("physics");
    if ((field == "motion" || field == "shape") && physics.contains(field) && physics.at(field) == value)
        return;
    if (field == "constraint-add")
    {
        if (!physics.contains("constraints")) physics["constraints"] = nlohmann::json::array();
        auto& joints = physics.at("constraints");
        std::string id;
        size_t suffix = joints.size();
        bool occupied;
        do
        {
            id = "joint-" + std::to_string(++suffix);
            occupied = false;
            for (const auto& joint : joints) if (joint.at("id") == id) occupied = true;
        } while (occupied);
        joints.push_back({{"id", id}, {"type", "fixed"}, {"target", value}, {"anchor", {0, 0, 0}}});
    }
    else if (field == "constraint-remove")
        physics.at("constraints").erase(value.get<size_t>());
    else if (field.rfind("constraint-", 0) == 0)
    {
        const auto key = field.substr(11);
        auto& joint = physics.at("constraints").at(request.at("index").get<size_t>());
        if (key == "anchor" || key == "axis")
        {
            if (request.contains("axis")) joint.at(key).at(request.at("axis").get<size_t>()) = value;
            else joint[key] = value;
        }
        else if (key == "target" || key == "type" || key == "minAngle" || key == "maxAngle") joint[key] = value;
        else throw std::invalid_argument("Unsupported constraint edit");
        if (key == "type")
        {
            if (value == "hinge") { joint["axis"] = {0, 1, 0}; joint["minAngle"] = -180; joint["maxAngle"] = 180; }
            else { joint.erase("axis"); joint.erase("minAngle"); joint.erase("maxAngle"); }
        }
    }
    else if (field == "halfExtents" && request.contains("axis"))
        physics.at(field).at(request.at("axis").get<size_t>()) = value;
    else if (field == "collisionLayer" || field == "collisionMask")
    {
        const double number = value.get<double>();
        // 层与掩码保持整数JSON，非法小数交给作者边界明确拒绝，不能静默截断。
        if (std::isfinite(number) && std::floor(number) == number && number >= 0 && number <= 65535)
            physics[field] = static_cast<uint32_t>(number);
        else physics[field] = value;
    }
    else physics[field] = value;
    // 形状切换原子地移除不适用的字段；角色始终使用非 Trigger 胶囊。
    if (field == "shape" || field == "motion")
    {
        if (physics.value("motion", "static") == "character")
        {
            physics["shape"] = "capsule";
            physics["trigger"] = false;
        }
        else for (const char* setting : {"stepHeight", "jumpSpeed", "maxSlopeAngle"}) physics.erase(setting);
        const auto shape = physics.value("shape", "box");
        if (shape != "compound" && actor.contains("components"))
            for (auto& node : actor.at("components")) node.erase("collision");
        if (shape == "mesh" || shape == "compound")
        {
            physics.erase("radius"); physics.erase("halfHeight"); physics.erase("halfExtents");
            if (shape == "mesh")
            {
                physics["motion"] = "static";
                physics.erase("shapes");
                for (const auto& node : actor.value("components", nlohmann::json::array()))
                    if (node.at("type") == "mesh")
                    { physics["component"] = node.value("id", "root"); break; }
            }
            else
            {
                physics.erase("component");
                bool collider = physics.contains("shapes");
                for (const auto& node : actor.value("components", nlohmann::json::array()))
                    collider = collider || node.contains("collision");
                if (!collider && actor.contains("components") && !actor.at("components").empty())
                    actor["components"][0]["collision"] = {{"shape", "box"}, {"halfExtents", {50, 50, 50}}};
            }
        }
        else if (shape == "box")
        {
            physics.erase("component"); physics.erase("shapes");
            if (!physics.contains("halfExtents")) physics["halfExtents"] = {50, 50, 50};
            physics.erase("radius"); physics.erase("halfHeight");
        }
        else
        {
            physics.erase("component"); physics.erase("shapes");
            physics.erase("halfExtents");
            if (!physics.contains("radius")) physics["radius"] = 30;
            if (shape == "capsule")
            { if (!physics.contains("halfHeight")) physics["halfHeight"] = 60; }
            else physics.erase("halfHeight");
        }
    }
}
} // namespace VL::Editor
