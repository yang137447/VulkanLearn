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
    S,
    C,
    V,
    X,
    Y,
    Z,
    F1,
    F10,
    F11,
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
    // 事件来源窗口（SDL 的窗口 id，与 `PlatformWindow::GetId()` 同一身份）。
    // 0 表示"不属于任何窗口"：手柄等设备来源的事件不带它。
    // 多窗口下这是"输入该给谁"的唯一依据；此前没有这个字段，于是所有窗口的事件无法区分，
    // 键盘与文本输入实际上被当成进程全局。
    uint32_t windowId = 0;
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
    // 逻辑尺寸（SDL 的窗口尺寸，受系统缩放影响）。UI 布局用它。
    uint32_t width = 0;
    uint32_t height = 0;
    // 物理像素尺寸，仅 WindowResized 会填。**Vulkan swapchain extent 必须用它**：
    // 缩放下它与上面的逻辑尺寸不同，而当初只用逻辑尺寸在建 swapchain，
    // 于是同一个 resize 事件被两个下游按不同含义消费（UI 布局 vs 绘制表面）。
    // 两者显式并存，是为了让"用错了哪一个"在代码里看得出来。
    uint32_t pixelWidth = 0;
    uint32_t pixelHeight = 0;
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
