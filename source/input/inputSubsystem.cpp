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
    return RuntimeResult<void>::Success();
}

void InputSubsystem::UpdateActionState()
{
    actionState = {};

    if (isMouseCaptured)
    {
        SDL_GetRelativeMouseState(&actionState.mouseDeltaX, &actionState.mouseDeltaY);
    }

    const bool* state = SDL_GetKeyboardState(nullptr);
    actionState.moveForward = state[SDL_SCANCODE_W];
    actionState.moveBackward = state[SDL_SCANCODE_S];
    actionState.moveLeft = state[SDL_SCANCODE_A];
    actionState.moveRight = state[SDL_SCANCODE_D];
    actionState.moveDown = state[SDL_SCANCODE_Q];
    actionState.moveUp = state[SDL_SCANCODE_E];
}

void InputSubsystem::SetMouseCaptured(bool captured)
{
    isMouseCaptured = captured;
    window->SetRelativeMouseMode(captured);
    SDL_GetRelativeMouseState(nullptr, nullptr);
}

void InputSubsystem::ToggleMouseCaptured()
{
    SetMouseCaptured(!isMouseCaptured);
}

} // namespace VL
