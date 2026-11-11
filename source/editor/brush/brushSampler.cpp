#include "editor/brush/brushSampler.h"

#include <algorithm>
#include <cmath>

namespace VL
{
namespace Editor
{
namespace
{

constexpr float MinStampSpacingCm = 1.0e-4f;

float ClampUnit(float value)
{
    if (!std::isfinite(value))
        return 0.0f;
    return std::clamp(value, 0.0f, 1.0f);
}

float EvaluateFalloff(const BrushProfile& profile, float t)
{
    switch (profile.falloff)
    {
    case BrushFalloff::Smooth:
        // t=0 为圆心、t=1 为边缘；这里用 1 - smoothstep(t) 让中心权重为 1。
        return 1.0f - t * t * (3.0f - 2.0f * t);
    case BrushFalloff::Linear:
        return 1.0f - t;
    case BrushFalloff::Constant:
        return 1.0f;
    case BrushFalloff::Lut:
        if (profile.falloffLut.size() < 2)
            return 0.0f;
        {
            const float position = t *
                static_cast<float>(profile.falloffLut.size() - 1);
            const size_t index = std::min(
                static_cast<size_t>(position),
                profile.falloffLut.size() - 2);
            const float fraction = position - static_cast<float>(index);
            return profile.falloffLut[index] * (1.0f - fraction) +
                profile.falloffLut[index + 1] * fraction;
        }
    }
    return 0.0f;
}

uint32_t Mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

} // namespace

float EffectiveRadiusCm(const BrushProfile& profile, float pressure)
{
    const float clampedPressure = ClampUnit(pressure);
    const float pressureScale = 1.0f +
        (clampedPressure - 1.0f) * profile.pressureSizeInfluence;
    return profile.radiusCm * pressureScale;
}

float SampleWeight(
    const BrushProfile& profile,
    float normalizedRadius,
    float pressure,
    float noiseSample)
{
    return SampleWeightAt(
        profile,
        Eigen::Vector2f(normalizedRadius, 0.0f),
        pressure,
        noiseSample);
}

float SampleWeightAt(
    const BrushProfile& profile,
    Eigen::Vector2f normalizedOffset,
    float pressure,
    float noiseSample)
{
    if (!normalizedOffset.allFinite())
        return 0.0f;
    if (profile.rotationDeg != 0.0f)
    {
        constexpr float Pi = 3.14159265358979323846f;
        const float radians = -profile.rotationDeg * (Pi / 180.0f);
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        normalizedOffset = Eigen::Vector2f(
            cosine * normalizedOffset.x() - sine * normalizedOffset.y(),
            sine * normalizedOffset.x() + cosine * normalizedOffset.y());
    }
    const float distance = profile.shape == BrushShape::Square
        ? std::max(std::abs(normalizedOffset.x()), std::abs(normalizedOffset.y()))
        : normalizedOffset.norm();
    if (distance >= 1.0f)
        return 0.0f;

    const float falloff = EvaluateFalloff(profile, std::max(0.0f, distance));
    const float clampedPressure = ClampUnit(pressure);
    const float pressureScale = 1.0f +
        (clampedPressure - 1.0f) * profile.pressureStrengthInfluence;
    const float clampedNoiseSample = ClampUnit(noiseSample);
    const float noiseScale = 1.0f - profile.noise +
        profile.noise * clampedNoiseSample;

    const float weight = falloff * profile.strength *
        pressureScale * noiseScale;
    if (!std::isfinite(weight))
        return 0.0f;
    return std::clamp(weight, 0.0f, 1.0f);
}

float StampSpacingCm(const BrushProfile& profile, float pressure)
{
    const float spacing = 2.0f *
        EffectiveRadiusCm(profile, pressure) * profile.spacing;
    return std::max(MinStampSpacingCm, spacing);
}

Eigen::Vector2f JitteredStampCenter(
    const BrushProfile& profile,
    Eigen::Vector2f center,
    uint32_t stampIndex)
{
    const float amplitude =
        profile.radiusCm * ClampUnit(profile.jitter);
    if (amplitude == 0.0f)
        return center;

    const float jitterX =
        StableNoise01(stampIndex, 0u, profile.noiseSeed) * 2.0f - 1.0f;
    const float jitterY =
        StableNoise01(stampIndex, 1u, profile.noiseSeed) * 2.0f - 1.0f;
    return center + Eigen::Vector2f(jitterX, jitterY) * amplitude;
}

float StableNoise01(uint32_t x, uint32_t y, uint32_t seed)
{
    const uint32_t hash = Mix32(
        x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu);
    return static_cast<float>(hash) * (1.0f / 4294967295.0f);
}

} // namespace Editor
} // namespace VL
