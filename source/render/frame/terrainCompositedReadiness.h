#pragma once

#include <cstdint>
#include <Eigen/Core>
#include "render/frame/terrainCompositedQualityValidation.h"

namespace VL
{
bool TerrainCompositedQualityReady(
    uint64_t worldGeneration,
    const Eigen::Matrix4f& viewProjection);
// 到达收敛：只反映本代/本次到达是否曾通过一次质量快照。加载黑帧与输入闸门用它，
// 不能随普通转身/走动重新进入 Loading；viewProjection 仅为统一 getter 签名保留。
bool TerrainCompositedArrivalReady(
    uint64_t worldGeneration,
    const Eigen::Matrix4f& viewProjection);
double TerrainCompositedQualityMonotonicMs();
void RequestTerrainCompositedQualityRevalidation(uint64_t worldGeneration);
TerrainQualityInvalidation TerrainCompositedQualityInvalidation();
}
