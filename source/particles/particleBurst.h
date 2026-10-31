#pragma once

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

} // namespace VL
