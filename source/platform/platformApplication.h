#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"

struct SDL_Gamepad;

namespace VL
{

struct PlatformApplicationDesc
{
    std::string windowTitle = "VulkanRenderer";
    int windowWidth = 1280;
    int windowHeight = 720;
    bool hiddenWindow = true;
    bool resizableWindow = true;
};

// 后续窗口的创建参数。与主窗口分开，是因为主窗口带启动语义（隐藏、尺寸来自配置），
// 而后续窗口是运行期按需创建的，参数应当显式传入而不是复用启动配置。
struct WindowDesc
{
    std::string title = "VulkanLearn";
    int width = 960;
    int height = 600;
    bool visible = true;
    bool resizable = true;
};

// Owns SDL startup, Vulkan loader setup, the main window, and raw SDL event
// collection. It deliberately returns platform-neutral PlatformEvent values so
// EngineLoop does not branch on SDL_Event directly.
class PlatformApplication
{
public:
    PlatformApplication() = default;
    ~PlatformApplication();

    PlatformApplication(const PlatformApplication&) = delete;
    PlatformApplication& operator=(const PlatformApplication&) = delete;

    RuntimeResult<void> Initialize(const PlatformApplicationDesc& desc);
    void Shutdown();

    void PollEvents(std::vector<PlatformEvent>& outEvents);

    // 主窗口（序号 0）。保留这个入口，是为了让"多窗口尚未接线"的既有调用点无需改动 ——
    // 它们要的确实是主窗口。需要**其它**窗口的调用点必须显式按身份取，见下面两个。
    PlatformWindow& GetWindow() { return *windows.front(); }
    const PlatformWindow& GetWindow() const { return *windows.front(); }

    // 窗口注册表。序号即身份：`PlatformEvent::windowId` 用它，`RmlUiRuntime::ContextNameFor`
    // 也用它，因此两者是同一个身份而不是两套编号。
    size_t WindowCount() const { return windows.size(); }
    PlatformWindow* FindWindowById(uint32_t windowId);
    // 按需开窗（多窗口的入口）。返回新窗口的序号；失败时返回窗口总数（即未创建）。
    // `visible=false` 用于测试与"先建后显"的场景。
    RuntimeResult<size_t> OpenWindow(const WindowDesc& desc);
    // 关闭某个非主窗口；主窗口不在此处关闭（它由 Shutdown 负责，且它的关闭意味着退出）。
    bool CloseWindow(size_t windowIndex);

    std::vector<const char*>& GetVulkanExtensions() { return vulkanExtensions; }

private:
    bool OpenGamepad(uint32_t instanceId);
    bool CloseGamepad(uint32_t instanceId);
    void CloseAllGamepads();

    bool sdlInitialized = false;
    bool vulkanLoaderLoaded = false;
    // 注册表持有窗口；`GetWindow()` 返回 front()（主窗口）。
    // 用 unique_ptr 而不是值语义容器：PlatformWindow 不可复制，且地址必须稳定 ——
    // 子系统持的是 `PlatformWindow*`。
    std::vector<std::unique_ptr<PlatformWindow>> windows;
    std::vector<const char*> vulkanExtensions;
    std::unordered_map<uint32_t, SDL_Gamepad*> gamepads;
};

} // namespace VL
