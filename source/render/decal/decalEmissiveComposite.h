#pragma once

#include <algorithm>
#include <cmath>

namespace VL
{

struct DecalEmissiveColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// DBufferEmissive 写端与合成端的 CPU 镜像，与
// shader/glsl/decal/decal.frag 和 shader/glsl/engine/decalComposite.glsl
// 保持同构：
// - 写端 rgb = emissive * strength * coverage；
// - GPU 用 One/OneMinusSrcAlpha 做预乘 over；
// - 合成端只累加预乘后的 rgb，不再乘一次 coverage。
inline float DecalEmissiveCoverage(float coverage)
{
    if (!std::isfinite(coverage))
        return 0.0f;
    return std::clamp(coverage, 0.0f, 1.0f);
}

inline DecalEmissiveColor DecalWriteEmissive(
    const DecalEmissiveColor& emissive,
    float strength,
    float coverage)
{
    const float safeStrength =
        std::isfinite(strength) ? std::max(strength, 0.0f) : 0.0f;
    const float safeCoverage = DecalEmissiveCoverage(coverage);
    const float scale = safeStrength * safeCoverage;
    return DecalEmissiveColor{
        std::max(emissive.r, 0.0f) * scale,
        std::max(emissive.g, 0.0f) * scale,
        std::max(emissive.b, 0.0f) * scale};
}

inline DecalEmissiveColor DecalCompositeEmissive(
    const DecalEmissiveColor& source,
    float sourceCoverage,
    const DecalEmissiveColor& destination)
{
    const float keep = 1.0f - DecalEmissiveCoverage(sourceCoverage);
    return DecalEmissiveColor{
        source.r + destination.r * keep,
        source.g + destination.g * keep,
        source.b + destination.b * keep};
}

inline DecalEmissiveColor ResolveDecalEmissive(
    const DecalEmissiveColor& premultiplied)
{
    return DecalEmissiveColor{
        std::max(premultiplied.r, 0.0f),
        std::max(premultiplied.g, 0.0f),
        std::max(premultiplied.b, 0.0f)};
}

} // namespace VL
