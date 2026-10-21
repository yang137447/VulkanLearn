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

// 帧语义与 UE 对齐：没有固定步 accumulator / fixed 回调，脚本与物理只按每帧变 dt 前进。
// 刚体的 1/60 子步完全留在 ScriptPhysicsWorld 内部，不属于这里的时间轴。
struct ScriptTimeState
{
    float scale = 1, delta = 0, unscaledDelta = 0;
    double startup = 0, play = 0, unscaledPlay = 0;
    uint64_t frame = 0;
};
}
