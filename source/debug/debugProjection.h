#pragma once
#include <Eigen/Core>

namespace VL::Debug
{
// 冻结相机的值投影，+Y 世界向上、屏幕向下；不依赖编辑器 Actor 或场景对象。
struct Camera
{
    Eigen::Vector3f position{0, 0, 0}, forward{0, 0, -1}, right{1, 0, 0}, up{0, 1, 0};
    float horizontalFovDegrees = 60, clipNear = 0.1f, clipFar = 1000;
};
struct ScreenLine { Eigen::Vector2f begin, end; float beginDepth = 0, endDepth = 0; };
bool ProjectPoint(const Camera& camera, const Eigen::Vector3f& point, float width, float height,
    Eigen::Vector2f& pixel, float* depth = nullptr);
bool ProjectLine(const Camera& camera, const Eigen::Vector3f& begin, const Eigen::Vector3f& end,
    float width, float height, ScreenLine& result);
}
