#pragma once
#include <nlohmann/json.hpp>
#include "debug/drawDebug.h"

namespace VL
{
// CLR 的一次性绘图命令只在 GT 领取；owner 来自已验证的 Blueprint 身份，不能由脚本冒用。
// 公开 C++ Submit 是独立输入边界，负责有限值/预算检查；显示适配器不重复校验。
inline void ApplyScriptDebugDraw(Debug::DrawDebug& draw, const nlohmann::json& actor,
    const nlohmann::json& commands)
{
    const std::string owner = "Script/" + actor.at("id").get<std::string>();
    if (actor.value("failed", false) || actor.value("destroyed", false) || !actor.value("enabled", true))
    { draw.ClearOwner(owner); return; }
    if (!commands.is_array() || commands.size() > 512) throw std::invalid_argument("Script.InvalidDebugBatch");
    if (commands.empty()) return;
    // 生命周期清理不可被同批新图形的预算失败回滚，否则禁用/故障组件会留下旧持久图形。
    for (const auto& command : commands)
    {
        if (!command.value("cleanup", false)) continue;
        const std::string op = command.at("op");
        if (op == "clearOwner") draw.ClearOwner(owner);
        else if (op == "clear")
            draw.ClearPublisher(owner, command.at("publisher").get<std::string>());
        else throw std::invalid_argument("Script.InvalidDebugCleanup");
    }
    auto candidate = draw;
    for (const auto& command : commands)
    {
        if (command.value("cleanup", false)) continue;
        const std::string op = command.at("op");
        if (op == "clearOwner") { candidate.ClearOwner(owner); continue; }
        const std::string publisher = command.at("publisher");
        if (op == "clear")
        {
            // clear(category) 仅删除当前发布者的项，不能清理同类的其它 Blueprint。
            const auto category = command.at("category");
            candidate.ClearPublisherCategory(owner, publisher, category.is_null() ? "" : category.get<std::string>());
            continue;
        }
        if (op == "remove")
        {
            candidate.RemoveKey(owner, publisher, command.at("category"), command.at("key"));
            continue;
        }
        if (op != "draw") throw std::invalid_argument("Script.InvalidDebugOperation");
        Debug::Request r;
        r.shape = static_cast<Debug::Shape>(command.at("shape").get<int>());
        r.style.owner = owner; r.style.publisher = publisher; r.style.category = command.at("category");
        r.style.key = command.at("key"); r.style.duration = command.at("duration"); r.style.thickness = command.at("thickness");
        r.style.color = command.at("color").get<Debug::Color>();
        r.style.depth = static_cast<Debug::DepthMode>(command.value("depth", 0));
        const auto begin = command.at("begin").get<std::array<float, 3>>();
        const auto end = command.at("end").get<std::array<float, 3>>();
        const auto extents = command.at("extents").get<std::array<float, 3>>();
        const auto q = command.at("orientation").get<std::array<float, 4>>();
        r.begin = Eigen::Vector3f(begin.data()); r.end = Eigen::Vector3f(end.data()); r.extents = Eigen::Vector3f(extents.data());
        r.orientation = Eigen::Quaternionf(q[3], q[0], q[1], q[2]);
        r.radius = command.at("radius"); r.halfHeight = command.at("halfHeight"); r.size = command.at("size");
        r.segments = command.at("segments"); r.closed = command.at("closed"); r.text = command.at("text");
        for (const auto& point : command.at("points"))
        {
            const auto values = point.get<std::array<float, 3>>();
            r.points.emplace_back(values.data());
        }
        candidate.Submit(std::move(r));
    }
    draw = std::move(candidate);
}
}
