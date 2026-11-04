#include "physics/fracture/glassDamagePolicy.h"

#include <algorithm>
#include <cmath>

namespace VL::Physics::Fracture
{
namespace
{

float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

int LerpCount(int first, int second, float t)
{
    return first + static_cast<int>(std::lround(
        static_cast<float>(second - first) * t));
}

} // namespace

GlassDamagePolicy::GlassDamagePolicy(const GlassDamageSettings& input)
    : settings(input)
{
    // 纯策略也要保证除零/反向区间不可能出现；调用方传错参数时退化为
    // 单个有效阈值，而不是在帧循环里产生 NaN。
    settings.crackOnlySpeed = std::max(0.001f, settings.crackOnlySpeed);
    settings.breakEnergy = std::max(1e-4f, settings.breakEnergy);
    settings.energyPerHit = Clamp01(settings.energyPerHit);
    settings.minRadialCracks = std::max(3, settings.minRadialCracks);
    settings.maxRadialCracks =
        std::max(settings.minRadialCracks, settings.maxRadialCracks);
    settings.minRingCount = std::max(1, settings.minRingCount);
    settings.maxRingCount =
        std::max(settings.minRingCount, settings.maxRingCount);
    settings.minCrackRadiusScale =
        std::clamp(settings.minCrackRadiusScale, 0.05f, 1.0f);
    settings.maxCrackRadiusScale = std::clamp(
        settings.maxCrackRadiusScale,
        settings.minCrackRadiusScale,
        1.0f);
}

GlassHitDecision GlassDamagePolicy::RegisterImpact(float relativeSpeed)
{
    GlassHitDecision decision;
    decision.accumulatedEnergy = accumulatedEnergy;
    decision.normalizedEnergy = Clamp01(accumulatedEnergy / settings.breakEnergy);

    if (broken)
    {
        decision.outcome = GlassHitOutcome::Fracture;
        decision.normalizedEnergy = 1.0f;
        decision.crackRadiusScale = settings.maxCrackRadiusScale;
        decision.radialCracks = settings.maxRadialCracks;
        decision.ringCount = settings.maxRingCount;
        return decision;
    }

    if (std::isfinite(relativeSpeed) && relativeSpeed >= settings.crackOnlySpeed)
    {
        broken = true;
        accumulatedEnergy = settings.breakEnergy;
        decision.outcome = GlassHitOutcome::Fracture;
        decision.addedEnergy = settings.breakEnergy;
        decision.accumulatedEnergy = accumulatedEnergy;
        decision.normalizedEnergy = 1.0f;
        decision.crackRadiusScale = settings.maxCrackRadiusScale;
        decision.radialCracks = settings.maxRadialCracks;
        decision.ringCount = settings.maxRingCount;
        return decision;
    }

    float energy = 0.0f;
    if (std::isfinite(relativeSpeed) && relativeSpeed > 0.0f)
    {
        const float ratio = relativeSpeed / settings.crackOnlySpeed;
        energy = settings.energyPerHit * ratio * ratio;
    }
    decision.addedEnergy = energy;
    accumulatedEnergy = std::min(settings.breakEnergy, accumulatedEnergy + energy);
    decision.accumulatedEnergy = accumulatedEnergy;
    decision.normalizedEnergy = Clamp01(accumulatedEnergy / settings.breakEnergy);

    if (accumulatedEnergy >= settings.breakEnergy)
    {
        broken = true;
        decision.outcome = GlassHitOutcome::Fracture;
    }

    decision.crackRadiusScale = settings.minCrackRadiusScale +
        (settings.maxCrackRadiusScale - settings.minCrackRadiusScale) *
        decision.normalizedEnergy;
    decision.radialCracks = LerpCount(
        settings.minRadialCracks, settings.maxRadialCracks,
        decision.normalizedEnergy);
    decision.ringCount = LerpCount(
        settings.minRingCount, settings.maxRingCount,
        decision.normalizedEnergy);
    return decision;
}

void GlassDamagePolicy::Reset()
{
    accumulatedEnergy = 0.0f;
    broken = false;
}

} // namespace VL::Physics::Fracture
