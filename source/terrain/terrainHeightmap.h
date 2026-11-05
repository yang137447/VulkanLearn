#pragma once

#include "terrain/terrainAssetTypes.h"

namespace VL
{
namespace TerrainHeightmap
{

// mesh UV [0,1] -> heightmap UV。margin=0 时是恒等映射；margin>0 时
// mesh 的边界落在 heightmap 的内圈，外圈只用于高度/法线/权重的邻接采样。
struct MeshUvTransform
{
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;

    Eigen::Vector2f Apply(float u, float v) const
    {
        return Eigen::Vector2f(
            offsetU + u * scaleU,
            offsetV + v * scaleV);
    }
};

MeshUvTransform MeshUvTransformFor(
    const TerrainAssetDesc& desc,
    const HostImage& image);

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
