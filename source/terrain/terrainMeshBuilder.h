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
// 单个 chunk 的 LOD0 AABB（不含边界缝合带；缝合带与地表同高），由 ComputeChunkBounds 直接从高度图
// 采样得到，用于加载期决定哪些细 LOD 需要真正生成。
struct TerrainChunkBounds
{
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

// 按 chunk 过滤生成。chunkMask 为空表示“生成全部”，保持历史行为；
// 非空时长度必须是 chunksPerSide²，只生成 mask 为真的 chunk。
// 顶点/索引内容与过滤无关（未生成的 chunk 只是不出现），因此不能把窗口
// 写进资源 key。
struct TerrainChunkBuildFilter
{
    std::vector<uint8_t> chunkMask;

    bool Empty() const noexcept { return chunkMask.empty(); }
    bool Accepts(
        uint32_t chunkX,
        uint32_t chunkZ,
        uint32_t chunksPerSide) const;
};

class TerrainMeshBuilder
{
public:
    // 生成指定 LOD 的全部 chunk。LOD0 与历史实现逐字节一致；
    // LOD>=1 把每方向 chunkQuads/2^lod 个格子合并一格，并把四条边界改为
    // 按 LOD0 间距采样的边界环 + 一圈过渡三角形，保证与任意 LOD 的邻居水密。
    static std::vector<TerrainMeshChunk> BuildChunks(
        const TerrainBuildPlan& plan,
        uint32_t lodLevel = 0,
        const TerrainChunkBuildFilter* filter = nullptr);

    // 只计算 LOD0 的 chunk AABB（顶点位置的范围），不生成法线/索引。
    // 加载期先用它确定窗口，再按窗口生成真正需要的网格；返回顺序与
    // BuildChunks 一致（chunkZ 外层、chunkX 内层）。
    static std::vector<TerrainChunkBounds> ComputeChunkBounds(
        const TerrainBuildPlan& plan);
};

// skirt 向下延伸的深度（cm）。当前按地形总高度尺度取 10%，下限 1cm；
// 所有 LOD 级共用同一深度，保证相邻级互相覆盖。
} // namespace VL
