#pragma once

#include <algorithm>
#include <cmath>

#include <Eigen/Dense>

#include "particles/particleAsset.h"

namespace VL
{

// 阴影 caster 的几何类型。billboard/ribbon 用 alpha-test 近似可见卡片，
// mesh 写真实三角形；beam/light/decal 仍不参与阴影。
enum class ParticleShadowCasterMode
{
    None = 0,
    Mesh = 1,
    Billboard = 2,
    Ribbon = 3
};

// 阴影 caster 的启用条件只有一个来源：作者开关 + 受支持的渲染模式。
// 回退 billboard 的 mesh emitter 必须不投影，否则会用错误的几何形状
// 写 shadow map。
inline ParticleShadowCasterMode ParticleShadowCasterModeFor(
    const ParticleEmitterAsset& asset,
    bool meshFallbackBillboard = false)
{
    if (!asset.castShadows)
    {
        return ParticleShadowCasterMode::None;
    }
    if (asset.renderMode == "mesh")
    {
        return meshFallbackBillboard
            ? ParticleShadowCasterMode::None
            : ParticleShadowCasterMode::Mesh;
    }
    if (asset.renderMode == "billboard")
    {
        return ParticleShadowCasterMode::Billboard;
    }
    if (asset.renderMode == "ribbon")
    {
        return ParticleShadowCasterMode::Ribbon;
    }
    return ParticleShadowCasterMode::None;
}

// 保留旧入口给仅关心“是否 mesh caster”的调用点；新代码应使用
// ParticleShadowCasterModeFor 区分三种几何。
inline bool ParticleEmitterCastsMeshShadow(
    const ParticleEmitterAsset& asset,
    bool meshFallbackBillboard = false)
{
    return ParticleShadowCasterModeFor(asset, meshFallbackBillboard) ==
        ParticleShadowCasterMode::Mesh;
}

// 发射器包围球与局部灯影响范围的保守相交测试。farCm <= 0 表示调用方
// 没有可用的光范围（CSM），此时不剔除。球体判定只会多画、不会漏画：
// 点光是半径球，聚光的影响锥也完全包含在同一半径球内。
inline bool ParticleMeshCasterIntersectsShadowVolume(
    const Eigen::Vector3f& worldCenter,
    float boundsRadius,
    const Eigen::Vector3f& lightPosition,
    float farCm)
{
    if (!(farCm > 0.0f) ||
        !worldCenter.allFinite() ||
        !lightPosition.allFinite() ||
        !std::isfinite(boundsRadius))
    {
        return true;
    }

    const float reach =
        farCm + std::max(boundsRadius, 0.0f);
    return (worldCenter - lightPosition).squaredNorm() <=
        reach * reach;
}

} // namespace VL
