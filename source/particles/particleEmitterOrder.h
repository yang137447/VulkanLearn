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
    // 作者排序层：数值小的先画（更远/背景），数值大的后画（更靠近镜头）。
    // 它优先于视深度，用于表达“烟永远在碎块后面”这类无法靠几何深度
    // 稳定表达的美术意图；默认 0 时行为与旧的纯深度排序完全一致。
    int sortOrder = 0;
};

// 全局 alpha 池在 drawOrder 里没有真实 emitter 身份；用固定哨兵 key
// 参与排序，保证同 sortOrder/同深度时结果仍然是确定的。
inline constexpr const char* kParticleGlobalDrawKey = "__global__";

inline Eigen::Vector3f NormalizeParticleSortCameraForward(
    const Eigen::Vector3f& cameraForward)
{
    const float forwardLengthSquared = cameraForward.squaredNorm();
    if (forwardLengthSquared <= 0.0f)
    {
        return Eigen::Vector3f::Zero();
    }
    return cameraForward / std::sqrt(forwardLengthSquared);
}

// 视深度沿相机前向计算并外扩包围球半径；值越大表示越远。
inline float ParticleEmitterViewDepth(
    const ParticleEmitterDepthKey& emitter,
    const Eigen::Vector3f& cameraPosition,
    const Eigen::Vector3f& normalizedCameraForward)
{
    return (emitter.worldCenter - cameraPosition).dot(
               normalizedCameraForward) +
        emitter.worldRadius;
}

// 返回按“远 -> 近”排序的发射器索引。深度沿相机前向计算并外扩包围球半径；
// 排序键顺序为：作者 sortOrder 升序（主序） -> 视深度降序（远先画）
// -> 稳定 key 字典序。sortOrder 必须是主序，否则深度排序会把它重新打散，
// 作者无法表达跨深度层的固定前后关系。
inline std::vector<size_t> ComputeParticleEmitterDrawOrder(
    const std::vector<ParticleEmitterDepthKey>& emitters,
    const Eigen::Vector3f& cameraPosition,
    const Eigen::Vector3f& cameraForward)
{
    const Eigen::Vector3f forward =
        NormalizeParticleSortCameraForward(cameraForward);

    std::vector<size_t> order(emitters.size());
    for (size_t index = 0; index < order.size(); ++index)
    {
        order[index] = index;
    }

    // 深度只依赖相机与稳定身份，先算一次，避免比较器里重复 sqrt。
    std::vector<float> depths(emitters.size(), 0.0f);
    for (size_t index = 0; index < emitters.size(); ++index)
    {
        depths[index] = ParticleEmitterViewDepth(
            emitters[index], cameraPosition, forward);
    }

    std::stable_sort(
        order.begin(),
        order.end(),
        [&](size_t leftIndex, size_t rightIndex)
        {
            const ParticleEmitterDepthKey& left = emitters[leftIndex];
            const ParticleEmitterDepthKey& right = emitters[rightIndex];
            if (left.sortOrder != right.sortOrder)
            {
                return left.sortOrder < right.sortOrder;
            }
            const float leftDepth = depths[leftIndex];
            const float rightDepth = depths[rightIndex];
            if (leftDepth != rightDepth)
            {
                return leftDepth > rightDepth;
            }
            return left.key < right.key;
        });
    return order;
}

// 全局池把多个 alpha billboard 合并成一次 draw，只能携带一个排序位置。
// 规则：
// - sortOrder 取池内最小值：显式排序是主序，整池按最靠背景的成员插入，
//   不会因为某个成员后来被作者调到背景层而被其它 emitter 拆开；
// - 深度取池内最大视深（代码约定里值越大越远，即池的“背景边缘”）。
//   若取最近成员，整池会被排到所有更远物体之后，重新出现“烟总是盖住
//   碎块/弹壳”的旧问题；取背景边缘则让中近景碎片有机会盖在烟上。
//   这是单次合并 draw 的近似边界：池内部仍按逐粒子视深排序，但整池
//   与其它 emitter 的先后只有一个位置。
// 空成员返回 false，调用方不得插入全局 item。
inline bool TryMakeParticleGlobalSortDrawKey(
    const std::vector<ParticleEmitterDepthKey>& members,
    const Eigen::Vector3f& cameraPosition,
    const Eigen::Vector3f& cameraForward,
    ParticleEmitterDepthKey& outKey)
{
    if (members.empty())
    {
        return false;
    }
    const Eigen::Vector3f forward =
        NormalizeParticleSortCameraForward(cameraForward);
    int minimumSortOrder = members.front().sortOrder;
    float backgroundDepth = ParticleEmitterViewDepth(
        members.front(), cameraPosition, forward);
    outKey = members.front();
    for (size_t index = 1; index < members.size(); ++index)
    {
        const ParticleEmitterDepthKey& member = members[index];
        minimumSortOrder = std::min(minimumSortOrder, member.sortOrder);
        const float depth =
            ParticleEmitterViewDepth(member, cameraPosition, forward);
        if (depth > backgroundDepth)
        {
            backgroundDepth = depth;
            // 复制真实成员的包围球，让 ComputeParticleEmitterDrawOrder
            // 重新求得同一个深度，而不是依赖额外的 override 字段。
            outKey.worldCenter = member.worldCenter;
            outKey.worldRadius = member.worldRadius;
        }
    }
    outKey.key = kParticleGlobalDrawKey;
    outKey.sortOrder = minimumSortOrder;
    return true;
}

} // namespace VL
