#include "platform/platformWindow.h"

#include <utility>

#include "SDL3/SDL.h"

namespace VL
{

PlatformWindow::~PlatformWindow()
{
    Destroy();
}

PlatformWindow::PlatformWindow(PlatformWindow&& other) noexcept
    : window(other.window),
      currentTitle(std::move(other.currentTitle))
{
    other.window = nullptr;
    other.currentTitle.clear();
    textInputEnabled = other.textInputEnabled;
    other.textInputEnabled = false;
    mouseConstraintSet = other.mouseConstraintSet;
    mouseConstraintX = other.mouseConstraintX;
    mouseConstraintY = other.mouseConstraintY;
    mouseConstraintWidth = other.mouseConstraintWidth;
    mouseConstraintHeight = other.mouseConstraintHeight;
    other.mouseConstraintSet = false;
}

PlatformWindow& PlatformWindow::operator=(PlatformWindow&& other) noexcept
{
    if (this != &other)
    {
        Destroy();
        window = other.window;
        currentTitle = std::move(other.currentTitle);
        textInputEnabled = other.textInputEnabled;
        mouseConstraintSet = other.mouseConstraintSet;
        mouseConstraintX = other.mouseConstraintX;
        mouseConstraintY = other.mouseConstraintY;
        mouseConstraintWidth = other.mouseConstraintWidth;
        mouseConstraintHeight = other.mouseConstraintHeight;
        other.window = nullptr;
        other.currentTitle.clear();
        other.textInputEnabled = false;
        other.mouseConstraintSet = false;
    }

    return *this;
}

RuntimeResult<void> PlatformWindow::Create(
    const char* title,
    int width,
    int height,
    bool hidden,
    bool resizable)
{
    Destroy();

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (hidden)
    {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (resizable)
    {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    window = SDL_CreateWindow(title, width, height, flags);
    if (window == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformWindow.CreateFailed",
            SDL_GetError()));
    }

    currentTitle = title != nullptr ? title : "";
    return RuntimeResult<void>::Success();
}

void PlatformWindow::Destroy()
{
    if (window != nullptr)
    {
        SDL_DestroyWindow(window);
        window = nullptr;
        currentTitle.clear();
        textInputEnabled = false;
        mouseConstraintSet = false;
        mouseConstraintX = mouseConstraintY = mouseConstraintWidth = mouseConstraintHeight = 0;
    }
}

void PlatformWindow::CenterOnScreen()
{
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void PlatformWindow::Show()
{
    SDL_ShowWindow(window);
}

void PlatformWindow::Focus()
{
    if (!window) return;
    SDL_SetWindowFocusable(window, true);
    SDL_RaiseWindow(window);
}

void PlatformWindow::SetTitle(const std::string& title)
{
    if (title == currentTitle)
    {
        return;
    }

    SDL_SetWindowTitle(window, title.c_str());
    currentTitle = title;
}

void PlatformWindow::SetSize(int width, int height)
{
    SDL_SetWindowSize(window, width, height);
}

// 逻辑尺寸与像素尺寸是**两个不同的量**，不是同一个数的两种写法：
// SDL 的窗口尺寸是逻辑点（受系统缩放影响），而 Vulkan 的 swapchain extent 必须用物理像素。
// 在 100% 缩放下两者相等，所以这个区别在开发机上不可见 —— 正因为不可见，才必须显式暴露，
// 否则一旦运行在缩放显示器上，swapchain 会被建成逻辑尺寸，画面被拉伸/模糊。
void PlatformWindow::GetSizeInPixels(int& outWidth, int& outHeight) const
{
    outWidth = 0;
    outHeight = 0;
    if (window == nullptr) return;
    SDL_GetWindowSizeInPixels(window, &outWidth, &outHeight);
}

uint32_t PlatformWindow::GetId() const
{
    if (window == nullptr) return 0;
    return static_cast<uint32_t>(SDL_GetWindowID(window));
}

void PlatformWindow::SetRelativeMouseMode(bool enabled)
{
    SDL_SetWindowRelativeMouseMode(window, enabled);
}

void PlatformWindow::SetMouseConstraint(int x, int y, int width, int height)
{
    if (window == nullptr || width <= 0 || height <= 0) return;
    if (mouseConstraintSet && mouseConstraintX == x && mouseConstraintY == y &&
        mouseConstraintWidth == width && mouseConstraintHeight == height)
        return;
    SDL_Rect rect{x, y, width, height};
    if (!SDL_SetWindowMouseRect(window, &rect)) return;
    mouseConstraintSet = true;
    mouseConstraintX = x;
    mouseConstraintY = y;
    mouseConstraintWidth = width;
    mouseConstraintHeight = height;
}

void PlatformWindow::ClearMouseConstraint()
{
    if (window == nullptr || !mouseConstraintSet) return;
    if (!SDL_SetWindowMouseRect(window, nullptr)) return;
    mouseConstraintSet = false;
    mouseConstraintX = mouseConstraintY = mouseConstraintWidth = mouseConstraintHeight = 0;
}

void PlatformWindow::SetTextInputEnabled(bool enabled)
{
    if (textInputEnabled == enabled)
    {
        return;
    }

    if (enabled)
    {
        SDL_StartTextInput(window);
    }
    else
    {
        SDL_StopTextInput(window);
    }
    textInputEnabled = enabled;
}

void PlatformWindow::SetTextInputArea(int x, int y, int width, int height)
{
    SDL_Rect rect{x, y, width, height};
    SDL_SetTextInputArea(window, &rect, 0);
}

} // namespace VL
