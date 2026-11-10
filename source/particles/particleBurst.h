#pragma once

#include <algorithm>
#include <cmath>

namespace VL
{

// burst 重触发判定：只有 interval > 0 的 burst 发射器会在跨周期时整池重生。
// 逐帧 floor 比较与 particleSimulate.comp 使用同一公式；时间不前进
// 或 interval <= 0 时永远返回 false，保持一次性 burst 语义。
inline bool ParticleBurstCycleCrossed(
    float previousTime,
    float time,
    float interval)
{
    if (!std::isfinite(interval) || interval <= 0.0f ||
        !std::isfinite(previousTime) || !std::isfinite(time) ||
        time <= previousTime)
    {
        return false;
    }
    // 用 double 做除法，避免长时间运行时 float 在周期边界丢精度。
    const double previousCycle =
        std::floor(static_cast<double>(previousTime) / interval);
    const double currentCycle =
        std::floor(static_cast<double>(time) / interval);
    return currentCycle > previousCycle;
}

// 单次 burst 的有效数量：0 保持旧资产的整池语义。
inline uint32_t EffectiveParticleBurstCount(
    uint32_t maxParticles,
    uint32_t burstCount)
{
    return burstCount == 0
        ? maxParticles
        : std::min(maxParticles, burstCount);
}

// burstDelay/burstPhase 与 shader 的 burstTiming 使用同一偏移规则：
// 第一次触发发生在 localTime 跨过 0，之后按 interval 的 floor 跨周期。
inline bool ParticleBurstDue(
    float previousTime,
    float time,
    float interval,
    float delay,
    float phase)
{
    if (!std::isfinite(interval) || interval <= 0.0f ||
        !std::isfinite(previousTime) || !std::isfinite(time) ||
        !std::isfinite(delay) || !std::isfinite(phase) ||
        time <= previousTime)
    {
        return false;
    }
    const double offset =
        static_cast<double>(delay) + static_cast<double>(phase);
    const double previous = static_cast<double>(previousTime) - offset;
    const double current = static_cast<double>(time) - offset;
    if (previous < 0.0 && current >= 0.0)
        return true;
    if (previous < 0.0 || current < 0.0)
        return false;
    return std::floor(current / interval) >
        std::floor(previous / interval);
}

// 位移发射在 GPU/CPU 路径统一折算成等效 spawnRate。distance 单位为 cm，
// 结果限制在作者 spawnRate 的同一上限内。
inline float ParticleSpawnPerUnitRate(
    float spawnPerUnit,
    float distance,
    float deltaSeconds)
{
    if (!std::isfinite(spawnPerUnit) || spawnPerUnit <= 0.0f ||
        !std::isfinite(distance) || distance <= 0.0f ||
        !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
    {
        return 0.0f;
    }
    return std::min(
        distance * spawnPerUnit / deltaSeconds,
        100000.0f);
}

} // namespace VL
