#pragma once

#include <string>

#include "core/runtimeResult.h"

struct SDL_Window;

namespace VL
{

// Small SDL window wrapper used as the current PlatformWindow boundary. It owns
// the native SDL_Window and exposes only the window operations engine code needs.
class PlatformWindow
{
public:
    PlatformWindow() = default;
    ~PlatformWindow();

    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;

    PlatformWindow(PlatformWindow&& other) noexcept;
    PlatformWindow& operator=(PlatformWindow&& other) noexcept;

    RuntimeResult<void> Create(
        const char* title,
        int width,
        int height,
        bool hidden,
        bool resizable);
    void Destroy();

    void CenterOnScreen();
    void Show();
    void SetTitle(const std::string& title);
    void SetSize(int width, int height);
    void SetRelativeMouseMode(bool enabled);

    SDL_Window* GetNativeHandle() const { return window; }
    bool IsValid() const { return window != nullptr; }

private:
    SDL_Window* window = nullptr;
    std::string currentTitle;
};

} // namespace VL
