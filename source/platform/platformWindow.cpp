#include "platform/platformWindow.h"

#include "SDL3/SDL.h"

namespace VL
{

PlatformWindow::~PlatformWindow()
{
    Destroy();
}

PlatformWindow::PlatformWindow(PlatformWindow&& other) noexcept
    : window(other.window)
{
    other.window = nullptr;
}

PlatformWindow& PlatformWindow::operator=(PlatformWindow&& other) noexcept
{
    if (this != &other)
    {
        Destroy();
        window = other.window;
        other.window = nullptr;
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

    return RuntimeResult<void>::Success();
}

void PlatformWindow::Destroy()
{
    if (window != nullptr)
    {
        SDL_DestroyWindow(window);
        window = nullptr;
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

void PlatformWindow::SetTitle(const std::string& title)
{
    SDL_SetWindowTitle(window, title.c_str());
}

void PlatformWindow::SetSize(int width, int height)
{
    SDL_SetWindowSize(window, width, height);
}

void PlatformWindow::SetRelativeMouseMode(bool enabled)
{
    SDL_SetWindowRelativeMouseMode(window, enabled);
}

} // namespace VL
