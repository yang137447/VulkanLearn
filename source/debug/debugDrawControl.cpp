#include "debugDrawControl.h"
#include "drawDebug.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace VL::Debug
{
namespace
{
void ControlName(const std::string& name, size_t limit, bool empty = false)
{
    if ((!empty && name.empty()) || name.size() > limit) throw std::invalid_argument("DrawDebug: invalid control name");
    for (unsigned char c : name) if (c < 32 || c == 127) throw std::invalid_argument("DrawDebug: control character in name");
}
}
bool ParseControl(const std::string& arguments, Control& result, std::string& error)
{
    std::istringstream input(arguments);
    Control candidate;
    std::string action, value, extra;
    bool valid = bool(input >> action);
    if (action == "on" || action == "off") { candidate.action = ControlAction::Enabled; candidate.enabled = action == "on"; }
    else if (action == "status") candidate.action = ControlAction::Status;
    else if (action == "demo")
    {
        candidate.action = ControlAction::Demo;
        if (input >> value)
        {
            valid = value == "overlay" || value == "depth";
            candidate.depthTest = value == "depth";
        }
        input.clear();
    }
    else if (action == "category")
    {
        candidate.action = ControlAction::Category;
        valid = bool(input >> std::quoted(candidate.name) >> value) && (value == "on" || value == "off");
        candidate.enabled = value == "on";
    }
    else if (action == "owner")
    {
        candidate.action = ControlAction::Owner;
        valid = bool(input >> std::quoted(candidate.name));
        if (candidate.name == "*") candidate.name.clear();
    }
    else if (action == "clear")
    {
        candidate.action = ControlAction::ClearAll;
        if (input >> value)
        {
            valid = (value == "owner" || value == "category") && bool(input >> std::quoted(candidate.name));
            candidate.action = value == "owner" ? ControlAction::ClearOwner : ControlAction::ClearCategory;
        }
        input.clear();
    }
    else valid = false;
    if (input >> extra) valid = false;
    try
    {
        if (candidate.action == ControlAction::Owner) ControlName(candidate.name, 128, true);
        if (candidate.action == ControlAction::ClearOwner) ControlName(candidate.name, 128);
        if (candidate.action == ControlAction::Category || candidate.action == ControlAction::ClearCategory)
            ControlName(candidate.name, 64);
    }
    catch (const std::invalid_argument&) { valid = false; }
    if (!valid)
    {
        error = "Usage: debugdraw <on|off|status|demo [overlay/depth]|category \"name\" on/off|owner \"id\"/*|clear [owner/category \"name\"]>";
        return false;
    }
    result = std::move(candidate); error.clear(); return true;
}
std::string ApplyControl(DrawDebug& draw, const Control& control)
{
    switch (control.action)
    {
    case ControlAction::Enabled: draw.SetEnabled(control.enabled); break;
    case ControlAction::Category: draw.SetCategoryEnabled(control.name, control.enabled); break;
    case ControlAction::Owner: draw.SetOwnerFilter(control.name); break;
    case ControlAction::ClearAll: draw.Clear(); break;
    case ControlAction::ClearOwner: ControlName(control.name, 128); draw.ClearOwner(control.name); break;
    case ControlAction::ClearCategory: ControlName(control.name, 64); draw.ClearCategory(control.name); break;
    case ControlAction::Demo: DrawDebugDemo(draw, control.depthTest); break;
    case ControlAction::Status: break;
    default: throw std::invalid_argument("DrawDebug: unknown control");
    }
    const auto snapshot = draw.Freeze();
    std::ostringstream text;
    text << "DrawDebug: " << (snapshot->enabled ? "on" : "off") << " | " << snapshot->visibleCount << "/"
        << snapshot->storedCount << " items | " << snapshot->lines.size() << " lines | "
        << snapshot->points.size() << " points | " << snapshot->texts.size() << " labels | overlay / optional depth test";
    if (control.action == ControlAction::Status)
    {
        for (const auto& category : snapshot->categories)
            text << "\n  category \"" << category.name << "\" " << (category.enabled ? "on" : "off") << " (" << category.count << ")";
        for (const auto& owner : snapshot->owners) text << "\n  owner \"" << owner << "\"";
    }
    return text.str();
}
void DrawDebugDemo(DrawDebug& draw, bool depthTest)
{
    Style style; style.owner = "Demo"; style.category = "DrawDebug"; style.duration = -1; style.thickness = 2.5f;
    style.depth = depthTest ? DepthMode::DepthTest : DepthMode::Overlay;
    // 以候选副本构造整套示例，预算失败保留活动图形；重复执行按固定 Key 替换。
    auto candidate = draw;
    style.key = "point"; style.color = {1, .25f, .3f, 1};
    candidate.DrawDebugPoint({-220, 100, 0}, 12, style);
    style.key = "line"; candidate.DrawDebugLine({-260, 30, 0}, {-180, 60, 0}, style);
    style.key = "polyline"; style.color = {1, .65f, .1f, 1};
    candidate.DrawDebugPolyline({{-140, 30, 0}, {-140, 110, 0}, {-80, 60, 0}}, true, style);
    style.key = "arrow"; candidate.DrawDebugArrow({-20, 30, 0}, {-20, 110, 0}, 18, style);
    style.key = "circle"; style.color = {.2f, 1, .35f, 1};
    candidate.DrawDebugCircle({110, 50, 0}, 40, Eigen::Quaternionf::Identity(), style);
    style.key = "sphere"; style.color = {.1f, .8f, 1, 1};
    candidate.DrawDebugSphere({-210, 65, -160}, 40, style);
    style.key = "box"; style.color = {1, .3f, .7f, 1};
    candidate.DrawDebugBox({-100, 65, -160}, {35, 45, 30},
        Eigen::Quaternionf(Eigen::AngleAxisf(.35f, Eigen::Vector3f::UnitY())), style);
    style.key = "capsule"; style.color = {.8f, 1, .15f, 1};
    candidate.DrawDebugCapsule({20, 70, -160}, 25, 35, Eigen::Quaternionf::Identity(), style);
    style.key = "axes"; candidate.DrawDebugCoordinateSystem({110, 30, -160}, 75, Eigen::Quaternionf::Identity(), style);
    for (const auto& label : std::vector<std::pair<Eigen::Vector3f, std::string>>{
        {{-260, 135, 0}, "Point / Line"}, {{-150, 135, 0}, "Polyline"}, {{-40, 135, 0}, "Arrow"},
        {{80, 135, 0}, "Circle"}, {{-250, 145, -160}, "Sphere"}, {{-140, 145, -160}, "Box"},
        {{-10, 145, -160}, "Capsule"}, {{100, 145, -160}, "Axes"}})
    {
        style.key = label.second; style.color = {1, 1, 1, 1};
        candidate.DrawDebugString(label.first, label.second, 14, style);
    }
    draw = std::move(candidate);
}
}
