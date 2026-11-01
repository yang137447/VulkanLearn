#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VL
{

// 固定步长调度：accumulator 保存不足一步的余量，stepIndex 是确定性
// 随机使用的离散时间。返回值 stepSeconds=0、steps=0 表示本帧没有
// 完整步（粒子冻结，但仍需跑计数/排序/绘制）。
struct ParticleFixedStepState
{
    float accumulator = 0.0f;
    uint32_t stepIndex = 0;
};

struct ParticleFixedStepTick
{
    float stepSeconds = 0.0f;
    uint32_t steps = 0;
};

inline ParticleFixedStepTick AdvanceParticleFixedStep(
    float deltaSeconds,
    float fixedStep,
    uint32_t maxSteps,
    ParticleFixedStepState& state) noexcept
{
    if (!(fixedStep > 0.0f))
    {
        return ParticleFixedStepTick{
            std::max(deltaSeconds, 0.0f), 1u};
    }
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
    {
        return ParticleFixedStepTick{0.0f, 0u};
    }
    state.accumulator += deltaSeconds;
    const uint32_t available = static_cast<uint32_t>(
        std::floor(state.accumulator / fixedStep));
    const uint32_t steps = std::min(available, maxSteps);
    if (steps == 0u)
    {
        return ParticleFixedStepTick{0.0f, 0u};
    }
    state.accumulator -=
        fixedStep * static_cast<float>(steps);
    state.stepIndex += steps;
    return ParticleFixedStepTick{fixedStep, steps};
}

} // namespace VL
