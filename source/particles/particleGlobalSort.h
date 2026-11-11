#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace VL
{

// 跨发射器透明排序的 v1 上限。超过上限时回退到逐发射器排序，避免
// 全局 bitonic 网络的 dispatch 成本失控。
constexpr uint32_t kParticleGlobalSortCapacity = 8192;
constexpr uint32_t kParticleGlobalTextureSlots = 8;
// GatheredBillboard 的 std430 stride：15 个 billboard 基础 vec4 + 4 个逐发射器
// 光照 vec4 + ribbon 第四角/kind + ribbon 末端颜色。shader 侧三处结构体定义
// 必须与这个值保持同步。
constexpr uint32_t kParticleGlobalRecordVec4Count = 17;

// CPU 镜像：一个已聚集的透明 billboard 粒子。GPU gather 写入同样的
// 字段顺序；CPU 侧只用于确定性排序测试与容量决策。
struct ParticleGlobalSortKey
{
    float viewDepth = 0.0f;
    uint32_t emitterIndex = 0;
    uint32_t particleIndex = 0;
    uint64_t serial = 0;
};

// back-to-front：深度大的先画。同深度时按 (emitterIndex, particleIndex)
// 稳定排序，保证相同输入得到相同 draw order。
inline bool ParticleGlobalSortKeyBefore(
    const ParticleGlobalSortKey& left,
    const ParticleGlobalSortKey& right) noexcept
{
    if (left.viewDepth != right.viewDepth)
    {
        return left.viewDepth > right.viewDepth;
    }
    if (left.emitterIndex != right.emitterIndex)
    {
        return left.emitterIndex < right.emitterIndex;
    }
    if (left.particleIndex != right.particleIndex)
    {
        return left.particleIndex < right.particleIndex;
    }
    return left.serial < right.serial;
}

inline void SortParticleGlobalKeysBackToFront(
    std::vector<ParticleGlobalSortKey>& keys)
{
    std::stable_sort(
        keys.begin(),
        keys.end(),
        ParticleGlobalSortKeyBefore);
}

// 全局路径接纳 alpha billboard 与 alpha ribbon；容量按 billboard 粒子数
// 或 ribbon quad 数统计。纹理槽位不足或预算超限时整体回退到逐发射器路径。
inline bool CanUseParticleGlobalSort(
    uint64_t totalCapacity,
    uint32_t uniqueTextureCount) noexcept
{
    return totalCapacity > 0 &&
        totalCapacity <= kParticleGlobalSortCapacity &&
        uniqueTextureCount <= kParticleGlobalTextureSlots;
}

} // namespace VL
