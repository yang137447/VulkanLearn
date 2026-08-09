#pragma once

#include "core/runtimeResult.h"

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
};

// Owns the current SDL keyboard/mouse sampling path and exposes engine-facing
// input intent. Controller code consumes InputActionState and no longer talks
// directly to SDL or the platform window.
class InputSubsystem
{
public:
    RuntimeResult<void> Initialize(PlatformWindow& window);

    void UpdateActionState();
    const InputActionState& GetActionState() const { return actionState; }

    void SetRelativeMouseModeEnabled(bool enabled);
    void ToggleRelativeMouseMode();
    bool IsRelativeMouseModeEnabled() const { return relativeMouseModeEnabled; }
    void SetGameKeyboardEnabled(bool enabled) { gameKeyboardEnabled = enabled; }
    void SetGamePointerEnabled(bool enabled) { gamePointerEnabled = enabled; }

private:
    PlatformWindow* window = nullptr;
    InputActionState actionState;
    bool relativeMouseModeEnabled = false;
    bool gameKeyboardEnabled = true;
    bool gamePointerEnabled = true;
};

} // namespace VL
