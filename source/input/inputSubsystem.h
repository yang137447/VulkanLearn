#pragma once

#include "core/runtimeResult.h"
#include "input/gameplayInput.h"

namespace VL
{

class PlatformWindow;

struct InputActionState
{
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveDown = false;
    bool moveUp = false;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    nlohmann::json gameplay = nlohmann::json::object();
};

// Owns the current SDL keyboard/mouse sampling path and exposes engine-facing
// input intent. Controller code consumes InputActionState and no longer talks
// directly to SDL or the platform window.
class InputSubsystem
{
public:
    RuntimeResult<void> Initialize(PlatformWindow& window);
    void ConfigureGameplay(const nlohmann::json& config);

    void UpdateActionState();
    const InputActionState& GetActionState() const { return actionState; }
    // 测试/自动化用：对应 UE UPlayerInput::InjectInputForAction 的合成 action，
    // 不依赖 OS 键盘焦点；只进入 gameplay action，不伪造 SDL 状态。
    void InjectAction(const std::string& action, bool pressed);

    void SetRelativeMouseModeRequested(bool requested);
    void SetRelativeMouseModeAllowed(bool allowed);
    void ToggleRelativeMouseModeRequest();
    bool IsRelativeMouseModeRequested() const { return relativeMouseModeRequested; }
    bool IsRelativeMouseModeEnabled() const { return relativeMouseModeEnabled; }
    void SetGameKeyboardEnabled(bool enabled) { gameKeyboardEnabled = enabled; }
    void SetGamePointerEnabled(bool enabled) { gamePointerEnabled = enabled; }

private:
    void ApplyRelativeMouseMode();

    PlatformWindow* window = nullptr;
    InputActionState actionState;
    GameplayInputMap gameplayMap;
    std::map<std::string, int> gameplayScancodes;
    std::set<std::string> injectedActions;
    std::map<std::string, bool> injectedPrevious;
    // UI visibility may suspend capture without overwriting the player's Esc preference.
    // 默认不请求捕获：启动后光标保持自由，鼠标移动不会带动渲染视窗。
    // 需要相机环绕/飞行时按 Esc 显式开启捕获。
    bool relativeMouseModeRequested = false;
    bool relativeMouseModeAllowed = false;
    bool relativeMouseModeEnabled = false;
    bool gameKeyboardEnabled = true;
    bool gamePointerEnabled = true;
};

} // namespace VL
