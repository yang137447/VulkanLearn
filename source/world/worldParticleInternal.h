#pragma once

#include <cstddef>
#include <cstdint>

#include "world/world.h"

namespace VL
{
namespace WorldParticleDetail
{

inline constexpr size_t kMaxParticleUserParameters = 64;
inline constexpr size_t kMaxParticleParameterKeyLength = 64;
inline constexpr float kMaxParticleParameterAbsoluteValue = 1.0e6f;
inline constexpr size_t kMaxParticleRuntimeEvents = 256;
inline constexpr uint32_t kMaxParticleTotalBudget = 1000000u;

bool ValidParticleScaleAxis(float value);
bool ValidParticleBudgetScale(float value);
uint32_t ScaledParticleCapacity(uint32_t capacity, float scale);
uint32_t EffectiveParticleMaxTotal(
    const WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget);
float EffectiveParticleBudgetScale(
    const WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget);
void RefreshParticleRuntime(
    WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget);
void InitializeParticleEmitterSlots(WorldParticleSystem& system);

} // namespace WorldParticleDetail
} // namespace VL
