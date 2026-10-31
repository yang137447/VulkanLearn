#pragma once

#include <cmath>

#include "particles/particleAsset.h"

namespace VL
{

// LOD 只缩放连续发射预算；burst、prewarm 池填充、寿命、尺寸和渲染
// 都继续使用作者参数。
//
// lodTiers 非空时按“已越过的最远档位”选择：档位距离严格递增，扫描到
// 第一个 distance < tier.distance 为止，使用最后一个满足条件的
// spawnRateScale。没有越过任何档位时保持 1。旧 lodDistance/
// lodSpawnRateScale 只在没有 tiers 时作为单档兼容逻辑生效。
inline float EffectiveParticleSpawnRate(
    const ParticleEmitterAsset& emitter,
    float distance)
{
    if (!std::isfinite(distance))
    {
        return emitter.spawnRate;
    }
    if (!emitter.lodTiers.empty())
    {
        float scale = 1.0f;
        for (const ParticleLodTier& tier : emitter.lodTiers)
        {
            if (tier.distance <= distance)
            {
                scale = tier.spawnRateScale;
            }
            else
            {
                break;
            }
        }
        return emitter.spawnRate * scale;
    }
    if (emitter.lodDistance <= 0.0f ||
        distance <= emitter.lodDistance)
    {
        return emitter.spawnRate;
    }
    return emitter.spawnRate * emitter.lodSpawnRateScale;
}

} // namespace VL
