#pragma once

#include "terrain/terrainAssetTypes.h"

namespace VL
{
namespace TerrainHeightmap
{

// 把像素统一成标量高度样本；仅支持解码器实际会产出的格式，未知格式直接报错。
float SampleNormalized(const HostImage& image, float u, float v);

// h = sample * heightScale + heightOffset。u/v 是 [0,1] 的地形 UV。
float SampleHeightCm(
    const TerrainAssetDesc& desc,
    const HostImage& image,
    float u,
    float v);

} // namespace TerrainHeightmap
} // namespace VL
