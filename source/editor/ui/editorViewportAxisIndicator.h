#pragma once

#include <array>
#include <cmath>

#include <Eigen/Dense>

namespace VL::EditorUi
{
// 视口角落坐标轴指示器的投影数学。
//
// 它刻意不依赖 RmlUi：面板只负责把这里算出来的角度与标签位置写进 DOM，
// 因此"三个轴该怎么摆"这件事可以脱离 UI 框架做模块测试。
// 输入是相机的正交基（forward/right/up，都必须是单位向量），来自冻结的相机快照。

// 指示器的几何口径。
inline constexpr float kAxisIndicatorRadiusPx = 20.0f;
inline constexpr float kAxisIndicatorLabelRadiusPx = 28.0f;

// 一根轴在屏幕上的落点：单位圆内的 (x, y)（x 向右、y 向下）+ 深度。
// 深度是它在相机前方向上的分量：越大越朝向观察者，面板据此把朝前的轴排在后面画。
struct AxisIndicatorProjection
{
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
    // 该轴是否近乎正对或背对相机。此时它的屏幕长度接近 0，方向角没有意义，
    // 面板应当把这一根轴画成一个圆点而不是一条线。
    bool degenerate = true;
};

// 把一条世界方向投影到屏幕单位圆。
//
// screenX = dot(axis, right)，screenY = -dot(axis, up)：屏幕 y 向下，所以取负。
// depth  = dot(axis, forward)，forward 指向观察方向（进屏幕），因此 depth 越大越远；
// 面板需要的是"朝向观察者"，故返回的是取负前的原值并让调用方决定绘制顺序 —— 这里
// 保持返回 dot(axis, forward)，约定"越小越靠近观察者"。
inline AxisIndicatorProjection ProjectAxisIndicator(
    const Eigen::Vector3f& axis,
    const Eigen::Vector3f& right,
    const Eigen::Vector3f& up,
    const Eigen::Vector3f& forward,
    float degenerateThreshold = 0.08f)
{
    AxisIndicatorProjection projection;
    projection.x = axis.dot(right);
    projection.y = -axis.dot(up);
    projection.depth = axis.dot(forward);
    const float screenLengthSquared =
        projection.x * projection.x + projection.y * projection.y;
    projection.degenerate = screenLengthSquared < degenerateThreshold * degenerateThreshold;
    return projection;
}

// 从屏幕单位圆落点得到 CSS rotate 用的角度（度）。
// 基准是"从锚点向右的水平条"，顺时针为正，与 CSS rotate 一致。
// 退化时返回 0：调用方不会用到它，但返回一个确定值是必要的，否则角度会随浮点噪声乱跳。
inline float AxisIndicatorRotationDegrees(const AxisIndicatorProjection& projection)
{
    if (projection.degenerate) return 0.0f;
    constexpr float kRadiansToDegrees = 57.29577951308232f;
    return std::atan2(projection.y, projection.x) * kRadiansToDegrees;
}

// 轴字母标签的位置（相对锚点的像素偏移）。
// 退化轴把标签压到锚点上，避免它绕着一个无意义的方向乱飞。
inline void AxisIndicatorLabelOffset(
    const AxisIndicatorProjection& projection,
    float& offsetX,
    float& offsetY)
{
    if (projection.degenerate)
    {
        offsetX = 0.0f;
        offsetY = 0.0f;
        return;
    }
    const float length = std::sqrt(
        projection.x * projection.x + projection.y * projection.y);
    offsetX = projection.x / length * kAxisIndicatorLabelRadiusPx;
    offsetY = projection.y / length * kAxisIndicatorLabelRadiusPx;
}

// 三个世界轴在屏幕上的落点，顺序固定为 X、Y、Z，供面板与测试共用同一份口径。
inline std::array<AxisIndicatorProjection, 3> ProjectAxisIndicatorTriad(
    const Eigen::Vector3f& right,
    const Eigen::Vector3f& up,
    const Eigen::Vector3f& forward)
{
    return {
        ProjectAxisIndicator(Eigen::Vector3f(1.0f, 0.0f, 0.0f), right, up, forward),
        ProjectAxisIndicator(Eigen::Vector3f(0.0f, 1.0f, 0.0f), right, up, forward),
        ProjectAxisIndicator(Eigen::Vector3f(0.0f, 0.0f, 1.0f), right, up, forward),
    };
}

} // namespace VL::EditorUi
