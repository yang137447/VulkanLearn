#pragma once

#include "platform/platformEvent.h"

namespace VL
{
inline bool ShouldReleaseScenePointerForEditor(PlatformEventType type, bool editorHit)
{
    // 相对鼠标移动不能解除场景捕获；面板上的明确点击或滚轮则必须交还 UI。
    return editorHit && (type == PlatformEventType::MouseButtonDown ||
        type == PlatformEventType::MouseWheel);
}
}
