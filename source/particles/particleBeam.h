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
inline constexpr uint32_t kMaxParticleBeamBranches = 4;
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

// 每条作者并行弧额外携带 branchCount 条分支；分支与主弧共用所属弧的
// 端点和 arc index，但使用独立噪声 seed；总绘制实例数 =
// count * (1 + branchCount)。
inline uint64_t ParticleBeamInstanceCount(
    uint32_t count,
    uint32_t branchCount = 0)
{
    return static_cast<uint64_t>(count) *
        (1u + std::min<uint32_t>(
            branchCount,
            kMaxParticleBeamBranches));
}

inline uint64_t ParticleBeamVertexCount(
    uint32_t segments,
    uint32_t count,
    uint32_t branchCount = 0)
{
    return static_cast<uint64_t>(
        ParticleBeamVerticesPerParticle(segments)) *
        ParticleBeamInstanceCount(count, branchCount);
}

} // namespace VL
