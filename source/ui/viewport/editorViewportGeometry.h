#pragma once

#include "ui/uiRenderSnapshot.h"

#include <cstdint>
#include <optional>

namespace VL
{

// 将 framebuffer 鼠标位置转换为视口左上角起算的局部像素，供输入/拾取消费。
// 不做 DPI 换算、不访问 UI/renderer；局部坐标保留小数，由 renderer 决定像素取整。
struct UiViewportPoint
{
    float x = 0.0f;
    float y = 0.0f;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
};

// 仅完整落在非空 framebuffer 内的非空矩形有效；负偏移应先经过 ClipViewportRect。
bool ValidateViewportRect(
    const UiViewportRect& rect,
    uint32_t framebufferWidth,
    uint32_t framebufferHeight) noexcept;

// 隐藏、空矩形、空 framebuffer 或无交集时返回全零矩形。
// 裁剪后的原点和尺寸共同定义局部像素坐标系，不保留被裁去的内容偏移。
UiViewportRect ClipViewportRect(
    const UiViewportRect& rect,
    uint32_t framebufferWidth,
    uint32_t framebufferHeight,
    bool visible = true) noexcept;

// 鼠标与 rect 必须同属 framebuffer 像素坐标系；使用 [left, right) / [top, bottom)。
// 此处允许尚未裁剪的负偏移；发布给 renderer 前由 ClipViewportRect 建立边界契约。
bool ContainsViewportPoint(
    const UiViewportRect& rect,
    float mouseX,
    float mouseY) noexcept;

std::optional<UiViewportPoint> MapViewportPoint(
    const UiViewportRect& rect,
    float mouseX,
    float mouseY) noexcept;

std::optional<float> ViewportAspect(const UiViewportRect& rect) noexcept;

} // namespace VL
