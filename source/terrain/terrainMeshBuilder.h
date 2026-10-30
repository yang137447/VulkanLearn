#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"
#include "vertexDataStruct.h"

namespace VL
{

// 一个 chunk 的 CPU 顶点/索引与局部 AABB；渲染器 loader 直接用它创建 RenderableObject。
struct TerrainMeshChunk
{
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    // 该份网格对应的 LOD 级（0 = 完整网格）。同一 chunk 的不同 LOD 是
    // 独立资源，AABB/顶点数都不同。
    uint32_t lodLevel = 0;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

// chunk 资源 key：同一 TR 的多处摆放必须生成不同 key，否则 RenderableObject
// 缓存会让两个放置共享同一份 GPU 顶点/索引缓冲。身份之外还包含所有影响
// 顶点/索引的输入（含 LOD 级与合并步长），资产参数变化时也会自然失效。
// lodDistances 只影响运行时选择，不影响单级网格，因此不进入 key。
std::string TerrainChunkResourceKey(
    const TerrainBuildPlan& plan,
    uint32_t chunkX,
    uint32_t chunkZ,
    uint32_t lodLevel = 0);

// 高度图在加载期一次性生成 CPU chunk 网格；法线使用全局高度采样中心差分，
// 因此相邻 chunk 的共享边界不会出现法线接缝。
class TerrainMeshBuilder
{
public:
    // 生成指定 LOD 的全部 chunk。LOD0 与历史实现逐字节一致（无 skirt）；
    // LOD>=1 把每方向 chunkQuads/2^lod 个格子合并一格，并在 chunk 边界
    // 追加一圈向下延伸的 skirt，用于遮挡与更粗 LOD 相邻时的裂缝。
    // buildSkirtOverride 为空时沿用历史规则（仅 LOD>=1 带裙边）；多级地形
    // 需要给 LOD0 也带裙边，否则与相邻 LOD1 chunk 的边界无法完全遮挡。
    static std::vector<TerrainMeshChunk> BuildChunks(
        const TerrainBuildPlan& plan,
        uint32_t lodLevel = 0,
        std::optional<bool> buildSkirtOverride = std::nullopt);
};

// skirt 向下延伸的深度（cm）。当前按地形总高度尺度取 10%，下限 1cm；
// 所有 LOD 级共用同一深度，保证相邻级互相覆盖。
float TerrainChunkSkirtDepthCm(const TerrainAssetDesc& desc);

} // namespace VL
