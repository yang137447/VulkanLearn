#pragma once

#include <cstdint>
#include <Eigen/Core>
#include "render/frame/terrainCompositedQualityValidation.h"

namespace VL
{
bool TerrainCompositedQualityReady(
    uint64_t worldGeneration,
    const Eigen::Matrix4f& viewProjection);
double TerrainCompositedQualityMonotonicMs();
void RequestTerrainCompositedQualityRevalidation(uint64_t worldGeneration);
TerrainQualityInvalidation TerrainCompositedQualityInvalidation();
}
