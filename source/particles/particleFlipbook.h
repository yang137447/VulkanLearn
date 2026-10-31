#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <Eigen/Dense>

#include "particles/particleAsset.h"

namespace VL
{

// 16x16 图集上限；每帧一份 duration/pivot，GPU 表按 vec4 打包。
inline constexpr uint32_t kMaxParticleFlipbookFrames = 256;
inline constexpr uint32_t kParticleFlipbookDurationVectors =
    kMaxParticleFlipbookFrames / 4;
inline constexpr uint32_t kParticleFlipbookPivotVectors =
    kMaxParticleFlipbookFrames / 2;
// 逐帧 UV rect 每帧独占一个 vec4，GPU 表固定 64 项；因此带 UV rect 的
// 资产帧区间上限是 64，避免 256 帧资产被静默截断。
inline constexpr uint32_t kParticleFlipbookUvVectors = 64;
// 逐帧旋转每 vec4 打包 4 帧，GPU 表固定 16 个 vec4（最多 64 帧）。
inline constexpr uint32_t kParticleFlipbookRotationVectors = 16;

// std140 打包：durations 每 vec4 装 4 帧，pivots 每 vec4 装 2 帧
// (u,v)，UV rect 每帧独占一个 vec4；flags =
// (hasDurations, hasPivots, frameCount, hasUVs)。
struct ParticleFlipbookGpuTable
{
    std::array<Eigen::Vector4f, kParticleFlipbookDurationVectors> durations{};
    std::array<Eigen::Vector4f, kParticleFlipbookPivotVectors> pivots{};
    std::array<Eigen::Vector4f, kParticleFlipbookUvVectors> uvs{};
    std::array<Eigen::Vector4f, kParticleFlipbookRotationVectors> rotations{};
    Eigen::Vector4f flags = Eigen::Vector4f::Zero();
    // 对应 UBO 的 flipbookParams4：x = hasRotations，y/z/w 保留。
    Eigen::Vector4f rotationFlags = Eigen::Vector4f::Zero();
};

// sprite 空间 [0,1] 的 UV 子矩形映射，镜像 shader 的
// mix(rect.xy, rect.zw, localUV)。
inline Eigen::Vector2f MapParticleFlipbookSpriteUV(
    const Eigen::Vector4f& rect,
    const Eigen::Vector2f& localUV)
{
    return Eigen::Vector2f(
        rect.x() + (rect.z() - rect.x()) * localUV.x(),
        rect.y() + (rect.w() - rect.y()) * localUV.y());
}

// CPU 侧的帧相位/选帧镜像，供资产测试与文档核对 shader 语义。
// weights 是相对时长；fps>0 时一轮固定为 frameCount/fps 秒。
struct ParticleFlipbookPhaseInput
{
    float framesPerSecond = 0.0f;
    bool loop = true;
    bool randomStartFrame = false;
    float elapsedSeconds = 0.0f;
    float lifeT = 0.0f;
    // 已离散到 [0,1) 的粒子随机相位。
    float randomSeed01 = 0.0f;
    const float* weights = nullptr;
    uint32_t frameCount = 0;
};

inline float ParticleFlipbookTotalWeight(
    const float* weights,
    uint32_t frameCount)
{
    if (weights == nullptr)
    {
        return 0.0f;
    }
    float total = 0.0f;
    for (uint32_t index = 0; index < frameCount; ++index)
    {
        total += std::max(weights[index], 0.0f);
    }
    return total;
}

inline float ComputeParticleFlipbookPhaseWeight(
    const ParticleFlipbookPhaseInput& input)
{
    if (input.frameCount == 0 || input.weights == nullptr)
    {
        return 0.0f;
    }
    const float totalWeight =
        ParticleFlipbookTotalWeight(
            input.weights, input.frameCount);
    if (totalWeight <= 0.0f)
    {
        return 0.0f;
    }

    if (input.framesPerSecond > 0.0f)
    {
        const float totalSeconds =
            static_cast<float>(input.frameCount) /
            input.framesPerSecond;
        if (totalSeconds <= 0.0f)
        {
            return 0.0f;
        }
        float phase =
            input.elapsedSeconds / totalSeconds * totalWeight;
        if (input.randomStartFrame)
        {
            phase += input.randomSeed01 * totalWeight;
        }
        phase = input.loop
            ? std::fmod(phase, totalWeight)
            : std::min(phase, totalWeight);
        return std::max(phase, 0.0f);
    }

    return std::clamp(input.lifeT, 0.0f, 1.0f) * totalWeight;
}

inline uint32_t SelectParticleFlipbookFrame(
    const float* weights,
    uint32_t frameCount,
    float phaseWeight)
{
    if (weights == nullptr || frameCount == 0)
    {
        return 0;
    }
    float accumulated = 0.0f;
    for (uint32_t index = 0; index < frameCount; ++index)
    {
        const float duration = std::max(weights[index], 0.0f);
        if (phaseWeight < accumulated + duration)
        {
            return index;
        }
        accumulated += duration;
    }
    return frameCount - 1;
}

// 帧区间是闭区间；非法区间返回 0，调用方已由 ValidateEmitter 拦截。
inline uint32_t ParticleFlipbookFrameCount(
    const ParticleEmitterAsset& emitter)
{
    if (emitter.flipbookEndFrame < emitter.flipbookStartFrame)
    {
        return 0;
    }
    return emitter.flipbookEndFrame - emitter.flipbookStartFrame + 1;
}

inline ParticleFlipbookGpuTable BuildParticleFlipbookGpuTable(
    const ParticleEmitterAsset& emitter)
{
    ParticleFlipbookGpuTable table;
    // Eigen 的默认构造不置零，显式清零保证未使用槽位是确定的。
    for (Eigen::Vector4f& packed : table.durations)
    {
        packed.setZero();
    }
    for (Eigen::Vector4f& packed : table.pivots)
    {
        packed.setZero();
    }
    for (Eigen::Vector4f& packed : table.uvs)
    {
        packed.setZero();
    }
    for (Eigen::Vector4f& packed : table.rotations)
    {
        packed.setZero();
    }
    table.flags.z() = static_cast<float>(
        ParticleFlipbookFrameCount(emitter));

    if (!emitter.flipbookFrameDurations.empty())
    {
        table.flags.x() = 1.0f;
        const size_t count = std::min<size_t>(
            emitter.flipbookFrameDurations.size(),
            kMaxParticleFlipbookFrames);
        for (size_t index = 0; index < count; ++index)
        {
            table.durations[index / 4][index % 4] =
                emitter.flipbookFrameDurations[index];
        }
    }
    if (!emitter.flipbookFramePivots.empty())
    {
        table.flags.y() = 1.0f;
        const size_t count = std::min<size_t>(
            emitter.flipbookFramePivots.size(),
            kMaxParticleFlipbookFrames);
        for (size_t index = 0; index < count; ++index)
        {
            const Eigen::Vector2f& pivot =
                emitter.flipbookFramePivots[index];
            table.pivots[index / 2][(index % 2) * 2] = pivot.x();
            table.pivots[index / 2][(index % 2) * 2 + 1] = pivot.y();
        }
    }
    if (!emitter.flipbookFrameUVs.empty())
    {
        table.flags.w() = 1.0f;
        const size_t count = std::min<size_t>(
            emitter.flipbookFrameUVs.size(),
            kParticleFlipbookUvVectors);
        for (size_t index = 0; index < count; ++index)
        {
            table.uvs[index] = emitter.flipbookFrameUVs[index];
        }
    }
    if (!emitter.flipbookFrameRotations.empty())
    {
        table.rotationFlags.x() = 1.0f;
        const size_t count = std::min<size_t>(
            emitter.flipbookFrameRotations.size(),
            kParticleFlipbookRotationVectors * 4);
        for (size_t index = 0; index < count; ++index)
        {
            table.rotations[index / 4][index % 4] =
                emitter.flipbookFrameRotations[index];
        }
    }
    return table;
}

} // namespace VL
