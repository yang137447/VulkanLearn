#pragma once

// 文件职责：L3 层纹理 VT 的纯 CPU 参考实现。shader
// （materialFunction/mf_terrainLayerVT.glsl）必须与这里的页号、mip、
// 页表 texel 和 atlas UV 公式保持一致；测试用这些函数锁定公式，
// 不依赖 GPU。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "terrain/terrainAssetTypes.h"

namespace VL
{
namespace TerrainLayerVT
{

inline constexpr uint32_t MaxLayers = 256;
inline constexpr uint32_t MaxMip = TerrainLayerMaxMipLevel;

// L3 资产校验：数组纹理的所有层共享同一个 extent，因此 catalog 中每层的
// sourceResolution 必须等于真实源图边长；否则页号会指向错误区域。
inline bool CatalogMatchesSourceExtent(
    const std::vector<TerrainLayerDesc>& layers,
    uint32_t width,
    uint32_t height)
{
    if (layers.empty() || width == 0 || height == 0 || width != height)
        return false;
    for (const TerrainLayerDesc& layer : layers)
    {
        if (layer.sourceResolution != width)
            return false;
    }
    return true;
}

// catalog 的 pagesPerSide 是 mip0 每边页数；尾页恒为 128²，
// 因此最大 mip = log2(pagesPerSide)（512 -> 2、1K -> 3、2K -> 4）。
inline uint32_t MaxMipForPagesPerSide(uint32_t pagesPerSide)
{
    uint32_t maxMip = 0;
    while (pagesPerSide > 1 && maxMip < MaxMip)
    {
        pagesPerSide >>= 1;
        ++maxMip;
    }
    return maxMip;
}

inline uint32_t PagesPerSideAtMip(uint32_t pagesPerSide, uint32_t mip)
{
    return std::max(1u, pagesPerSide >> mip);
}

// shader 省略 sourceResolution uniform，用 pagesPerSide * pageResolution
// 还原纹素密度；这里接受 texelsPerPixel = max(|ddx|, |ddy|) * resolution。
inline uint32_t SelectMip(
    float texelsPerPixel,
    uint32_t pagesPerSide,
    uint32_t maxMip)
{
    if (!(texelsPerPixel > 0.0f) || !std::isfinite(texelsPerPixel))
        return 0;
    const float lod = std::log2(texelsPerPixel);
    const int rounded = static_cast<int>(std::floor(lod + 0.5f));
    const int clamped = std::clamp(
        rounded,
        0,
        static_cast<int>(std::min(maxMip, MaxMipForPagesPerSide(pagesPerSide))));
    return static_cast<uint32_t>(clamped);
}

// L4e：console `terrainlayer forcemip` 的 CPU 参考。forcedMip < 0 表示自动；
// 0..4 覆盖自动结果后再按该层实际 mip 链 clamp。shader 的
// TerrainLayerTargetMip（terrainLayerVTMath.glsl）必须与此完全一致。
inline uint32_t ResolveTargetMip(
    uint32_t autoMip,
    int forcedMip,
    uint32_t maxMip)
{
    if (forcedMip < 0)
        return std::min(autoMip, maxMip);
    return std::min(static_cast<uint32_t>(forcedMip), maxMip);
}

// L4e：目标 mip 缺页、shader 向更粗 mip 回退时的混合系数。
// 每差一级混入 20%，上限 60%；gap = 0 必须为 0（完整驻留不改变像素）。
inline float FallbackBlendForMipGap(
    uint32_t targetMip,
    uint32_t usedMip)
{
    if (usedMip <= targetMip)
        return 0.0f;
    const float gap = static_cast<float>(usedMip - targetMip);
    return std::min(0.2f * gap, 0.6f);
}

// L4e：相机前向预取的单位。mip N 的页 (x,y) 在 mip N-1 上覆盖 2×2 个
// 子页；按子页提前请求，相机靠近时更细的 mip 已经驻留，不会先掉到粗页。
// 纯几何：调用方保证 pageX/pageY 在 mip 的 pagesPerSide 内。
struct LayerChildPage
{
    uint32_t pageX = 0;
    uint32_t pageY = 0;
};

inline std::array<LayerChildPage, 4> FinerMipChildren(
    uint32_t pageX,
    uint32_t pageY)
{
    return {LayerChildPage{pageX * 2, pageY * 2},
            LayerChildPage{pageX * 2 + 1, pageY * 2},
            LayerChildPage{pageX * 2, pageY * 2 + 1},
            LayerChildPage{pageX * 2 + 1, pageY * 2 + 1}};
}

// L2 的页表布局把“每个 mip 的最大 stride * 256 层”展开成 2D texel；
// 因此 shader 用 texel 总数 / 256 就能还原 CPU 的 layerMipStride。
inline uint32_t PerLayerStrideFromTexelCount(uint64_t texelCount)
{
    return static_cast<uint32_t>(texelCount / MaxLayers);
}

inline uint64_t VirtualPageIndex(
    uint32_t layer,
    uint32_t pageX,
    uint32_t pageY,
    uint32_t pagesPerSide,
    uint32_t perLayerStride)
{
    return static_cast<uint64_t>(layer) * perLayerStride +
        static_cast<uint64_t>(pageY) * pagesPerSide +
        pageX;
}

struct PageTableTexel
{
    uint32_t x = 0;
    uint32_t y = 0;
};

inline PageTableTexel PageTableTexelFor(
    uint64_t virtualPage,
    uint32_t tableWidth)
{
    PageTableTexel texel;
    if (tableWidth == 0)
        return texel;
    texel.x = static_cast<uint32_t>(virtualPage % tableWidth);
    texel.y = static_cast<uint32_t>(virtualPage / tableWidth);
    return texel;
}

struct LayerPageUv
{
    uint32_t pageX = 0;
    uint32_t pageY = 0;
    float localU = 0.0f;
    float localV = 0.0f;
};

// shader 的平铺切页：fract 处理 repeat 与负 UV，再按 side 切页。
// 注意 u=1.0 经 fract 后回绕到第 0 页第 0 个 texel，这与 repeat 采样一致。
inline LayerPageUv LayerUvToPage(float u, float v, uint32_t pagesPerSide)
{
    LayerPageUv result;
    const uint32_t side = std::max(1u, pagesPerSide);
    const float tiledU = u - std::floor(u);
    const float tiledV = v - std::floor(v);
    const float scaledU = tiledU * static_cast<float>(side);
    const float scaledV = tiledV * static_cast<float>(side);
    result.pageX = std::min(
        static_cast<uint32_t>(std::max(0.0f, std::floor(scaledU))),
        side - 1);
    result.pageY = std::min(
        static_cast<uint32_t>(std::max(0.0f, std::floor(scaledV))),
        side - 1);
    result.localU = scaledU - std::floor(scaledU);
    result.localV = scaledV - std::floor(scaledV);
    return result;
}

struct AtlasUv
{
    float u = 0.0f;
    float v = 0.0f;
};

// physicalPage -> (slice, tileX, tileY) -> atlas UV。local 先做半 texel 内缩，
// 避免双线性 tap 跨到相邻物理页。
inline AtlasUv AtlasUvForPhysicalPage(
    uint32_t physicalPage,
    float localU,
    float localV,
    uint32_t pageResolution,
    uint32_t atlasResolution,
    uint32_t tilesPerSide,
    uint32_t pagesPerSlice)
{
    AtlasUv result;
    if (pageResolution == 0 || atlasResolution == 0 ||
        tilesPerSide == 0 || pagesPerSlice == 0)
    {
        return result;
    }
    const uint32_t tileIndex = physicalPage % pagesPerSlice;
    const uint32_t tileX = tileIndex % tilesPerSide;
    const uint32_t tileY = tileIndex / tilesPerSide;
    const float halfTexel = 0.5f / static_cast<float>(pageResolution);
    const float insetU = std::clamp(localU, halfTexel, 1.0f - halfTexel);
    const float insetV = std::clamp(localV, halfTexel, 1.0f - halfTexel);
    result.u =
        (static_cast<float>(tileX * pageResolution) +
            insetU * static_cast<float>(pageResolution)) /
        static_cast<float>(atlasResolution);
    result.v =
        (static_cast<float>(tileY * pageResolution) +
            insetV * static_cast<float>(pageResolution)) /
        static_cast<float>(atlasResolution);
    return result;
}

inline uint32_t SliceForPhysicalPage(
    uint32_t physicalPage,
    uint32_t pagesPerSlice)
{
    return pagesPerSlice == 0 ? 0 : physicalPage / pagesPerSlice;
}

} // namespace TerrainLayerVT
} // namespace VL
