#include "debugProjection.h"
#include <algorithm>
#include <cmath>

namespace VL::Debug
{
namespace
{
Eigen::Vector3f ViewPoint(const Camera& camera, const Eigen::Vector3f& point)
{
    const Eigen::Vector3f delta = point - camera.position;
    return {delta.dot(camera.right), delta.dot(camera.up), delta.dot(camera.forward)};
}
Eigen::Vector2f ScreenPoint(const Camera& camera, const Eigen::Vector3f& point, float width, float height)
{
    const float focal = width * 0.5f / std::tan(camera.horizontalFovDegrees * 0.008726646259971648f);
    return {width * 0.5f + point.x() * focal / point.z(), height * 0.5f - point.y() * focal / point.z()};
}
float DeviceDepth(const Camera& camera, float viewZ)
{
    // 与 Camera 的 Vulkan 正向 Z 投影一致：near=0，far=1。
    return camera.clipFar / (camera.clipFar - camera.clipNear) * (1 - camera.clipNear / viewZ);
}
bool Clip(float start, float delta, float minimum, float maximum, float& lower, float& upper)
{
    if (std::abs(delta) < 1e-8f) return start >= minimum && start <= maximum;
    const float a = (minimum - start) / delta, b = (maximum - start) / delta;
    lower = std::max(lower, std::min(a, b)); upper = std::min(upper, std::max(a, b));
    return lower <= upper;
}
}
bool ProjectPoint(const Camera& camera, const Eigen::Vector3f& point, float width, float height,
    Eigen::Vector2f& pixel, float* depth)
{
    if (width <= 0 || height <= 0) return false;
    const auto local = ViewPoint(camera, point);
    if (local.z() < camera.clipNear || local.z() > camera.clipFar) return false;
    pixel = ScreenPoint(camera, local, width, height);
    if (depth) *depth = DeviceDepth(camera, local.z());
    return pixel.x() >= 0 && pixel.x() < width && pixel.y() >= 0 && pixel.y() < height;
}
bool ProjectLine(const Camera& camera, const Eigen::Vector3f& a, const Eigen::Vector3f& b,
    float width, float height, ScreenLine& result)
{
    if (width <= 0 || height <= 0) return false;
    const auto begin = ViewPoint(camera, a), end = ViewPoint(camera, b);
    const Eigen::Vector3f delta = end - begin;
    float lower = 0, upper = 1;
    // 从编辑器辅助线提取：先裁近远面，再透视除法，避免跨眼点线段翻转/无限放大。
    if (!Clip(begin.z(), delta.z(), camera.clipNear, camera.clipFar, lower, upper)) return false;
    const Eigen::Vector3f firstView = begin + delta * lower, lastView = begin + delta * upper;
    const auto first = ScreenPoint(camera, firstView, width, height);
    const auto last = ScreenPoint(camera, lastView, width, height);
    const float firstDepth = DeviceDepth(camera, firstView.z()), lastDepth = DeviceDepth(camera, lastView.z());
    const Eigen::Vector2f screenDelta = last - first;
    lower = 0; upper = 1;
    if (!Clip(first.x(), screenDelta.x(), 0, width, lower, upper) ||
        !Clip(first.y(), screenDelta.y(), 0, height, lower, upper)) return false;
    // device Z 在屏幕线段上为仿射量；视口裁剪后也必须同步裁剪深度，不能插值 view Z。
    result = {first + screenDelta * lower, first + screenDelta * upper,
        firstDepth + (lastDepth - firstDepth) * lower, firstDepth + (lastDepth - firstDepth) * upper};
    return true;
}
}
