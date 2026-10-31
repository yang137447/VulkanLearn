#pragma once

#include <algorithm>
#include <cstdint>

namespace VL
{

// ribbon 每个粒子最多消费 3 段：current->history0、history0->history1、
// history1->history2。段数少于 3 时尾部历史点不参与绘制。
inline constexpr uint32_t kMaxParticleRibbonSegments = 3;

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

} // namespace VL
