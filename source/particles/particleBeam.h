#pragma once

#include <algorithm>
#include <cstdint>

namespace VL
{

// beam 是渲染器模式，不参与粒子历史点插值：每条弧由作者化的起点/终点
// 生成固定段数，段内再做带包络的噪声扰动。
inline constexpr uint32_t kMinParticleBeamSegments = 2;
inline constexpr uint32_t kMaxParticleBeamSegments = 64;
inline constexpr uint32_t kMinParticleBeamCount = 1;
inline constexpr uint32_t kMaxParticleBeamCount = 16;
inline constexpr uint32_t kParticleBeamVerticesPerSegment = 6;

inline uint32_t ClampParticleBeamSegments(uint32_t segments)
{
    return std::clamp<uint32_t>(
        segments,
        kMinParticleBeamSegments,
        kMaxParticleBeamSegments);
}

inline uint32_t ParticleBeamVerticesPerParticle(uint32_t segments)
{
    return ClampParticleBeamSegments(segments) *
        kParticleBeamVerticesPerSegment;
}

inline uint64_t ParticleBeamVertexCount(
    uint32_t segments,
    uint32_t count)
{
    return static_cast<uint64_t>(
        ParticleBeamVerticesPerParticle(segments)) *
        count;
}

} // namespace VL
