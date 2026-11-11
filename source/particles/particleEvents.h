#pragma once

#include <cstdint>

#include <Eigen/Dense>

namespace VL
{

// GPU 事件记录：写满即丢弃（bounded），计数仍在 counter 里单调累加，
// 读取端用 min(count, capacity) 判断是否有截断。
enum class ParticleEventType : uint32_t
{
    Spawn = 0,
    Death = 1,
    Collision = 2
};

constexpr uint32_t kParticleEventCapacity = 4096;

// std430 布局与 particleCommon.glsl 的 ParticleEvent 一一对应。
struct ParticleEventRecord
{
    Eigen::Vector4f positionAge;
    Eigen::Vector4f velocityLife;
    // x = ParticleEventType, y = particle index, z/w reserved
    Eigen::Vector4f eventData;
    // xyz = collision normal（无事件时为 0）, w reserved
    Eigen::Vector4f normal;
};

// 每个发射器每帧清零的计数器：alive、events（总写入槽位）、
// spawn/death/collision（逐类型计数）。总槽位保证三种事件共享同一
// bounded buffer 时不会互相覆盖。
struct ParticleEventCounters
{
    uint32_t alive = 0;
    uint32_t events = 0;
    uint32_t spawn = 0;
    uint32_t death = 0;
    uint32_t collision = 0;
    // 容量上限；fillBuffer 只清零前 5 个 uint，capacity 在创建后保持不变。
    uint32_t capacity = 0;
};

// 图形阶段调试计数。只统计通过 discard 并最终写出的片元数，
// 因此它可作为该 emitter/全局池这一帧的 GPU overdraw 样本数。
struct ParticleOverdrawCounters
{
    uint32_t fragments = 0;
    uint32_t reserved = 0;
};

static_assert(
    sizeof(ParticleEventRecord) == sizeof(Eigen::Vector4f) * 4,
    "ParticleEventRecord must match the std430 ParticleEvent layout");
static_assert(
    sizeof(ParticleEventCounters) == sizeof(uint32_t) * 6,
    "ParticleEventCounters must match the GPU counter layout");
static_assert(
    sizeof(ParticleOverdrawCounters) == sizeof(uint32_t) * 2,
    "ParticleOverdrawCounters must match the graphics debug layout");

} // namespace VL
