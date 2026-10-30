#pragma once

#include <cstdint>
#include <vector>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

struct TerrainTileCandidateList
{
    uint32_t tileX = 0;
    uint32_t tileZ = 0;
    std::vector<uint8_t> layerIds;
};

// Phase B 的 CPU 烘焙结果：每 texel 4 个全局 layer ID + 4 个 8-bit 权重。
// ids/weights 都是 RGBA8，按行优先；tiles 保存每 tile 的候选层，未来直接喂给 VT。
struct TerrainWeightMap
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> ids;
    std::vector<uint8_t> weights;
    std::vector<TerrainTileCandidateList> tiles;
};

// C2c：单个 tile 的 page 烘焙结果。TerrainPageManager 按需请求页面时只生成这一块，
// 不再为了一个 tile 复制整张地形的权重图。
struct TerrainPageData
{
    uint32_t pageResolution = 0;
    std::vector<uint8_t> ids;
    std::vector<uint8_t> weights;
    TerrainTileCandidateList candidates;
};

// Phase C1 的最小 VT：page table 记录 tile -> atlas page，atlas 仍是全量常驻。
// 后续 feedback/eviction 会替换 atlas 内容，但 page table/UV 语义保持不变。
struct TerrainVirtualTexture
{
    uint32_t pageTableSize = 0;
    uint32_t tilesPerSide = 0;
    uint32_t pageResolution = 0;
    uint32_t atlasResolution = 0;
    std::vector<uint8_t> pageTable;
    std::vector<uint8_t> ids;
    std::vector<uint8_t> weights;
    std::vector<TerrainTileCandidateList> tiles;
};

class TerrainWeightMapBuilder
{
public:
    static TerrainWeightMap Build(
        const TerrainBuildPlan& plan,
        uint32_t resolution,
        uint32_t tilesPerSide);

    // 只烘焙指定 tile；kernel 与 Build/BuildVirtualTexture 使用同一套采样和 top-4 规则。
    static TerrainPageData BuildPage(
        const TerrainBuildPlan& plan,
        uint32_t tilesPerSide,
        uint32_t pageResolution,
        uint32_t tileX,
        uint32_t tileZ);

    static TerrainVirtualTexture BuildVirtualTexture(
        const TerrainBuildPlan& plan,
        uint32_t tilesPerSide,
        uint32_t pageResolution,
        uint32_t pageTableSize);
};

} // namespace VL
