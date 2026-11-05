#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

namespace VL
{

// GPU particle light 的固定容量。该预算与普通粒子池/全局排序池完全独立：
// light 发射器仍各自拥有最多 64 个粒子的模拟池，但所有发射器共享这一份
// per-swapchain-image 输出 SSBO。
inline constexpr uint32_t kMaxParticleLights = 64;

// std430 布局：uvec4 header 后接 32 字节的 ParticleLightGPU 数组。
// count 是已写入槽位的 high-water mark；槽位允许空洞（死亡/被截断的粒子
// 不写），光照 shader 用零强度跳过空槽。
struct alignas(16) ParticleLightSSBOHeader
{
    uint32_t count = 0;
    uint32_t capacity = kMaxParticleLights;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};

// 与 common/lighting.glsl 的 ParticleLightGPU 逐字段一致。
// colorIntensity.xyz 是线性 RGB tint；w 是 cd * 100^2，与 LightGPU 相同。
struct alignas(16) ParticleLightGPU
{
    Eigen::Vector4f positionRadius = Eigen::Vector4f::Zero();
    Eigen::Vector4f colorIntensity = Eigen::Vector4f::Zero();
    // x=local shadow slice, y=mode, z=far cm, w=bias cm。
    Eigen::Vector4f shadowParams = Eigen::Vector4f(-1.0f, 0.0f, 0.0f, 0.0f);
    // x=IES profile index, y=has Ies, z/w reserved。
    Eigen::Vector4f meta = Eigen::Vector4f::Zero();
};

static_assert(
    sizeof(ParticleLightSSBOHeader) == 16,
    "ParticleLight SSBO header must match std430 uvec4");
static_assert(
    sizeof(ParticleLightGPU) == 64,
    "ParticleLightGPU must match std430 four vec4 layout");
static_assert(
    offsetof(ParticleLightGPU, colorIntensity) == 16,
    "ParticleLightGPU colorIntensity offset must match std430");

inline constexpr std::size_t ParticleLightSSBOSize()
{
    return sizeof(ParticleLightSSBOHeader) +
        sizeof(ParticleLightGPU) * kMaxParticleLights;
}

// 每个 light 发射器在全局 64 槽中的确定性子区间。
struct ParticleLightSlot
{
    std::string key;
    uint32_t base = 0;
    uint32_t capacity = 0;
};

// 稳定 key 顺序 first-fit：请求按 key 排序后依次占用剩余槽位。
// 这样同一帧、同一组发射器的截断结果与 unordered_map 迭代顺序无关。
inline std::vector<ParticleLightSlot> AllocateParticleLightSlots(
    std::vector<std::pair<std::string, uint32_t>> requests,
    uint32_t totalCapacity = kMaxParticleLights)
{
    std::sort(
        requests.begin(),
        requests.end(),
        [](const auto& left, const auto& right)
        {
            return left.first < right.first;
        });

    std::vector<ParticleLightSlot> slots;
    slots.reserve(requests.size());
    uint32_t next = 0;
    for (const auto& [key, requested] : requests)
    {
        ParticleLightSlot slot;
        slot.key = key;
        slot.base = next;
        const uint32_t remaining =
            next < totalCapacity ? totalCapacity - next : 0;
        slot.capacity = std::min(requested, remaining);
        next += slot.capacity;
        slots.push_back(std::move(slot));
    }
    return slots;
}

} // namespace VL
