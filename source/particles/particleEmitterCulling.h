#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include <Eigen/Dense>

namespace VL
{

struct ParticleEmitterCullingInput
{
    Eigen::Vector3f worldCenter = Eigen::Vector3f::Zero();
    float worldRadius = 0.0f;
    float maxDrawDistance = 0.0f;
    float fadeDistance = 0.0f;
    float minScreenSize = 0.0f;
};

struct ParticleEmitterCullingResult
{
    bool visible = true;
    float opacityScale = 1.0f;
};

// Gribb-Hartmann 平面提取；Vulkan 的裁剪深度是 0..1，因此 near 取 row2，
// far 取 row3 - row2，而不是 OpenGL 的 [-1,1] near/far 对称形式。
inline std::array<Eigen::Vector4f, 6> BuildParticleFrustumPlanes(
    const Eigen::Matrix4f& viewProjection)
{
    const Eigen::Vector4f row0 = viewProjection.row(0).transpose();
    const Eigen::Vector4f row1 = viewProjection.row(1).transpose();
    const Eigen::Vector4f row2 = viewProjection.row(2).transpose();
    const Eigen::Vector4f row3 = viewProjection.row(3).transpose();

    std::array<Eigen::Vector4f, 6> planes = {
        row3 + row0,
        row3 - row0,
        row3 + row1,
        row3 - row1,
        row2,
        row3 - row2};
    for (Eigen::Vector4f& plane : planes)
    {
        const float length = plane.head<3>().norm();
        if (length > 1.0e-8f)
        {
            plane /= length;
        }
    }
    return planes;
}

inline ParticleEmitterCullingResult CullParticleEmitter(
    const ParticleEmitterCullingInput& emitter,
    const std::array<Eigen::Vector4f, 6>& frustumPlanes,
    const Eigen::Vector3f& cameraPosition,
    const Eigen::Vector3f& cameraForward,
    float projectionScaleY,
    float viewportHeightPixels)
{
    const auto nonNegative = [](float value)
    {
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    };

    if (!emitter.worldCenter.allFinite() ||
        !cameraPosition.allFinite())
    {
        return {false, 0.0f};
    }

    const float radius = nonNegative(emitter.worldRadius);
    for (const Eigen::Vector4f& plane : frustumPlanes)
    {
        if (plane.head<3>().squaredNorm() <= 1.0e-16f)
        {
            continue;
        }
        if (plane.head<3>().dot(emitter.worldCenter) + plane.w() <
            -radius)
        {
            return {false, 0.0f};
        }
    }

    const float distance = std::max(
        0.0f,
        (emitter.worldCenter - cameraPosition).norm() - radius);
    const float maxDrawDistance =
        nonNegative(emitter.maxDrawDistance);
    if (maxDrawDistance > 0.0f && distance > maxDrawDistance)
    {
        return {false, 0.0f};
    }

    const float fadeDistance = nonNegative(emitter.fadeDistance);
    float opacityScale = 1.0f;
    if (maxDrawDistance > 0.0f && fadeDistance > 0.0f)
    {
        opacityScale = std::clamp(
            (maxDrawDistance - distance) / fadeDistance,
            0.0f,
            1.0f);
    }

    const float minScreenSize =
        nonNegative(emitter.minScreenSize);
    const float forwardLength = cameraForward.norm();
    if (minScreenSize > 0.0f &&
        std::isfinite(forwardLength) &&
        forwardLength > 1.0e-8f)
    {
        const float depth = (emitter.worldCenter - cameraPosition)
            .dot(cameraForward / forwardLength);
        if (depth > 0.0f)
        {
            const float pixelDiameter =
                radius * nonNegative(projectionScaleY) *
                nonNegative(viewportHeightPixels) / depth;
            if (pixelDiameter < minScreenSize)
            {
                return {false, 0.0f};
            }
        }
    }

    return {true, opacityScale};
}

} // namespace VL
