#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <Eigen/Core>

#include "particles/particleAsset.h"
#include "particles/particleDataChannel.h"

namespace VL
{

// 运行时发射器标量属性绑定。只有通道存在且为标量时覆盖作者值；
// 向量/四元数或缺失通道保持作者默认值，避免类型不匹配静默变成 0。
struct ParticleBoundSpawnAttributes
{
    float spawnRate = 0.0f;
    float spawnPerUnit = 0.0f;
    uint32_t burstCount = 0;
    Eigen::Vector3f gravity = Eigen::Vector3f::Zero();
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    float sizeStart = 0.0f;
    float sizeEnd = 0.0f;
    float opacity = 1.0f;
};

inline ParticleBoundSpawnAttributes BindParticleSpawnAttributes(
    const ParticleEmitterAsset& emitter,
    const ParticleDataChannelMap& channels)
{
    ParticleBoundSpawnAttributes result{
        emitter.spawnRate,
        emitter.spawnPerUnit,
        emitter.burstCount,
        emitter.gravity,
        emitter.wind,
        emitter.sizeStart,
        emitter.sizeEnd,
        emitter.opacity};
    const auto readScalar = [&](const char* parameter, float& value) {
        const auto binding = emitter.attributeBindings.find(parameter);
        if (binding == emitter.attributeBindings.end())
            return;
        float channelValue = 0.0f;
        if (channels.TryGetFloat(binding->second, channelValue))
            value = channelValue;
    };
    readScalar("spawnRate", result.spawnRate);
    readScalar("spawnPerUnit", result.spawnPerUnit);
    result.spawnRate = std::clamp(result.spawnRate, 0.0f, 100000.0f);
    result.spawnPerUnit = std::clamp(
        result.spawnPerUnit, 0.0f, 100000.0f);
    const auto readVector3 = [&](const char* parameter,
                                 Eigen::Vector3f& value) {
        const auto binding = emitter.attributeBindings.find(parameter);
        if (binding == emitter.attributeBindings.end())
            return;
        const auto channel = channels.find(binding->second);
        if (channel == channels.end())
            return;
        const ParticleDataChannelValue& channelValue =
            channel->second;
        if (channelValue.ComponentCount() < 3)
            return;
        value = Eigen::Vector3f(
            channelValue.values[0],
            channelValue.values[1],
            channelValue.values[2]);
    };
    readVector3("gravity", result.gravity);
    readVector3("wind", result.wind);
    readScalar("sizeStart", result.sizeStart);
    readScalar("sizeEnd", result.sizeEnd);
    readScalar("opacity", result.opacity);
    result.sizeStart = std::clamp(result.sizeStart, 0.0f, 1000.0f);
    result.sizeEnd = std::clamp(result.sizeEnd, 0.0f, 1000.0f);
    result.opacity = std::clamp(result.opacity, 0.0f, 1.0f);

    float burstCount = static_cast<float>(result.burstCount);
    readScalar("burstCount", burstCount);
    if (burstCount > 0.0f)
    {
        result.burstCount = std::min(
            emitter.maxParticles,
            static_cast<uint32_t>(std::clamp(
                burstCount,
                0.0f,
                static_cast<float>(emitter.maxParticles))));
    }
    else
    {
        result.burstCount = 0;
    }
    return result;
}

} // namespace VL
