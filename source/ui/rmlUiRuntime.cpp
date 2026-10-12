#include "ui/rmlUiRuntime.h"

#include <RmlUi/Core.h>

#include <stdexcept>

namespace VL::Ui
{

namespace
{
// 进程级引用计数。RmlUi 自己的初始化不是重入的，因此必须由这里保证配平。
int g_acquireCount = 0;
} // namespace

void RmlUiRuntime::Acquire()
{
    if (g_acquireCount > 0)
    {
        ++g_acquireCount;
        return;
    }

    // 系统接口与全局渲染接口由各自的 owner 在调用本函数**之前**设置：
    // 它们是 `UiSubsystem` 的成员（每窗口一份），而这里只负责 RmlUi 自身的初始化。
    // 顺序要求是 "SetSystemInterface / SetRenderInterface 先于 Initialise"，
    // 这个约束由调用方满足，违反时 RmlUi 会自行报错而不是静默降级。
    if (!Rml::Initialise())
    {
        throw std::runtime_error("RmlUi failed to initialize.");
    }
    g_acquireCount = 1;
}

void RmlUiRuntime::Release()
{
    if (g_acquireCount == 0)
    {
        return;
    }

    --g_acquireCount;
    if (g_acquireCount == 0)
    {
        Rml::Shutdown();
    }
}

bool RmlUiRuntime::IsInitialized()
{
    return g_acquireCount > 0;
}

std::string RmlUiRuntime::ContextNameFor(size_t windowIndex)
{
    return "vulkanlearn-window-" + std::to_string(windowIndex);
}

} // namespace VL::Ui
