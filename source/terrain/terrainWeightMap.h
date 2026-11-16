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

// 单个权重 texel 的 top-4 烘焙结果（id 已是 World 全局层号）。
// CPU 位移采样器与页烘焙共用同一条 SelectTop4/噪声/高度坡度路径，
// 避免脚 IK 消费另一套权重近似。
struct TerrainTop4Texel
{
    uint8_t ids[4] = {0, 0, 0, 0};
    uint8_t weights[4] = {0, 0, 0, 0};
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
    // layerIdMap 是 local layer 下标 -> World 全局 layerId 的映射；为空表示
    // 恒等映射（单 TR / 旧调用点）。ID/权重页写的是全局 ID，候选层列表
    // 同样重映射，保证 shader 与 feedback 看到同一套层号。
    static TerrainWeightMap Build(
        const TerrainBuildPlan& plan,
        uint32_t resolution,
        uint32_t tilesPerSide,
        const std::vector<uint8_t>& layerIdMap = {});

    // 只烘焙指定 tile；kernel 与 Build/BuildVirtualTexture 使用同一套采样和 top-4 规则。
    static TerrainPageData BuildPage(
        const TerrainBuildPlan& plan,
        uint32_t tilesPerSide,
        uint32_t pageResolution,
        uint32_t tileX,
        uint32_t tileZ,
        const std::vector<uint8_t>& layerIdMap = {});

    // 只计算一个 page 的候选层（局部层号，不重映射到 globalId）。
    // GPU 烘焙把这份列表作为 job 输入，保证与 CPU BuildPage 的
    // SelectTop4 输入顺序完全一致；列表为空时 GPU 侧按层 0 兜底。
    static TerrainTileCandidateList BuildPageCandidates(
        const TerrainBuildPlan& plan,
        uint32_t tilesPerSide,
        uint32_t pageResolution,
        uint32_t tileX,
        uint32_t tileZ);

    // 只烘焙指定 texel 的 top-4，公式与 BuildPage 的 BakeTile 逐 texel 一致。
    // resolution = tilesPerSide * pageResolution；texelX/Y 是整张权重图的绝对 texel。
    // 调用方负责缓存 tile 的候选层列表。
    static TerrainTop4Texel SampleTop4Texel(
        const TerrainBuildPlan& plan,
        uint32_t resolution,
        const TerrainTileCandidateList& candidates,
        uint32_t texelX,
        uint32_t texelY,
        const std::vector<uint8_t>& layerIdMap = {});

    static TerrainVirtualTexture BuildVirtualTexture(
        const TerrainBuildPlan& plan,
        uint32_t tilesPerSide,
        uint32_t pageResolution,
        uint32_t pageTableSize,
        const std::vector<uint8_t>& layerIdMap = {});
};

} // namespace VL
