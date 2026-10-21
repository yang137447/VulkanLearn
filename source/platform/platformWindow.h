#pragma once

#include <cstdint>
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
    // 自动化验收用：显式请求 OS 输入焦点，避免后台启动时 SDL_GetKeyboardFocus 为空。
    void Focus();
    void SetTitle(const std::string& title);
    void SetSize(int width, int height);
    // 物理像素尺寸（受系统缩放影响），与 SDL 的逻辑窗口尺寸不是同一个量。
    // Vulkan swapchain extent 必须用它；UI 布局用逻辑尺寸。选择用出参而不是返回结构，
    // 是为了避免本头文件为了一个宽高对再引入一个类型。
    void GetSizeInPixels(int& outWidth, int& outHeight) const;
    void SetRelativeMouseMode(bool enabled);
    void SetMouseConstraint(int x, int y, int width, int height);
    void ClearMouseConstraint();
    void SetTextInputEnabled(bool enabled);
    void SetTextInputArea(int x, int y, int width, int height);

    SDL_Window* GetNativeHandle() const { return window; }
    // SDL 的窗口 id。事件带的就是这个值，因此它是"事件属于哪个窗口"的权威身份；
    // 窗口序号只在注册表内部使用，两者靠本函数对应。
    uint32_t GetId() const;
    bool IsValid() const { return window != nullptr; }
    bool IsTextInputEnabled() const { return textInputEnabled; }

private:
    SDL_Window* window = nullptr;
    std::string currentTitle;
    bool textInputEnabled = false;
    bool mouseConstraintSet = false;
    int mouseConstraintX = 0;
    int mouseConstraintY = 0;
    int mouseConstraintWidth = 0;
    int mouseConstraintHeight = 0;
};

} // namespace VL
