#pragma once

#include <cmath>

namespace VL
{
// Point-light pulse approximation. The renderer has no particle readback, so
// effects such as muzzle flashes drive a periodic light curve from CPU time
// instead of sampling particle birth/death events.
//
// `phase` shifts the pulse within its period. The returned factor is 1 at the
// start of the pulse, falls as (1 - t/duration)^2, and reaches 0 at the pulse
// boundary. Time and phase are seconds; the curve is periodic every `period`.
inline float EvaluateLightPulse(
    float timeSeconds,
    float period,
    float duration,
    float phase)
{
    if (!std::isfinite(timeSeconds) || !std::isfinite(period) ||
        !std::isfinite(duration) || !std::isfinite(phase))
        return 0.0f;
    if (period <= 0.0f || duration <= 0.0f)
        return 0.0f;

    // 在 double 中求和，避免长时间运行后 time + phase 溢出到 inf。
    const double shifted =
        static_cast<double>(timeSeconds) + static_cast<double>(phase);
    double local = std::fmod(shifted, static_cast<double>(period));
    if (local < 0.0)
        local += static_cast<double>(period);

    const double pulseDuration = static_cast<double>(
        duration < period ? duration : period);
    if (local >= pulseDuration)
        return 0.0f;

    const float normalized = static_cast<float>(local / pulseDuration);
    const float fade = 1.0f - normalized;
    return fade * fade;
}
} // namespace VL
