#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainMeshBuilder.h"

namespace VL
{

// 加载期几何 chunk 驻留的输入。相机位置只在加载期使用：第一版不做运行时
// 异步重传，窗口外的细 LOD 直接不创建，运行时只影响选择/兜底。
struct TerrainChunkResidencyDesc
{
    // false 时退回“所有级全部创建”的历史行为（测试/离线/无相机场景）。
    bool cameraKnown = false;
    Eigen::Vector3f cameraPositionCm = Eigen::Vector3f::Zero();
};

// 一个地形摆放里某一级 LOD 的几何统计。chunkCount 是本级实际创建的 chunk，
// skippedChunkCount 是因驻留窗口被跳过的 chunk；两者之和才是“全量创建”的数量，
// 因此可以不借助另一套基线就直接给出加载期节省。
struct TerrainChunkLevelStats
{
    uint32_t lodLevel = 0;
    bool skirt = false;
    uint32_t chunkCount = 0;
    uint32_t skippedChunkCount = 0;
    uint64_t vertexCount = 0;
    uint64_t indexCount = 0;
    uint64_t skippedVertexCount = 0;
    uint64_t skippedIndexCount = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
    uint64_t skippedVertexBytes = 0;
    uint64_t skippedIndexBytes = 0;
};

struct TerrainChunkPlacementStats
{
    std::string placementId;
    std::string assetPath;
    uint32_t chunksPerSide = 0;
    uint32_t lodLevelCount = 1;
    bool residencyEnabled = false;
    std::vector<float> residencyDistancesCm;
    std::vector<TerrainChunkLevelStats> levels;
};

// 加载期一次性写入、之后只读的几何驻留快照；console `terrainchunk stats`
// 读取它做基线，不参与每帧渲染。
struct TerrainChunkResidencyStats
{
    bool valid = false;
    std::vector<TerrainChunkPlacementStats> placements;

    uint64_t TotalVertexBytes() const;
    uint64_t TotalIndexBytes() const;
    uint64_t TotalSkippedVertexBytes() const;
    uint64_t TotalSkippedIndexBytes() const;
    uint32_t TotalChunks() const;
    uint32_t TotalSkippedChunks() const;
};

// 单 chunk 按 LOD 的确定性顶点/索引数量（含 skirt），用于给被跳过的 chunk
// 估算“如果不做驻留会占多少”。它必须与 TerrainMeshBuilder::BuildChunks
// 的实际输出一致，测试里逐级对照。
uint64_t TerrainChunkVertexCount(
    const TerrainAssetDesc& desc,
    uint32_t lodLevel,
    bool buildSkirt);
uint64_t TerrainChunkIndexCount(
    const TerrainAssetDesc& desc,
    uint32_t lodLevel,
    bool buildSkirt);

// 把局部 AABB 变换到世界空间 AABB；与 WorldSnapshotBuilder 的 LOD 距离口径一致，
// 避免加载期驻留和运行时选择使用两个不同的距离。
void TerrainChunkWorldBounds(
    const Eigen::Matrix4f& model,
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    Eigen::Vector3f& worldMin,
    Eigen::Vector3f& worldMax);

// 加载期单级判定：相机未知 / 单级地形时恒为 true；否则按
// TerrainChunkLodIsResident 的距离窗口判断。
bool TerrainChunkLevelIsResident(
    const TerrainChunkResidencyDesc& residency,
    const std::vector<float>& residencyDistancesCm,
    const Eigen::Matrix4f& model,
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    uint32_t lodLevel,
    uint32_t lodLevelCount);

} // namespace VL
