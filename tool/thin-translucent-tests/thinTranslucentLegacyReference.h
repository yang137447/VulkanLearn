#pragma once

// 仅供测试使用的 Thin Translucent 透射闭包数值参考。
// 它刻意放在 tool 下，不进入运行时代码，避免形成必须手工同步的第二套生产实现。
// 本文件只镜像 UE Legacy 透射与降级公式，不镜像已经共享的 Default Lit 反射 BRDF。

#include <algorithm>
#include <array>
#include <cmath>

namespace ThinTranslucentReference
{

using Color = std::array<double, 3>;

inline double ComputeNoV(double normalDotView)
{
    return std::clamp(std::abs(normalDotView) + 1e-5, 0.0, 1.0);
}

inline Color ComputeSpecularColor(
    const Color& baseColor,
    double metallic,
    double specular)
{
    Color color{};
    for (size_t channel = 0; channel < color.size(); ++channel)
    {
        color[channel] =
            (1.0 - metallic) * (0.08 * specular) +
            metallic * baseColor[channel];
    }
    return color;
}

inline Color ComputeLegacyTransmission(
    const Color& transmittanceColor,
    const Color& specularColor,
    double normalDotView,
    double rootOpacity)
{
    // 零次内部往返模型：(1-F)^2 * Absorption * (1-RootOpacity)。
    const double noV = ComputeNoV(normalDotView);
    const double fresnelCurve = std::pow(1.0 - noV, 5.0);
    const double grazingReflectance = std::clamp(
        50.0 * specularColor[1],
        0.0,
        1.0);
    Color transmission{};
    for (size_t channel = 0; channel < transmission.size(); ++channel)
    {
        const double fresnel =
            grazingReflectance * fresnelCurve +
            (1.0 - fresnelCurve) * specularColor[channel];
        const double interfaceTransmission = 1.0 - fresnel;
        const double absorption =
            std::pow(transmittanceColor[channel], 1.0 / noV);
        transmission[channel] =
            interfaceTransmission * interfaceTransmission *
            absorption * (1.0 - rootOpacity);
    }
    return transmission;
}

inline Color ApplySurfaceCoverage(
    const Color& legacyTransmission,
    double surfaceCoverage)
{
    // Coverage=0 表示像素未被薄表面覆盖，目标颜色乘数必须退回 1。
    Color multiplier{};
    for (size_t channel = 0; channel < multiplier.size(); ++channel)
    {
        multiplier[channel] =
            (1.0 - surfaceCoverage) +
            surfaceCoverage * legacyTransmission[channel];
    }
    return multiplier;
}

inline double ComputeFallbackOpacity(
    const Color& destinationMultiplier)
{
    // 普通 alpha blend 无法表达 RGB 目标乘数，只能取平均值保存总体透射强度。
    const double averageMultiplier =
        (destinationMultiplier[0] +
            destinationMultiplier[1] +
            destinationMultiplier[2]) /
        3.0;
    return 1.0 - averageMultiplier;
}

} // namespace ThinTranslucentReference
