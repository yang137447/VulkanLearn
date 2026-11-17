#pragma once

#include <memory>

#include <Eigen/Dense>

#include "terrain/terrainCollisionProxy.h"

namespace VL
{

struct TerrainCollisionPatchBuildSettings
{
    // 世界 cm；patch 在宏代理局部 XZ 中按这个尺寸采样。
    float patchSizeCm = 6400.0f;
    float spacingCm = 50.0f;
    // 边缘 blend 到宏高度；A 口径下 patch 的唯一高度源就是宏代理，
    // 因此当前是恒等 blend，保留参数给后续高分辨率碰撞源。
    float edgeBlendCm = 600.0f;
};

// 0.5 m 宏高度碰撞 patch。proxy 字段与 TerrainCollisionProxy 完全同构，
// 因此 body 创建、bounds 和查询身份不需要第二套类型。
struct TerrainCollisionPatch
{
    TerrainCollisionProxy proxy;
    Eigen::Vector2f centerLocalCm = Eigen::Vector2f::Zero();
    float sourcePatchSizeCm = 0.0f;

    bool IsValid() const { return proxy.IsValid(); }
};

// centerLocalCm 是宏代理局部 XZ；patch 必须完全落在代理内，否则返回 nullptr
// （地形边缘回退到宏 body）。高度只来自 macroProxy，tiling layerHeight/detail
// 不进入碰撞。
std::shared_ptr<const TerrainCollisionPatch> BuildTerrainCollisionPatch(
    const TerrainCollisionProxy& macroProxy,
    const Eigen::Vector2f& centerLocalCm,
    const TerrainCollisionPatchBuildSettings& settings = {});

} // namespace VL
