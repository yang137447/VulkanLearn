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
    bool buildSkirt)
{
    const uint32_t quads = QuadsPerChunk(desc, lodLevel);
    const uint64_t base =
        static_cast<uint64_t>(quads + 1) * static_cast<uint64_t>(quads + 1);
    const uint64_t skirt =
        buildSkirt ? static_cast<uint64_t>(quads) * 4ull : 0ull;
    return base + skirt;
}

uint64_t TerrainChunkIndexCount(
    const TerrainAssetDesc& desc,
    uint32_t lodLevel,
    bool buildSkirt)
{
    const uint32_t quads = QuadsPerChunk(desc, lodLevel);
    const uint64_t base =
        static_cast<uint64_t>(quads) * static_cast<uint64_t>(quads) * 6ull;
    const uint64_t skirt =
        buildSkirt ? static_cast<uint64_t>(quads) * 4ull * 6ull : 0ull;
    return base + skirt;
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
