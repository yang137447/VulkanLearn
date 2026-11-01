#pragma once

#include <algorithm>
#include <vector>

#include "particles/particleAsset.h"

namespace VL
{

// 以归一化年龄 lifeT = age / lifetime 采样作者曲线。空曲线恒为 1，
// 首尾外 clamp，段内线性插值；重复 time 取稳定顺序中最后一个
// <= lifeT 的 key，再与下一个 key 插值，保证结果确定。
inline float EvaluateParticleLifetimeScale(
    const std::vector<ParticleLifetimeScaleKey>& keys,
    float normalizedAge)
{
    if (keys.empty()) return 1.0f;

    // NaN 也落到 0，避免作者异常值把倍率传播进模拟。
    float lifeT = normalizedAge;
    if (!(lifeT > 0.0f)) lifeT = 0.0f;
    if (lifeT > 1.0f) lifeT = 1.0f;

    size_t index = 0;
    for (size_t current = 0; current < keys.size(); ++current)
    {
        if (keys[current].time <= lifeT) index = current;
        else break;
    }

    if (index + 1 >= keys.size())
    {
        return std::clamp(keys[index].scale, 0.0f, 4.0f);
    }
    const ParticleLifetimeScaleKey& first = keys[index];
    const ParticleLifetimeScaleKey& second = keys[index + 1];
    if (lifeT <= first.time)
    {
        return std::clamp(first.scale, 0.0f, 4.0f);
    }
    const float span = second.time - first.time;
    if (!(span > 0.0f))
    {
        return std::clamp(second.scale, 0.0f, 4.0f);
    }
    const float weight =
        std::clamp((lifeT - first.time) / span, 0.0f, 1.0f);
    return std::clamp(
        first.scale + (second.scale - first.scale) * weight,
        0.0f,
        4.0f);
}

} // namespace VL
