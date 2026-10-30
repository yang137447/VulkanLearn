#pragma once

#include <cstdint>
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
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

// chunk 资源 key：同一 TR 的多处摆放必须生成不同 key，否则 RenderableObject
// 缓存会让两个放置共享同一份 GPU 顶点/索引缓冲。身份之外还包含所有影响
// 顶点/索引的输入，资产参数变化时也会自然失效。
std::string TerrainChunkResourceKey(
    const TerrainBuildPlan& plan,
    uint32_t chunkX,
    uint32_t chunkZ);

// 高度图在加载期一次性生成 CPU chunk 网格；法线使用全局高度采样中心差分，
// 因此相邻 chunk 的共享边界不会出现法线接缝。
class TerrainMeshBuilder
{
public:
    static std::vector<TerrainMeshChunk> BuildChunks(const TerrainBuildPlan& plan);
};

} // namespace VL
