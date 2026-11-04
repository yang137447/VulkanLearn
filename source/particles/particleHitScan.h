#pragma once

#include <cmath>
#include <optional>

#include <Eigen/Dense>

namespace VL
{

struct ParticlePlaneHit
{
    float distance = 0.0f;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
};

// Beam HitScan 的确定性命中测试：从 lineStart 指向 lineEnd 的有限线段
// 与无限平面相交。maxDistance 是沿归一化方向的世界距离；返回 false 时
// 调用方保留作者化终点。
inline std::optional<ParticlePlaneHit> IntersectParticlePlane(
    const Eigen::Vector3f& lineStart,
    const Eigen::Vector3f& lineEnd,
    const Eigen::Vector3f& planeNormal,
    float planeOffset,
    float maxDistance)
{
    if (!lineStart.allFinite() || !lineEnd.allFinite() ||
        !planeNormal.allFinite() || !std::isfinite(planeOffset) ||
        !std::isfinite(maxDistance) || maxDistance <= 0.0f)
    {
        return std::nullopt;
    }
    const float normalLengthSq = planeNormal.squaredNorm();
    if (normalLengthSq <= 1.0e-12f)
    {
        return std::nullopt;
    }
    const Eigen::Vector3f normal = planeNormal / std::sqrt(normalLengthSq);
    const Eigen::Vector3f direction = lineEnd - lineStart;
    const float directionLength = direction.norm();
    if (directionLength <= 1.0e-6f)
    {
        return std::nullopt;
    }
    const Eigen::Vector3f rayDirection = direction / directionLength;
    const float denominator = normal.dot(rayDirection);
    if (std::abs(denominator) <= 1.0e-6f)
    {
        return std::nullopt;
    }
    const float distance =
        (planeOffset - normal.dot(lineStart)) / denominator;
    if (distance < 1.0e-4f || distance > maxDistance)
    {
        return std::nullopt;
    }
    ParticlePlaneHit hit;
    hit.distance = distance;
    hit.position = lineStart + rayDirection * distance;
    // 命中法线朝向射线来源，供后续 decal/火花定向使用。
    hit.normal = denominator > 0.0f ? -normal : normal;
    return hit;
}

} // namespace VL
