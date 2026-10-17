#pragma once

#include <Eigen/Dense>
#include <stdexcept>
#include <cmath>

namespace VL
{
// 视口左上原点归一化坐标反投影到厘米世界；首版只放到 y=0 平面。
// 输入矩阵沿用 Camera 的 Vulkan NDC（z=0..1、y 向下），不重复翻转 Y。
inline Eigen::Vector3f ScriptBlueprintPlacement(const Eigen::Matrix4f& viewProjection, float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x >= 1 || y < 0 || y >= 1)
        throw std::invalid_argument("Blueprint drop must be inside the scene viewport");
    Eigen::FullPivLU<Eigen::Matrix4f> matrix(viewProjection);
    if (!matrix.isInvertible()) throw std::invalid_argument("Blueprint drop camera is not invertible");
    const Eigen::Matrix4f inverse = matrix.inverse();
    const Eigen::Vector4f nearPoint = inverse * Eigen::Vector4f(2*x-1, 2*y-1, 0, 1);
    const Eigen::Vector4f farPoint = inverse * Eigen::Vector4f(2*x-1, 2*y-1, 1, 1);
    if (std::abs(nearPoint.w()) < 1e-8f || std::abs(farPoint.w()) < 1e-8f)
        throw std::invalid_argument("Blueprint drop ray is invalid");
    const Eigen::Vector3f origin = nearPoint.head<3>() / nearPoint.w();
    const Eigen::Vector3f direction = farPoint.head<3>() / farPoint.w() - origin;
    if (std::abs(direction.y()) < 1e-6f)
        throw std::invalid_argument("Blueprint drop ray is parallel to the ground plane");
    const float distance = -origin.y() / direction.y();
    if (distance < 0) throw std::invalid_argument("Blueprint drop ground plane is behind the camera");
    const Eigen::Vector3f position = origin + distance * direction;
    if (!position.allFinite()) throw std::invalid_argument("Blueprint drop position is not finite");
    return position;
}
}
