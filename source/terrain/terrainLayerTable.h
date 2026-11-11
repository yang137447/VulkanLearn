#pragma once

#include <cstdint>
#include <vector>

#include "resource/image/hostImage.h"
#include "terrain/terrainAssetTypes.h"

namespace VL
{

// catalog 的 CPU 视图：与 256x4 RGBA32F 的 4 个 texel 一一对应。
// sourceResolution 不入 catalog，由 pagesPerSide * 128 还原，供 L1 校验与
// L2 计算页表基址。
struct TerrainLayerCatalogEntry
{
    // row0
    float heightMin = 0.0f;
    float heightMax = 0.0f;
    float slopeMin = 0.0f;
    float slopeMax = 0.0f;
    // row1
    float noise = 0.0f;
    float blendEdge = 0.0f;
    float uvScale = 0.0f;
    float uvRotationRadians = 0.0f;
    // row2
    uint32_t pagesPerSide = 0;
    uint32_t pageTableBase = 0;
    uint32_t sourceSet = 0;
    uint32_t flags = 0;
    // row3
    Eigen::Vector3f avgColor = Eigen::Vector3f::Zero();
    // row3.w：位移峰峰值（米）。height blend 用它把每层高度换算到米，
    // 否则把每层归一化到 0..1 会让几乎平整的沙层和岩石同样起伏。
    float displacementM = 0.0f;
    // CPU 侧推导值，不占用 catalog 通道。
    uint32_t sourceResolution = 0;
};

// 全局层 catalog：最多 256 层打包成 256x4 RGBA32F LUT。
// row0 = (heightMin, heightMax, slopeMin, slopeMax)
// row1 = (noise, blendEdge, uvScale, uvRotationRadians)
// row2 = (pagesPerSide(mip0), pageTableBase, sourceSet, flags)
// row3 = (avgColor.r, avgColor.g, avgColor.b, displacementM)
// pageTableBase 按层顺序累加每层的总页数（512=21、1K=85、2K=341），
// 是“每层一张扁平表”变体的基址；当前 shader 只读 row1，L2 才消费 row2/row3。
class TerrainLayerTable
{
public:
    static constexpr uint32_t MaxLayers = 256;
    static constexpr uint32_t Rows = 4;
    // 高度混合边缘的当前固定值；TR 还没有显式字段，catalog 先把它写实。
    static constexpr float DefaultBlendEdge = 0.05f;

    // 按 catalog 行布局生成 256x4 的 CPU 条目；未使用层全零。
    static std::vector<TerrainLayerCatalogEntry> BuildEntries(
        const std::vector<TerrainLayerDesc>& layers);
    static HostImage BuildImage(const std::vector<TerrainLayerDesc>& layers);
    // 从 BuildImage 的产物读回一层；未使用层（row2 的 pagesPerSide=0）返回全零。
    static TerrainLayerCatalogEntry ReadLayer(
        const HostImage& image,
        uint32_t layerId);

    // World 级 layer table 共享前提：所有共享 M_terrain 的地形必须使用同一套层定义。
    static bool AreLayersIdentical(
        const std::vector<TerrainLayerDesc>& lhs,
        const std::vector<TerrainLayerDesc>& rhs);
};

} // namespace VL
