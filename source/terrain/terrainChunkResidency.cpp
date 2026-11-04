#include "terrain/terrainChunkResidency.h"

#include <algorithm>
#include <limits>

namespace VL
{
namespace
{

// 与 TerrainMeshBuilder 相同的格子数推导；两处必须同步，否则估算字节数
// 会与真实网格偏离。测试用同一份资产逐级对照实际 BuildChunks 输出。
uint32_t QuadsPerChunk(const TerrainAssetDesc& desc, uint32_t lodLevel)
{
    const uint32_t step = TerrainLodStepQuads(lodLevel);
    if (step == 0 || desc.chunkQuads % step != 0)
        return 0;
    return desc.chunkQuads / step;
}

} // namespace

uint64_t TerrainChunkVertexCount(
    const TerrainAssetDesc& desc,
    uint32_t lodLevel,
    bool stitched)
{
    const uint32_t quads = QuadsPerChunk(desc, lodLevel);
    const uint64_t base =
        static_cast<uint64_t>(quads + 1) * static_cast<uint64_t>(quads + 1);
    if (!stitched || TerrainLodStepQuads(lodLevel) == 1)
        return base;
    // 四条边都按 LOD0 间距补一圈边界环。
    return base + static_cast<uint64_t>(desc.chunkQuads) * 4ull;
}

uint64_t TerrainChunkIndexCount(
    const TerrainAssetDesc& desc,
    uint32_t lodLevel,
    bool stitched)
{
    const uint32_t quads = QuadsPerChunk(desc, lodLevel);
    const uint32_t step = TerrainLodStepQuads(lodLevel);
    const uint64_t full =
        static_cast<uint64_t>(quads) * static_cast<uint64_t>(quads) * 6ull;
    if (!stitched || step == 1)
        return full;
    // 最外一圈格子改由边界带填充：内部 (quads-2)² 个格子，每条边
    // quads×(step+1) 个三角形。
    const uint32_t interior = quads > 1 ? quads - 2 : 0;
    const uint64_t band =
        static_cast<uint64_t>(quads) * (step + 1ull) * 3ull * 4ull;
    return static_cast<uint64_t>(interior) * interior * 6ull + band;
}

uint64_t TerrainChunkResidencyStats::TotalVertexBytes() const
{
    uint64_t total = 0;
    for (const TerrainChunkPlacementStats& placement : placements)
        for (const TerrainChunkLevelStats& level : placement.levels)
            total += level.vertexBytes;
    return total;
}

uint64_t TerrainChunkResidencyStats::TotalIndexBytes() const
{
    uint64_t total = 0;
    for (const TerrainChunkPlacementStats& placement : placements)
        for (const TerrainChunkLevelStats& level : placement.levels)
            total += level.indexBytes;
    return total;
}

uint64_t TerrainChunkResidencyStats::TotalSkippedVertexBytes() const
{
    uint64_t total = 0;
    for (const TerrainChunkPlacementStats& placement : placements)
        for (const TerrainChunkLevelStats& level : placement.levels)
            total += level.skippedVertexBytes;
    return total;
}

uint64_t TerrainChunkResidencyStats::TotalSkippedIndexBytes() const
{
    uint64_t total = 0;
    for (const TerrainChunkPlacementStats& placement : placements)
        for (const TerrainChunkLevelStats& level : placement.levels)
            total += level.skippedIndexBytes;
    return total;
}

uint32_t TerrainChunkResidencyStats::TotalChunks() const
{
    uint32_t total = 0;
    for (const TerrainChunkPlacementStats& placement : placements)
        for (const TerrainChunkLevelStats& level : placement.levels)
            total += level.chunkCount;
    return total;
}

uint32_t TerrainChunkResidencyStats::TotalSkippedChunks() const
{
    uint32_t total = 0;
    for (const TerrainChunkPlacementStats& placement : placements)
        for (const TerrainChunkLevelStats& level : placement.levels)
            total += level.skippedChunkCount;
    return total;
}

void TerrainChunkWorldBounds(
    const Eigen::Matrix4f& model,
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    Eigen::Vector3f& worldMin,
    Eigen::Vector3f& worldMax)
{
    worldMin = Eigen::Vector3f(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    worldMax = Eigen::Vector3f(
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());

    for (float x : {localMin.x(), localMax.x()})
    {
        for (float y : {localMin.y(), localMax.y()})
        {
            for (float z : {localMin.z(), localMax.z()})
            {
                const Eigen::Vector4f corner =
                    model * Eigen::Vector4f(x, y, z, 1.0f);
                const Eigen::Vector3f world = corner.head<3>();
                worldMin = worldMin.cwiseMin(world);
                worldMax = worldMax.cwiseMax(world);
            }
        }
    }
}

bool TerrainChunkLevelIsResident(
    const TerrainChunkResidencyDesc& residency,
    const std::vector<float>& residencyDistancesCm,
    const Eigen::Matrix4f& model,
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    uint32_t lodLevel,
    uint32_t lodLevelCount)
{
    if (!residency.cameraKnown || lodLevelCount <= 1)
        return true;
    if (lodLevel + 1 >= lodLevelCount)
        return true;
    Eigen::Vector3f worldMin;
    Eigen::Vector3f worldMax;
    TerrainChunkWorldBounds(model, localMin, localMax, worldMin, worldMax);
    const float distanceCm = TerrainLodDistanceToAabbCm(
        residency.cameraPositionCm,
        worldMin,
        worldMax);
    return TerrainChunkLodIsResident(
        lodLevel,
        lodLevelCount,
        distanceCm,
        residencyDistancesCm);
}

} // namespace VL
