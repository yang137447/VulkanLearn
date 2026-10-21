#include "input/inputSubsystem.h"

#include "SDL3/SDL.h"

#include "platform/platformWindow.h"

namespace VL
{

RuntimeResult<void> InputSubsystem::Initialize(PlatformWindow& window)
{
    if (!window.IsValid())
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "InputSubsystem.NullWindow",
            "Cannot initialize InputSubsystem with an invalid platform window."));
    }

    this->window = &window;
    // 启动默认释放鼠标捕获：光标留在桌面，避免鼠标划动意外旋转/平移视窗。
    // Esc 仍然是显式开关（ToggleRelativeMouseModeRequest），需要自由视角时再开启。
    relativeMouseModeRequested = false;
    relativeMouseModeAllowed = false;
    relativeMouseModeEnabled = false;
    gameKeyboardEnabled = true;
    gamePointerEnabled = true;
    this->window->SetRelativeMouseMode(false);
    SDL_GetRelativeMouseState(nullptr, nullptr);
    ConfigureGameplay(nlohmann::json::object());
    return RuntimeResult<void>::Success();
}

void InputSubsystem::ConfigureGameplay(const nlohmann::json& config)
{
    GameplayInputMap candidate;
    candidate.Configure(config);
    std::map<std::string, int> scancodes;
    for (const auto& key : candidate.Keys())
    {
        if (key == "MouseLeft" || key == "MouseRight" || key == "MouseMiddle" ||
            key == "MouseX" || key == "MouseY") continue;
        const auto scancode = SDL_GetScancodeFromName(key.c_str());
        if (scancode == SDL_SCANCODE_UNKNOWN) throw std::invalid_argument("Unknown input key: " + key);
        scancodes[key] = scancode;
    }
    gameplayMap = std::move(candidate);
    gameplayScancodes = std::move(scancodes);
}

void InputSubsystem::UpdateActionState()
{
    actionState = {};

    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    SDL_GetRelativeMouseState(&mouseDeltaX, &mouseDeltaY);
    // 即使释放捕获也要持续清空 SDL 累积量，但自由光标移动不能驱动相机。
    if (gamePointerEnabled && relativeMouseModeEnabled)
    {
        actionState.mouseDeltaX = mouseDeltaX;
        actionState.mouseDeltaY = mouseDeltaY;
    }

    const bool* state = SDL_GetKeyboardState(nullptr);
    std::set<std::string> held;
    for (const auto& binding : gameplayScancodes)
        if (state[binding.second]) held.insert(binding.first);
    const auto buttons = SDL_GetMouseState(nullptr, nullptr);
    if (buttons & SDL_BUTTON_LMASK) held.insert("MouseLeft");
    if (buttons & SDL_BUTTON_RMASK) held.insert("MouseRight");
    if (buttons & SDL_BUTTON_MMASK) held.insert("MouseMiddle");
    // 失焦时沿用 UI 仲裁的禁用规则；自由指针仍可点击，但不产生相对视角轴。
    const bool focused = SDL_GetKeyboardFocus() == window->GetNativeHandle();
    actionState.gameplay = gameplayMap.Sample(held, gameKeyboardEnabled && focused,
        gamePointerEnabled && focused, actionState.mouseDeltaX, actionState.mouseDeltaY);
    // 合成 action 只在 gameplay action 层合并；不要求 SDL 焦点，也不改变 held/SDL 状态。
    for (auto& entry : injectedPrevious)
    {
        const bool down = injectedActions.count(entry.first) != 0;
        auto& actions = actionState.gameplay["actions"];
        auto& node = actions[entry.first];
        const bool sdlHeld = node.value("held", false);
        node["held"] = sdlHeld || down;
        if (down && !entry.second) node["pressed"] = true;
        if (!down && entry.second) node["released"] = true;
        entry.second = down;
    }
    if (!gameKeyboardEnabled || !focused)
    {
        return;
    }

    actionState.moveForward = state[SDL_SCANCODE_W];
    actionState.moveBackward = state[SDL_SCANCODE_S];
    actionState.moveLeft = state[SDL_SCANCODE_A];
    actionState.moveRight = state[SDL_SCANCODE_D];
    actionState.moveDown = state[SDL_SCANCODE_Q];
    actionState.moveUp = state[SDL_SCANCODE_E];
}

void InputSubsystem::InjectAction(const std::string& action, bool pressed)
{
    if (action.empty() || action.size() > 128)
        throw std::invalid_argument("Injected input action name is invalid");
    injectedPrevious.try_emplace(action, false);
    if (pressed) injectedActions.insert(action);
    else injectedActions.erase(action);
}

void InputSubsystem::SetRelativeMouseModeRequested(bool requested)
{
    relativeMouseModeRequested = requested;
    ApplyRelativeMouseMode();
}

void InputSubsystem::SetRelativeMouseModeAllowed(bool allowed)
{
    relativeMouseModeAllowed = allowed;
    ApplyRelativeMouseMode();
}

void InputSubsystem::ToggleRelativeMouseModeRequest()
{
    SetRelativeMouseModeRequested(!relativeMouseModeRequested);
}

void InputSubsystem::ApplyRelativeMouseMode()
{
    const bool shouldEnable = relativeMouseModeRequested && relativeMouseModeAllowed;
    if (relativeMouseModeEnabled == shouldEnable)
    {
        return;
    }

    relativeMouseModeEnabled = shouldEnable;
    window->SetRelativeMouseMode(shouldEnable);
    SDL_GetRelativeMouseState(nullptr, nullptr);
}

} // namespace VL
