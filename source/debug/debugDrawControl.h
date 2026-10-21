#pragma once
#include <string>

namespace VL::Debug
{
class DrawDebug;
enum class ControlAction { Enabled, Category, Owner, ClearAll, ClearOwner, ClearCategory, Status, Demo };
// UI/Console 仅生产值意图，GT 适配器在当前 World generation 上应用。
struct Control
{
    ControlAction action = ControlAction::Status;
    std::string name;
    bool enabled = true;
    bool depthTest = false;
};
bool ParseControl(const std::string& arguments, Control& result, std::string& error);
std::string ApplyControl(DrawDebug& draw, const Control& control);
void DrawDebugDemo(DrawDebug& draw, bool depthTest = false);
}
