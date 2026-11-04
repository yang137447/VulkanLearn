#pragma once

#include <algorithm>
#include <cmath>

#include <Eigen/Dense>

#include "particles/particleAsset.h"

namespace VL
{

// 阴影 caster 的启用条件只有一个来源：作者开关 + mesh 渲染 + 网格确实
// 成功导入。回退 billboard 的 mesh emitter 必须不投影，否则会用错误的
// 几何形状写 shadow map。其它 renderMode 即使写了 castShadows 也忽略，
// 本轮只支持 mesh 粒子。
inline bool ParticleEmitterCastsMeshShadow(
    const ParticleEmitterAsset& asset,
    bool meshFallbackBillboard = false)
{
    return asset.castShadows &&
        asset.renderMode == "mesh" &&
        !meshFallbackBillboard;
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
