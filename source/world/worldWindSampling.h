#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "world/worldWind.h"

namespace VL
{

// 场景风强度（cm/s）换算到响应曲线横轴 [0, 1] 的基准。3270 cm/s（32.7 m/s）是
// 蒲福 12 级的下界，因此横轴 1.0 对应最强风级。这是 VulkanLearn 的换算约定，
// 不是 SpeedTree SDK 或 TressFX 的参数；曲线采样方自行决定是否再夹取。
inline constexpr float SceneWindCurveFullScaleCmPerSec = 3270.0f;

inline float SceneWindCurveAxis(float strengthCmPerSec)
{
    return std::clamp(
        strengthCmPerSec / SceneWindCurveFullScaleCmPerSec, 0.0f, 1.0f);
}

namespace SceneWindDetail
{

// 阵风 turbulence 为 1 时，细节八度相对粗八度的频率倍率；offset 让两个八度
// 的格点错开，避免 turbulence 两端出现相同相位。
constexpr double GustFineOctaveRatio = 4.0;
constexpr double GustFineOctaveOffset = 17.0;

inline float Interpolate(float start, float end, float amount)
{
    return start + (end - start) * amount;
}

// 确定性整数格点哈希，返回 [-1, 1]。只依赖格点编号，不依赖运行时长或帧率，
// 因此同一风时间总得到同一阵风值，截图可复现。
inline float LatticeValue(int64_t lattice)
{
    uint32_t hash = static_cast<uint32_t>(lattice) * 0x9E3779B9u;
    hash ^= hash >> 16;
    hash *= 0x85EBCA6Bu;
    hash ^= hash >> 13;
    hash *= 0xC2B2AE35u;
    hash ^= hash >> 16;
    return static_cast<float>(hash) / static_cast<float>(UINT32_MAX) * 2.0f - 1.0f;
}

// 一维 value noise，格点间 smoothstep 插值。相位用 double 保存：风时间长时间
// 运行后 float 的小数位会丢失，阵风会出现台阶。
inline float ValueNoise(double phase)
{
    const double cell = std::floor(phase);
    const float fraction = static_cast<float>(phase - cell);
    const float smooth = fraction * fraction * (3.0f - 2.0f * fraction);
    const int64_t lattice = static_cast<int64_t>(cell);
    return Interpolate(LatticeValue(lattice), LatticeValue(lattice + 1), smooth);
}

} // namespace SceneWindDetail

// 阵风是连续噪声对归一化基准强度的相对调制（输入输出均为 [0, 1] 归一化强度），
// 不是 SpeedTree 的离散阵风事件：没有持续时间与上升/下降系数。amplitude 决定
// 调制幅度，frequency 决定噪声格点频率，turbulence 在平滑粗八度与细节八度之间
// 插值。frequency 为 0 时阵风不产生调制，强度即为基准值。
// 场级消费者（头发）的阵风实现；SpeedTree 植被使用各 profile 自己的离散 gust，
// 不调用这里。场级消费者共享同一风时间时，其阵风相位一致。
inline float ApplySceneWindGust(
    float baseStrength, const WorldWindGust& gust, double timeSeconds)
{
    if (baseStrength <= 0.0f ||
        !gust.enabled ||
        gust.amplitude <= 0.0f ||
        gust.frequency <= 0.0f)
    {
        return baseStrength;
    }

    const double phase = timeSeconds * static_cast<double>(gust.frequency);
    const float coarse = SceneWindDetail::ValueNoise(phase);
    const float fine = SceneWindDetail::ValueNoise(
        phase * SceneWindDetail::GustFineOctaveRatio +
        SceneWindDetail::GustFineOctaveOffset);
    const float noise = SceneWindDetail::Interpolate(
        coarse, fine, gust.turbulence);
    // amplitude 限制在 [0, 1]，因此调制因子非负；噪声本身在 [-1, 1] 内。
    const float modulation = 1.0f + gust.amplitude * noise;
    return baseStrength * modulation;
}

} // namespace VL
