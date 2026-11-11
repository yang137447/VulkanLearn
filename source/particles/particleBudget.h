#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace VL
{

// 预算后的池容量：authored 是资产里的 maxParticles，budgeted 是 World
// 按总预算/缩放逐发射器分配的值。telemetry 不可用时退回作者容量，
// 避免旧快照把所有发射器误判为 0。
inline uint32_t ResolveParticleEmitterCapacity(
    uint32_t authoredMaxParticles,
    uint32_t budgetedMaxParticles,
    bool budgetTelemetryAvailable) noexcept
{
    if (!budgetTelemetryAvailable)
    {
        return authoredMaxParticles;
    }
    return std::min(authoredMaxParticles, budgetedMaxParticles);
}

// 0 容量表示本帧不分配 GPU 池；已有池必须走正常 retire 路径。
inline bool ShouldAllocateParticlePool(
    uint32_t capacity) noexcept
{
    return capacity > 0u;
}

// telemetry 不可用时的本地预算镜像：按 World 的“作者顺序消费总上限 +
// 缩放后向下取整”规则分配，保证 snapshot.budget 的总上限仍被尊重。
inline std::vector<uint32_t> ComputeParticleEmitterBudgetCapacities(
    const std::vector<uint32_t>& authoredMaxParticles,
    uint32_t maxTotalParticles,
    float emitterBudgetScale)
{
    const double scale =
        std::isfinite(emitterBudgetScale)
        ? static_cast<double>(std::max(emitterBudgetScale, 0.0f))
        : 0.0;
    std::vector<uint32_t> capacities;
    capacities.reserve(authoredMaxParticles.size());
    uint64_t consumed = 0;
    for (const uint32_t authored : authoredMaxParticles)
    {
        const double scaledValue =
            static_cast<double>(authored) * scale;
        const uint32_t scaled = scaledValue <= 0.0
            ? 0u
            : (scaledValue >= static_cast<double>(
                    std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(scaledValue));
        const uint64_t remaining = maxTotalParticles == 0u
            ? std::numeric_limits<uint32_t>::max()
            : (consumed >= maxTotalParticles
                ? 0u
                : static_cast<uint64_t>(maxTotalParticles) - consumed);
        const uint32_t budgeted = static_cast<uint32_t>(
            std::min<uint64_t>(scaled, remaining));
        consumed += budgeted;
        capacities.push_back(budgeted);
    }
    return capacities;
}

} // namespace VL
