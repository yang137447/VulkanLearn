#pragma once

#include <algorithm>
#include <cmath>

#include "particles/particleAsset.h"

namespace VL
{

// LOD 同时影响连续发射预算与渲染尺寸；burst、prewarm 池填充与寿命
// 继续使用作者参数。池容量永远不随 LOD 改变。
//
// lodTiers 非空时按“已越过的最远档位”选择：档位距离严格递增，扫描到
// 第一个 distance < tier.distance 为止，使用最后一个满足条件的
// spawnRateScale/renderScale。fadeRange>0 时在进入下一档前用
// smoothstep 在上一档和下一档之间插值；fadeRange=0 保持硬切。
// 没有越过任何档位时保持 1。旧 lodDistance/
// lodSpawnRateScale 只在没有 tiers 时作为单档兼容逻辑生效。
struct ParticleLodState
{
    float spawnRateScale = 1.0f;
    float renderScale = 1.0f;
    // 0 = 上一档，1 = 当前/下一档；仅用于 GPU 诊断/未来 alpha
    // 交叉淡入，当前不参与 alpha，避免与距离 fade 重复。
    float blend = 1.0f;
};

inline ParticleLodState EffectiveParticleLod(
    const ParticleEmitterAsset& emitter,
    float distance)
{
    ParticleLodState state;
    if (!std::isfinite(distance))
    {
        return state;
    }
    if (!emitter.lodTiers.empty())
    {
        int current = -1;
        for (size_t index = 0;
             index < emitter.lodTiers.size(); ++index)
        {
            if (emitter.lodTiers[index].distance <= distance)
            {
                current = static_cast<int>(index);
            }
            else
            {
                break;
            }
        }
        const auto previousSpawn = [&]()
        {
            return current >= 0
                ? emitter.lodTiers[static_cast<size_t>(current)]
                    .spawnRateScale
                : 1.0f;
        };
        const auto previousRender = [&]()
        {
            return current >= 0
                ? emitter.lodTiers[static_cast<size_t>(current)]
                    .renderScale
                : 1.0f;
        };
        state.spawnRateScale = previousSpawn();
        state.renderScale = previousRender();

        const size_t next = static_cast<size_t>(current + 1);
        if (next < emitter.lodTiers.size())
        {
            const ParticleLodTier& tier =
                emitter.lodTiers[next];
            const float fadeRange = std::max(tier.fadeRange, 0.0f);
            // 过渡不能早于上一档边界，避免 fadeRange 大于档距时
            // 同时插值两个档位。
            const float previousBoundary = current >= 0
                ? emitter.lodTiers[static_cast<size_t>(current)].distance
                : 0.0f;
            const float start = std::max(
                std::max(tier.distance - fadeRange, previousBoundary),
                0.0f);
            const float span = tier.distance - start;
            if (fadeRange > 0.0f && span > 0.0f &&
                distance >= start && distance < tier.distance)
            {
                const float t = std::clamp(
                    (distance - start) / span, 0.0f, 1.0f);
                const float smooth =
                    t * t * (3.0f - 2.0f * t);
                state.spawnRateScale =
                    previousSpawn() +
                    (tier.spawnRateScale - previousSpawn()) * smooth;
                state.renderScale =
                    previousRender() +
                    (tier.renderScale - previousRender()) * smooth;
                state.blend = smooth;
            }
        }
        return state;
    }
    if (emitter.lodDistance > 0.0f &&
        distance > emitter.lodDistance)
    {
        state.spawnRateScale = emitter.lodSpawnRateScale;
    }
    return state;
}

inline float EffectiveParticleSpawnRate(
    const ParticleEmitterAsset& emitter,
    float distance)
{
    return emitter.spawnRate *
        EffectiveParticleLod(emitter, distance).spawnRateScale;
}

} // namespace VL
