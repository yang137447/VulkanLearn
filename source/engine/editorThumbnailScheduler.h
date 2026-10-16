#pragma once

#include <chrono>
#include <vector>

#include "platform/platformEvent.h"

namespace VL
{
// GT 缩略图输入退避；消费平台事件和 UI 捕获状态，不加载资源或执行 GPU 工作。
class EditorThumbnailScheduler
{
public:
    using Clock = std::chrono::steady_clock;
    explicit EditorThumbnailScheduler(Clock::time_point now = Clock::now()) : idleSince(now) {}

    bool CanAdvance(Clock::time_point now, const std::vector<PlatformEvent>& events, bool available)
    {
        for (const auto& event : events)
        {
            // 普通悬停不重置退避，否则持续移动鼠标会让可见资源永久等不到预览。
            // 相机与拖动由 available 拦截；滚动、点击、输入和 resize 仍等待稳定。
            switch (event.type)
            {
            case PlatformEventType::MouseWheel:
            case PlatformEventType::MouseButtonDown:
            case PlatformEventType::MouseButtonUp:
            case PlatformEventType::KeyDown:
            case PlatformEventType::KeyUp:
            case PlatformEventType::TextInput:
            case PlatformEventType::TextEditing:
            case PlatformEventType::WindowResized:
                idleSince = now;
                break;
            default:
                break;
            }
        }
        if (!available) idleSince = now;
        return available && now - idleSince >= std::chrono::milliseconds(750);
    }

private:
    Clock::time_point idleSince;
};
} // namespace VL
