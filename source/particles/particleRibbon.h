#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "particles/particleAsset.h"

namespace VL
{

// ribbon 保存 16 个历史采样点（不含当前位置），顶点阶段先按弧长
// 重采样到 1..32 段。固定历史深度避免把模拟帧率直接暴露给条带形状。
inline constexpr uint32_t kParticleRibbonHistorySamples = 16;
inline constexpr uint32_t kMaxParticleRibbonSegments = 32;

inline uint32_t ClampParticleRibbonSegments(uint32_t segments)
{
    return std::clamp<uint32_t>(
        segments, 1u, kMaxParticleRibbonSegments);
}

inline uint32_t ParticleRibbonVerticesPerParticle(uint32_t segments)
{
    // 每段一个 quad（6 顶点），与 billboard 的 kParticleCorners 顺序一致。
    return ClampParticleRibbonSegments(segments) * 6u;
}

inline uint64_t ParticleRibbonVertexCount(
    uint32_t maxParticles,
    uint32_t segments)
{
    return static_cast<uint64_t>(maxParticles) *
        ParticleRibbonVerticesPerParticle(segments);
}

// 顶点在单个粒子内的局部段号；调用方保证 vertexCount 已按段数放大。
inline uint32_t ParticleRibbonSegmentForLocalVertex(
    uint32_t localVertex,
    uint32_t segments)
{
    return std::min(
        localVertex / 6u,
        ClampParticleRibbonSegments(segments) - 1u);
}

// 沿条带从当前端(0)到尾部(1)的归一化位置，供尾端 alpha 衰减使用。
inline float ParticleRibbonTailParameter(
    uint32_t segment,
    float alongSegment,
    uint32_t segments)
{
    const uint32_t count = ClampParticleRibbonSegments(segments);
    const float clampedSegment =
        static_cast<float>(std::min(segment, count - 1u));
    const float t =
        (clampedSegment + std::clamp(alongSegment, 0.0f, 1.0f)) /
        static_cast<float>(count);
    return std::clamp(t, 0.0f, 1.0f);
}

// 与 ribbon shader 的宽度曲线采样保持一致：空表恒为 1，首尾外 clamp，
// 段内按 time 线性插值。
inline float EvaluateParticleRibbonWidth(
    const std::vector<ParticleRibbonWidthKey>& keys,
    float t,
    ParticleCurveInterpolation interpolation =
        ParticleCurveInterpolation::Linear)
{
    if (keys.empty())
    {
        return 1.0f;
    }
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    if (clamped <= keys.front().time)
    {
        return keys.front().width;
    }
    if (clamped >= keys.back().time)
    {
        return keys.back().width;
    }
    const float tangentScale =
        ParticleCurveTangentScale(interpolation);
    for (size_t index = 0; index + 1 < keys.size(); ++index)
    {
        const float t0 = keys[index].time;
        const float t1 = keys[index + 1].time;
        if (clamped >= t0 && clamped <= t1)
        {
            const float span = t1 - t0;
            const float weight = span > 1.0e-6f
                ? (clamped - t0) / span
                : 0.0f;
            if (interpolation != ParticleCurveInterpolation::Linear)
            {
                return ParticleHermiteScalar(
                    keys[index].width,
                    keys[index + 1].width,
                    keys[index].tangentOut * tangentScale,
                    keys[index + 1].tangentIn * tangentScale,
                    weight,
                    span);
            }
            return keys[index].width +
                (keys[index + 1].width - keys[index].width) * weight;
        }
    }
    return keys.back().width;
}

inline Eigen::Vector4f EvaluateParticleRibbonColor(
    const std::vector<ParticleRibbonColorKey>& keys,
    float t,
    const Eigen::Vector4f& fallback,
    ParticleCurveInterpolation interpolation =
        ParticleCurveInterpolation::Linear)
{
    if (keys.empty())
    {
        return fallback;
    }
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    if (clamped <= keys.front().time)
    {
        return keys.front().color;
    }
    if (clamped >= keys.back().time)
    {
        return keys.back().color;
    }
    const float tangentScale =
        ParticleCurveTangentScale(interpolation);
    for (size_t index = 0; index + 1 < keys.size(); ++index)
    {
        const float t0 = keys[index].time;
        const float t1 = keys[index + 1].time;
        if (clamped < t0 || clamped > t1)
        {
            continue;
        }
        const float span = t1 - t0;
        if (!(span > 1.0e-6f))
        {
            return keys[index + 1].color;
        }
        const float weight = (clamped - t0) / span;
        if (interpolation != ParticleCurveInterpolation::Linear)
        {
            return ParticleHermiteVector4(
                keys[index].color,
                keys[index + 1].color,
                keys[index].tangentOut * tangentScale,
                keys[index + 1].tangentIn * tangentScale,
                weight,
                span);
        }
        return keys[index].color +
            (keys[index + 1].color - keys[index].color) * weight;
    }
    return keys.back().color;
}

} // namespace VL
