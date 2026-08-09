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
    if (gamePointerEnabled)
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

void InputSubsystem::SetRelativeMouseModeEnabled(bool enabled)
{
    relativeMouseModeEnabled = enabled;
    window->SetRelativeMouseMode(enabled);
    SDL_GetRelativeMouseState(nullptr, nullptr);
}

void InputSubsystem::ToggleRelativeMouseMode()
{
    SetRelativeMouseModeEnabled(!relativeMouseModeEnabled);
}

} // namespace VL
