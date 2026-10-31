#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

namespace VL
{

struct ParticleEmitterDepthKey
{
    // 稳定身份，通常是 sceneName|assetPath|emitterName。
    std::string key;
    Eigen::Vector3f worldCenter = Eigen::Vector3f::Zero();
    // 近似包围球半径（cm），让大体积发射器在透明排序中更靠前。
    float worldRadius = 0.0f;
};

// 返回按“远 -> 近”排序的发射器索引。深度沿相机前向计算并外扩包围球半径；
// 完全同深时按稳定 key 字典序升序，避免 unordered_map 迭代顺序影响结果。
inline std::vector<size_t> ComputeParticleEmitterDrawOrder(
    const std::vector<ParticleEmitterDepthKey>& emitters,
    const Eigen::Vector3f& cameraPosition,
    const Eigen::Vector3f& cameraForward)
{
    const float forwardLengthSquared = cameraForward.squaredNorm();
    Eigen::Vector3f forward = Eigen::Vector3f::Zero();
    if (forwardLengthSquared > 0.0f)
    {
        forward = cameraForward / std::sqrt(forwardLengthSquared);
    }

    std::vector<size_t> order(emitters.size());
    for (size_t index = 0; index < order.size(); ++index)
    {
        order[index] = index;
    }

    std::stable_sort(
        order.begin(),
        order.end(),
        [&](size_t leftIndex, size_t rightIndex)
        {
            const ParticleEmitterDepthKey& left = emitters[leftIndex];
            const ParticleEmitterDepthKey& right = emitters[rightIndex];
            const float leftDepth =
                (left.worldCenter - cameraPosition).dot(forward) +
                left.worldRadius;
            const float rightDepth =
                (right.worldCenter - cameraPosition).dot(forward) +
                right.worldRadius;
            if (leftDepth != rightDepth)
            {
                return leftDepth > rightDepth;
            }
            return left.key < right.key;
        });
    return order;
}

} // namespace VL
