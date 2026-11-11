#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include <Eigen/Dense>

#include "particles/particleLimits.h"

namespace VL
{

// 场景对象级运行时覆盖，不写入 PS_*.json / SC_*.json。
// 发射器掩码只使用低 kMaxParticleEmitters 位；多余位保持 1，兼容隐式发射器。
struct ParticleSystemRuntimeOverrides
{
    float spawnRateScale = 1.0f;
    Eigen::Vector4f colorTint = Eigen::Vector4f::Ones();
    uint32_t enabledEmitterMask = (1u << kMaxParticleEmitters) - 1u;
};

inline bool ValidateParticleSpawnRateScale(float value, std::string& outError)
{
    if (!std::isfinite(value) || value < 0.0f || value > 16.0f)
    {
        outError = "Particle spawnRateScale must be finite and in [0, 16].";
        return false;
    }
    outError.clear();
    return true;
}

inline bool ValidateParticleColorTint(
    const Eigen::Vector4f& value,
    std::string& outError)
{
    if (!value.allFinite() ||
        (value.head<3>().array() < 0.0f).any() ||
        (value.head<3>().array() > 16.0f).any() ||
        value.w() < 0.0f || value.w() > 1.0f)
    {
        outError =
            "Particle colorTint requires finite RGB in [0, 16] and alpha in [0, 1].";
        return false;
    }
    outError.clear();
    return true;
}

inline bool ValidateParticleEmitterIndex(uint32_t index, std::string& outError)
{
    if (index >= kMaxParticleEmitters)
    {
        outError = "Particle emitter index is out of range.";
        return false;
    }
    outError.clear();
    return true;
}

} // namespace VL
