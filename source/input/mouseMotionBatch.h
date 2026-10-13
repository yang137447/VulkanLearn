#pragma once

#include <utility>
#include <vector>

#include "platform/platformEvent.h"

namespace VL
{
inline void CoalesceRelativeMouseMotion(std::vector<PlatformEvent>& events)
{
    size_t count = 0;
    for (size_t index = 0; index < events.size(); ++index)
    {
        auto& current = events[index];
        if (count != 0 && current.type == PlatformEventType::MouseMove)
        {
            auto& previous = events[count - 1];
            if (previous.type == PlatformEventType::MouseMove &&
                previous.windowId == current.windowId && previous.shift == current.shift &&
                previous.control == current.control && previous.alt == current.alt &&
                previous.super == current.super)
            {
                // 相机每帧消费总位移；UI 只需最后位置，不能跨点击/按键/窗口边界合并。
                previous.mouseX = current.mouseX;
                previous.mouseY = current.mouseY;
                previous.mouseDeltaX += current.mouseDeltaX;
                previous.mouseDeltaY += current.mouseDeltaY;
                continue;
            }
        }
        if (count != index) events[count] = std::move(current);
        ++count;
    }
    events.resize(count);
}
}
