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
}

PlatformWindow& PlatformWindow::operator=(PlatformWindow&& other) noexcept
{
    if (this != &other)
    {
        Destroy();
        window = other.window;
        currentTitle = std::move(other.currentTitle);
        textInputEnabled = other.textInputEnabled;
        other.window = nullptr;
        other.currentTitle.clear();
        other.textInputEnabled = false;
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

void PlatformWindow::SetRelativeMouseMode(bool enabled)
{
    SDL_SetWindowRelativeMouseMode(window, enabled);
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
