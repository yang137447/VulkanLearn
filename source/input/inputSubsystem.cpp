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
    return RuntimeResult<void>::Success();
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

    if (!gameKeyboardEnabled)
    {
        return;
    }

    const bool* state = SDL_GetKeyboardState(nullptr);
    actionState.moveForward = state[SDL_SCANCODE_W];
    actionState.moveBackward = state[SDL_SCANCODE_S];
    actionState.moveLeft = state[SDL_SCANCODE_A];
    actionState.moveRight = state[SDL_SCANCODE_D];
    actionState.moveDown = state[SDL_SCANCODE_Q];
    actionState.moveUp = state[SDL_SCANCODE_E];
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
