#include "ui/viewport/editorViewportGeometry.h"

#include <algorithm>
#include <cmath>

namespace VL
{
namespace
{

int64_t RightEdge(const UiViewportRect& rect) noexcept
{
    // 先提升两种整数到同一有符号空间，避免负偏移被 uint32 转换和边界加法溢出。
    return static_cast<int64_t>(rect.x) + static_cast<int64_t>(rect.width);
}

int64_t BottomEdge(const UiViewportRect& rect) noexcept
{
    return static_cast<int64_t>(rect.y) + static_cast<int64_t>(rect.height);
}

bool HasArea(const UiViewportRect& rect) noexcept
{
    return rect.width != 0 && rect.height != 0;
}

} // namespace

bool ValidateViewportRect(
    const UiViewportRect& rect,
    uint32_t framebufferWidth,
    uint32_t framebufferHeight) noexcept
{
    return HasArea(rect) &&
        framebufferWidth != 0 && framebufferHeight != 0 &&
        rect.x >= 0 && rect.y >= 0 &&
        RightEdge(rect) <= static_cast<int64_t>(framebufferWidth) &&
        BottomEdge(rect) <= static_cast<int64_t>(framebufferHeight);
}

UiViewportRect ClipViewportRect(
    const UiViewportRect& rect,
    uint32_t framebufferWidth,
    uint32_t framebufferHeight,
    bool visible) noexcept
{
    if (!visible || !HasArea(rect) || framebufferWidth == 0 || framebufferHeight == 0)
    {
        return {};
    }

    const int64_t left = std::max<int64_t>(0, rect.x);
    const int64_t top = std::max<int64_t>(0, rect.y);
    const int64_t right = std::min<int64_t>(framebufferWidth, RightEdge(rect));
    const int64_t bottom = std::min<int64_t>(framebufferHeight, BottomEdge(rect));
    if (right <= left || bottom <= top)
    {
        return {};
    }

    // left/top 只能是零或原 int32 偏移；交集尺寸不超过 framebuffer 的 uint32 尺寸。
    return {
        static_cast<int32_t>(left),
        static_cast<int32_t>(top),
        static_cast<uint32_t>(right - left),
        static_cast<uint32_t>(bottom - top)};
}

bool ContainsViewportPoint(
    const UiViewportRect& rect,
    float mouseX,
    float mouseY) noexcept
{
    if (!HasArea(rect) || !std::isfinite(mouseX) || !std::isfinite(mouseY))
    {
        return false;
    }

    // 用 double 比较整数边界，防止大尺寸转 float 后将右/下边界舍入成内部点。
    const double x = mouseX;
    const double y = mouseY;
    return x >= static_cast<double>(rect.x) && x < static_cast<double>(RightEdge(rect)) &&
        y >= static_cast<double>(rect.y) && y < static_cast<double>(BottomEdge(rect));
}

std::optional<UiViewportPoint> MapViewportPoint(
    const UiViewportRect& rect,
    float mouseX,
    float mouseY) noexcept
{
    if (!ContainsViewportPoint(rect, mouseX, mouseY))
    {
        return std::nullopt;
    }

    return UiViewportPoint{
        static_cast<float>(static_cast<double>(mouseX) - static_cast<double>(rect.x)),
        static_cast<float>(static_cast<double>(mouseY) - static_cast<double>(rect.y)),
        rect.width,
        rect.height};
}

std::optional<float> ViewportAspect(const UiViewportRect& rect) noexcept
{
    if (!HasArea(rect))
    {
        return std::nullopt;
    }
    return static_cast<float>(rect.width) / static_cast<float>(rect.height);
}

} // namespace VL
