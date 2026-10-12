#include "platform/platformApplication.h"

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"

namespace VL
{

namespace
{

PlatformKey TranslateKey(SDL_Scancode scancode)
{
    switch (scancode)
    {
    case SDL_SCANCODE_TAB: return PlatformKey::Tab;
    case SDL_SCANCODE_LEFT: return PlatformKey::Left;
    case SDL_SCANCODE_RIGHT: return PlatformKey::Right;
    case SDL_SCANCODE_UP: return PlatformKey::Up;
    case SDL_SCANCODE_DOWN: return PlatformKey::Down;
    case SDL_SCANCODE_PAGEUP: return PlatformKey::PageUp;
    case SDL_SCANCODE_PAGEDOWN: return PlatformKey::PageDown;
    case SDL_SCANCODE_HOME: return PlatformKey::Home;
    case SDL_SCANCODE_END: return PlatformKey::End;
    case SDL_SCANCODE_INSERT: return PlatformKey::Insert;
    case SDL_SCANCODE_DELETE: return PlatformKey::Delete;
    case SDL_SCANCODE_BACKSPACE: return PlatformKey::Backspace;
    case SDL_SCANCODE_SPACE: return PlatformKey::Space;
    case SDL_SCANCODE_RETURN: return PlatformKey::Enter;
    case SDL_SCANCODE_ESCAPE: return PlatformKey::Escape;
    case SDL_SCANCODE_A: return PlatformKey::A;
    case SDL_SCANCODE_S: return PlatformKey::S;
    case SDL_SCANCODE_C: return PlatformKey::C;
    case SDL_SCANCODE_V: return PlatformKey::V;
    case SDL_SCANCODE_X: return PlatformKey::X;
    case SDL_SCANCODE_Y: return PlatformKey::Y;
    case SDL_SCANCODE_Z: return PlatformKey::Z;
    case SDL_SCANCODE_F1: return PlatformKey::F1;
    case SDL_SCANCODE_F10: return PlatformKey::F10;
    case SDL_SCANCODE_0: return PlatformKey::Num0;
    case SDL_SCANCODE_1: return PlatformKey::Num1;
    case SDL_SCANCODE_2: return PlatformKey::Num2;
    case SDL_SCANCODE_3: return PlatformKey::Num3;
    case SDL_SCANCODE_4: return PlatformKey::Num4;
    case SDL_SCANCODE_5: return PlatformKey::Num5;
    case SDL_SCANCODE_6: return PlatformKey::Num6;
    case SDL_SCANCODE_7: return PlatformKey::Num7;
    case SDL_SCANCODE_8: return PlatformKey::Num8;
    case SDL_SCANCODE_9: return PlatformKey::Num9;
    default: return PlatformKey::Unknown;
    }
}

PlatformMouseButton TranslateMouseButton(uint8_t button)
{
    switch (button)
    {
    case SDL_BUTTON_LEFT: return PlatformMouseButton::Left;
    case SDL_BUTTON_RIGHT: return PlatformMouseButton::Right;
    case SDL_BUTTON_MIDDLE: return PlatformMouseButton::Middle;
    case SDL_BUTTON_X1: return PlatformMouseButton::X1;
    case SDL_BUTTON_X2: return PlatformMouseButton::X2;
    default: return PlatformMouseButton::Unknown;
    }
}

PlatformGamepadButton TranslateGamepadButton(uint8_t button)
{
    switch (static_cast<SDL_GamepadButton>(button))
    {
    case SDL_GAMEPAD_BUTTON_SOUTH: return PlatformGamepadButton::South;
    case SDL_GAMEPAD_BUTTON_EAST: return PlatformGamepadButton::East;
    case SDL_GAMEPAD_BUTTON_WEST: return PlatformGamepadButton::West;
    case SDL_GAMEPAD_BUTTON_NORTH: return PlatformGamepadButton::North;
    case SDL_GAMEPAD_BUTTON_BACK: return PlatformGamepadButton::Back;
    case SDL_GAMEPAD_BUTTON_START: return PlatformGamepadButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return PlatformGamepadButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PlatformGamepadButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return PlatformGamepadButton::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return PlatformGamepadButton::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return PlatformGamepadButton::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return PlatformGamepadButton::DpadRight;
    default: return PlatformGamepadButton::Unknown;
    }
}

PlatformGamepadAxis TranslateGamepadAxis(uint8_t axis)
{
    switch (static_cast<SDL_GamepadAxis>(axis))
    {
    case SDL_GAMEPAD_AXIS_LEFTX: return PlatformGamepadAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY: return PlatformGamepadAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX: return PlatformGamepadAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY: return PlatformGamepadAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return PlatformGamepadAxis::LeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return PlatformGamepadAxis::RightTrigger;
    default: return PlatformGamepadAxis::Unknown;
    }
}

void ApplyModifiers(PlatformEvent& platformEvent, SDL_Keymod modifiers)
{
    platformEvent.shift = (modifiers & SDL_KMOD_SHIFT) != 0;
    platformEvent.control = (modifiers & SDL_KMOD_CTRL) != 0;
    platformEvent.alt = (modifiers & SDL_KMOD_ALT) != 0;
    platformEvent.super = (modifiers & SDL_KMOD_GUI) != 0;
}

} // namespace

PlatformApplication::~PlatformApplication()
{
    Shutdown();
}

RuntimeResult<void> PlatformApplication::Initialize(const PlatformApplicationDesc& desc)
{
    if (desc.windowWidth <= 0 || desc.windowHeight <= 0)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.InvalidWindowSize",
            "PlatformApplication requires a positive window size."));
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.SdlInitFailed",
            SDL_GetError()));
    }
    sdlInitialized = true;

    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.VulkanLoaderFailed",
            SDL_GetError()));
    }
    vulkanLoaderLoaded = true;

    // 主窗口进入注册表并成为序号 0。`GetWindow()` 返回它，因此既有调用点语义不变。
    windows.push_back(std::make_unique<PlatformWindow>());
    auto windowResult = windows.front()->Create(
        desc.windowTitle.c_str(),
        desc.windowWidth,
        desc.windowHeight,
        desc.hiddenWindow,
        desc.resizableWindow);
    if (windowResult.IsFailure())
    {
        windows.clear();
        return windowResult;
    }

    unsigned int count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.VulkanExtensionsFailed",
            SDL_GetError()));
    }

    vulkanExtensions.clear();
    for (unsigned int i = 0; i < count; ++i)
    {
        vulkanExtensions.push_back(extensions[i]);
    }

    return RuntimeResult<void>::Success();
}

void PlatformApplication::Shutdown()
{
    vulkanExtensions.clear();
    CloseAllGamepads();
    // 逆序销毁：后建的窗口可能引用了先建窗口所在的资源（例如共享的 UI 上下文），
    // 逆序保证依赖方先消失。
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
    {
        if (*it != nullptr)
        {
            (*it)->Destroy();
        }
    }
    windows.clear();

    if (vulkanLoaderLoaded)
    {
        SDL_Vulkan_UnloadLibrary();
        vulkanLoaderLoaded = false;
    }

    if (sdlInitialized)
    {
        SDL_Quit();
        sdlInitialized = false;
    }
}

PlatformWindow* PlatformApplication::FindWindowById(uint32_t windowId)
{
    for (auto& candidate : windows)
    {
        if (candidate != nullptr && candidate->GetId() == windowId)
        {
            return candidate.get();
        }
    }
    return nullptr;
}

RuntimeResult<size_t> PlatformApplication::OpenWindow(const WindowDesc& desc)
{
    if (!sdlInitialized)
    {
        return RuntimeResult<size_t>::Failure(MakeRuntimeError(
            "PlatformApplication.OpenWindowBeforeInitialize",
            "A window cannot be opened before platform initialization."));
    }
    if (desc.width <= 0 || desc.height <= 0)
    {
        return RuntimeResult<size_t>::Failure(MakeRuntimeError(
            "PlatformApplication.OpenWindowInvalidSize",
            "PlatformApplication requires a positive window size."));
    }

    auto candidate = std::make_unique<PlatformWindow>();
    // Create 的 hidden 与 visible 语义相反：这里按"是否可见"传入，转换只在这一处发生。
    auto result = candidate->Create(
        desc.title.c_str(), desc.width, desc.height, !desc.visible, desc.resizable);
    if (result.IsFailure())
    {
        return RuntimeResult<size_t>::Failure(result.Error());
    }

    // 新窗口必须有自己的 Vulkan surface。它不在这里创建：surface 属于呈现层，
    // 平台层只负责窗口本身。调用方拿到序号后自行创建对应呈现目标。
    windows.push_back(std::move(candidate));
    return RuntimeResult<size_t>::Success(windows.size() - 1);
}

bool PlatformApplication::CloseWindow(size_t windowIndex)
{
    // 主窗口不在此关闭：它的关闭等于应用退出，必须走 Quit 路径而不是静默销毁。
    if (windowIndex == 0 || windowIndex >= windows.size())
    {
        return false;
    }
    if (windows[windowIndex] != nullptr)
    {
        windows[windowIndex]->Destroy();
    }
    windows.erase(windows.begin() + static_cast<std::ptrdiff_t>(windowIndex));
    return true;
}

bool PlatformApplication::OpenGamepad(uint32_t instanceId)
{
    if (gamepads.find(instanceId) != gamepads.end())
    {
        return false;
    }

    SDL_Gamepad* gamepad = SDL_OpenGamepad(static_cast<SDL_JoystickID>(instanceId));
    if (gamepad == nullptr)
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_INPUT,
            "Unable to open gamepad %u: %s",
            instanceId,
            SDL_GetError());
        return false;
    }

    gamepads.emplace(instanceId, gamepad);
    return true;
}

bool PlatformApplication::CloseGamepad(uint32_t instanceId)
{
    auto gamepadIt = gamepads.find(instanceId);
    if (gamepadIt == gamepads.end())
    {
        return false;
    }

    SDL_CloseGamepad(gamepadIt->second);
    gamepads.erase(gamepadIt);
    return true;
}

void PlatformApplication::CloseAllGamepads()
{
    for (const auto& gamepadEntry : gamepads)
    {
        SDL_CloseGamepad(gamepadEntry.second);
    }
    gamepads.clear();
}
namespace
{
// SDL 的事件里，窗口来源的事件都带一个 windowID；手柄等设备来源的事件没有。
// 用这个模板统一盖章，而不是在每个分支里各写一次 —— 后者漏掉一个分支就是
// "某个事件莫名属于主窗口"，而那种错误不会报错、只会表现为输入走错窗口。
template <typename SDLEvent>
void StampWindowId(PlatformEvent& out, const SDLEvent& source)
{
    out.windowId = static_cast<uint32_t>(source.windowID);
}
} // namespace

void PlatformApplication::PollEvents(std::vector<PlatformEvent>& outEvents)
{
    outEvents.clear();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        {
            PlatformEvent platformEvent;
            platformEvent.type = event.type == SDL_EVENT_KEY_DOWN ?
                PlatformEventType::KeyDown : PlatformEventType::KeyUp;
            platformEvent.key = TranslateKey(event.key.scancode);
            platformEvent.repeat = event.key.repeat;
            ApplyModifiers(platformEvent, event.key.mod);
            StampWindowId(platformEvent, event.key);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_TEXT_INPUT)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::TextInput;
            platformEvent.text = event.text.text != nullptr ? event.text.text : "";
            StampWindowId(platformEvent, event.text);
            outEvents.push_back(std::move(platformEvent));
            continue;
        }

        if (event.type == SDL_EVENT_TEXT_EDITING)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::TextEditing;
            platformEvent.text = event.edit.text != nullptr ? event.edit.text : "";
            platformEvent.textEditingStart = event.edit.start;
            platformEvent.textEditingLength = event.edit.length;
            StampWindowId(platformEvent, event.edit);
            outEvents.push_back(std::move(platformEvent));
            continue;
        }

        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::MouseMove;
            platformEvent.mouseX = event.motion.x;
            platformEvent.mouseY = event.motion.y;
            platformEvent.mouseDeltaX = event.motion.xrel;
            platformEvent.mouseDeltaY = event.motion.yrel;
            ApplyModifiers(platformEvent, SDL_GetModState());
            StampWindowId(platformEvent, event.motion);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        {
            PlatformEvent platformEvent;
            platformEvent.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ?
                PlatformEventType::MouseButtonDown : PlatformEventType::MouseButtonUp;
            platformEvent.mouseButton = TranslateMouseButton(event.button.button);
            platformEvent.mouseX = event.button.x;
            platformEvent.mouseY = event.button.y;
            ApplyModifiers(platformEvent, SDL_GetModState());
            StampWindowId(platformEvent, event.button);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_MOUSE_WHEEL)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::MouseWheel;
            const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            platformEvent.wheelX = event.wheel.x * direction;
            platformEvent.wheelY = event.wheel.y * direction;
            platformEvent.mouseX = event.wheel.mouse_x;
            platformEvent.mouseY = event.wheel.mouse_y;
            ApplyModifiers(platformEvent, SDL_GetModState());
            StampWindowId(platformEvent, event.wheel);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::MouseLeave;
            StampWindowId(platformEvent, event.window);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_GAMEPAD_ADDED)
        {
            const uint32_t instanceId = static_cast<uint32_t>(event.gdevice.which);
            if (OpenGamepad(instanceId))
            {
                PlatformEvent platformEvent;
                platformEvent.type = PlatformEventType::GamepadConnected;
                platformEvent.gamepadConnected = true;
                outEvents.push_back(platformEvent);
            }
            continue;
        }

        if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
        {
            const uint32_t instanceId = static_cast<uint32_t>(event.gdevice.which);
            if (CloseGamepad(instanceId))
            {
                PlatformEvent platformEvent;
                platformEvent.type = PlatformEventType::GamepadDisconnected;
                platformEvent.gamepadConnected = !gamepads.empty();
                outEvents.push_back(platformEvent);
            }
            continue;
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        {
            PlatformEvent platformEvent;
            platformEvent.type = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ?
                PlatformEventType::GamepadButtonDown : PlatformEventType::GamepadButtonUp;
            platformEvent.gamepadButton = TranslateGamepadButton(event.gbutton.button);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::GamepadAxisMotion;
            platformEvent.gamepadAxis = TranslateGamepadAxis(event.gaxis.axis);
            platformEvent.gamepadAxisValue = event.gaxis.value < 0 ?
                static_cast<float>(event.gaxis.value) / 32768.0f :
                static_cast<float>(event.gaxis.value) / 32767.0f;
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        {
            PlatformEvent platformEvent;
            platformEvent.type = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ?
                PlatformEventType::WindowFocusGained : PlatformEventType::WindowFocusLost;
            StampWindowId(platformEvent, event.window);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::WindowResized;
            // data1/data2 是逻辑尺寸；绘制表面需要物理像素，因此在这里一并取到。
            // 两者在 100% 缩放下相等，但 swapchain 必须用像素尺寸，不能依赖它们相等。
            platformEvent.width = static_cast<uint32_t>(event.window.data1);
            platformEvent.height = static_cast<uint32_t>(event.window.data2);
            int pixelWidth = 0;
            int pixelHeight = 0;
            SDL_GetWindowSizeInPixels(SDL_GetWindowFromID(event.window.windowID),
                &pixelWidth, &pixelHeight);
            platformEvent.pixelWidth = static_cast<uint32_t>(pixelWidth > 0 ? pixelWidth : 0);
            platformEvent.pixelHeight = static_cast<uint32_t>(pixelHeight > 0 ? pixelHeight : 0);
            StampWindowId(platformEvent, event.window);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_QUIT)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::Quit;
            outEvents.push_back(platformEvent);
        }
    }
}

} // namespace VL
