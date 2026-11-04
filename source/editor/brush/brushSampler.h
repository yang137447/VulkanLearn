#pragma once

#include <cstdint>

#include <Eigen/Dense>

#include "editor/brush/brushProfile.h"

namespace VL
{
namespace Editor
{

float EffectiveRadiusCm(const BrushProfile& profile, float pressure);

float SampleWeight(
    const BrushProfile& profile,
    float normalizedRadius,
    float pressure = 1.0f,
    float noiseSample = 0.0f);

// 2D 版本：offset 以“当前有效半径”为单位。Circle 用欧氏距离，Square 用
// Chebyshev 距离，Stamp 在 mask 采样器接入前暂按 Circle 处理；rotationDeg
// 会先旋转 offset。所有形状最终都走同一 falloff/strength/pressure/noise。
float SampleWeightAt(
    const BrushProfile& profile,
    Eigen::Vector2f normalizedOffset,
    float pressure = 1.0f,
    float noiseSample = 0.0f);

float StampSpacingCm(const BrushProfile& profile, float pressure = 1.0f);

Eigen::Vector2f JitteredStampCenter(
    const BrushProfile& profile,
    Eigen::Vector2f center,
    uint32_t stampIndex);

float StableNoise01(uint32_t x, uint32_t y, uint32_t seed);

} // namespace Editor
} // namespace VL
