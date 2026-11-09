#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <Eigen/Core>

namespace VL
{

// 共享 64 槽 decal 预算。GPU decal gather 只写紧凑存活记录，容量与
// DecalRenderer 的实例上限一致，避免出现 GPU 侧记录多于 DBuffer 槽位。
constexpr uint32_t kParticleDecalGpuCapacity = 64;

// GPU gather 输出缓冲头部：count 使用一个 vec4 的 std430 对齐空间，
// 后续实例数组从 16 字节偏移开始。CPU 回读时按同一布局解析。
struct DecalGpuHeader
{
    uint32_t count = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    uint32_t reserved2 = 0;
};
static_assert(sizeof(DecalGpuHeader) == 16,
    "DecalGpuHeader must keep the vec4-aligned std430 header layout");

// 单条 GPU decal 实例。字段刻意保持 5 个 vec4，CPU/GPU 共用同一份
// std430 偏移：
//   positionLifeT : xyz = simulation-space position, w = normalized lifeT
//   sizeRotation  : xyz = final decal size (cm), w reserved
//   rotationStable: xyz = decal rotation degrees, w = stable particle slot
//   colorOpacity  : rgb = final decal color, a = final opacity
//   lifeInfo      : x = age, y = lifetime, z = stable slot, w reserved
struct ParticleDecalGpuInstance
{
    Eigen::Vector4f positionLifeT = Eigen::Vector4f::Zero();
    Eigen::Vector4f sizeRotation = Eigen::Vector4f::Zero();
    Eigen::Vector4f rotationStable = Eigen::Vector4f::Zero();
    Eigen::Vector4f colorOpacity = Eigen::Vector4f::Zero();
    Eigen::Vector4f lifeInfo = Eigen::Vector4f::Zero();
};
static_assert(sizeof(ParticleDecalGpuInstance) == 80,
    "ParticleDecalGpuInstance must match the std430 gather record");
static_assert(offsetof(ParticleDecalGpuInstance, sizeRotation) == 16,
    "ParticleDecalGpuInstance size/rotation offset must match particleDecalGather.comp");
static_assert(offsetof(ParticleDecalGpuInstance, rotationStable) == 32,
    "ParticleDecalGpuInstance rotation/stable offset must match particleDecalGather.comp");
static_assert(offsetof(ParticleDecalGpuInstance, colorOpacity) == 48,
    "ParticleDecalGpuInstance color/opacity offset must match particleDecalGather.comp");
static_assert(offsetof(ParticleDecalGpuInstance, lifeInfo) == 64,
    "ParticleDecalGpuInstance life info offset must match particleDecalGather.comp");

constexpr uint64_t kParticleDecalGatherBytes =
    sizeof(DecalGpuHeader) +
    sizeof(ParticleDecalGpuInstance) * kParticleDecalGpuCapacity;

// decalId 只由 emitter 稳定 key 与粒子稳定槽位派生，不包含 burst 代际；
// GPU/CPU 两条路径必须使用同一函数，避免同一槽位在切换路径时换纹理集。
inline uint64_t ParticleDecalStableId(
    std::string_view emitterKey,
    uint32_t stableIndex)
{
    constexpr uint64_t fnvPrime = 1099511628211ull;
    // 与旧 CPU 实现 HashString(value, 0x9E3779B97F4A7C15) 完全一致：
    // 非零 seed 直接作为 FNV 初值，不再在结尾混入盐。
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    const std::string_view prefix = "particle-decal|";
    for (unsigned char character : prefix)
    {
        hash ^= character;
        hash *= fnvPrime;
    }
    for (unsigned char character : emitterKey)
    {
        hash ^= character;
        hash *= fnvPrime;
    }
    hash ^= static_cast<unsigned char>('#');
    hash *= fnvPrime;
    const std::string slot = std::to_string(stableIndex);
    for (unsigned char character : slot)
    {
        hash ^= character;
        hash *= fnvPrime;
    }
    return hash;
}

} // namespace VL
