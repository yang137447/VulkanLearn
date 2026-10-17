#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL
{
// 状态只由 GT 修改；UI 消费值副本，不持有宿主或 World。
enum class ScriptPlayState { Editing, Playing, Paused };

struct ScriptHudLine
{
    std::string blueprintId;
    std::string componentId;
    std::string key;
    std::string text;
};

struct ScriptRuntimeView
{
    bool available = false;
    ScriptPlayState state = ScriptPlayState::Editing;
    uint64_t worldGeneration = 0;
    nlohmann::json classes = nlohmann::json::array();
    std::string feedback;
    bool canUndo = false;
    bool canRedo = false;
    bool buildAvailable = false;
    bool buildBusy = false;
    bool autoCompile = false;
    std::string buildOutput;
    nlohmann::json componentClasses = nlohmann::json::array();
    float timeScale = 1;
    nlohmann::json performance = nlohmann::json::object();
    std::vector<ScriptHudLine> hudLines;
};

// 只有脚本调度器累计固定步；物理每次只消费显式的一步，不再拥有第二份播放余量。
struct ScriptTimeState
{
    float scale = 1, delta = 0, unscaledDelta = 0;
    double startup = 0, play = 0, unscaledPlay = 0, accumulated = 0, droppedTime = 0;
    uint64_t frame = 0, fixedFrame = 0, droppedSteps = 0;
    unsigned fixedSteps = 0;
};
}
