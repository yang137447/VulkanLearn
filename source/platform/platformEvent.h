#pragma once

#include <cstdint>
#include <string>

namespace VL
{

enum class PlatformEventType
{
    Unknown,
    KeyDown,
    KeyUp,
    TextInput,
    TextEditing,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    MouseLeave,
    GamepadConnected,
    GamepadDisconnected,
    GamepadButtonDown,
    GamepadButtonUp,
    GamepadAxisMotion,
    WindowFocusGained,
    WindowFocusLost,
    WindowResized,
    Quit
};

enum class PlatformKey
{
    Unknown,
    Tab,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Insert,
    Delete,
    Backspace,
    Space,
    Enter,
    Escape,
    A,
    C,
    V,
    X,
    Y,
    Z,
    F1,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9
};

enum class PlatformMouseButton
{
    Unknown,
    Left,
    Right,
    Middle,
    X1,
    X2
};

enum class PlatformGamepadButton
{
    Unknown,
    South,
    East,
    West,
    North,
    Back,
    Start,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight
};

enum class PlatformGamepadAxis
{
    Unknown,
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger
};

struct PlatformEvent
{
    PlatformEventType type = PlatformEventType::Unknown;
    PlatformKey key = PlatformKey::Unknown;
    PlatformMouseButton mouseButton = PlatformMouseButton::Unknown;
    PlatformGamepadButton gamepadButton = PlatformGamepadButton::Unknown;
    PlatformGamepadAxis gamepadAxis = PlatformGamepadAxis::Unknown;
    bool repeat = false;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
    bool gamepadConnected = false;
    uint32_t width = 0;
    uint32_t height = 0;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float wheelX = 0.0f;
    float wheelY = 0.0f;
    float gamepadAxisValue = 0.0f;
    int textEditingStart = 0;
    int textEditingLength = 0;
    std::string text;
};

} // namespace VL
