#pragma once

#include <algorithm>
#include <cmath>

#include <Eigen/Dense>

namespace VL
{

// CPU 镜像：与 particleSoftDepth.glsl / particle.frag 的线性视深度、
// thickness fade 上限保持同一公式，用于契约测试。
inline float ParticleViewDepthFromDeviceDepthCpu(
    float deviceDepth,
    const Eigen::Matrix4f& invProjection)
{
    const Eigen::Vector4f view =
        invProjection * Eigen::Vector4f(0.0f, 0.0f, deviceDepth, 1.0f);
    return -view.z() / std::max(view.w(), 1.0e-6f);
}

inline float ParticleSoftFadeCpu(
    float depthDelta,
    float fadeDistance,
    float thicknessCm)
{
    float fade = std::clamp(
        depthDelta / std::max(fadeDistance, 0.001f),
        0.0f,
        1.0f);
    if (thicknessCm > 0.0f)
    {
        fade = std::min(
            fade,
            std::clamp(
                depthDelta / std::max(thicknessCm, 0.001f),
                0.0f,
                1.0f));
    }
    return fade;
}

} // namespace VL
