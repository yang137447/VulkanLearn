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

std::vector<TerrainChunkStreamingCandidate> ComputeTerrainChunkStreamingSet(
    const TerrainChunkResidencyDesc& residency,
    const std::vector<float>& residencyDistancesCm,
    const Eigen::Matrix4f& model,
    const std::vector<TerrainChunkBounds>& chunkBounds,
    uint32_t lodLevelCount,
    float hysteresisScale)
{
    std::vector<TerrainChunkStreamingCandidate> candidates;
    if (!residency.cameraKnown || lodLevelCount == 0 || chunkBounds.empty())
        return candidates;

    const float scale = std::max(hysteresisScale, 1.0f);
    const uint32_t coarsestLevel = lodLevelCount - 1;
    candidates.reserve(
        chunkBounds.size() * static_cast<size_t>(lodLevelCount));
    for (const TerrainChunkBounds& bounds : chunkBounds)
    {
        Eigen::Vector3f worldMin;
        Eigen::Vector3f worldMax;
        TerrainChunkWorldBounds(
            model, bounds.boundsMin, bounds.boundsMax, worldMin, worldMax);
        const float distanceCm = TerrainLodDistanceToAabbCm(
            residency.cameraPositionCm, worldMin, worldMax);
        for (uint32_t lodLevel = 0; lodLevel < lodLevelCount; ++lodLevel)
        {
            bool keep = lodLevel == coarsestLevel;
            if (!keep && lodLevel < residencyDistancesCm.size())
            {
                keep = distanceCm <=
                    residencyDistancesCm[lodLevel] * scale;
            }
            if (!keep)
                continue;
            candidates.push_back(TerrainChunkStreamingCandidate{
                TerrainChunkId{bounds.chunkX, bounds.chunkZ, lodLevel},
                distanceCm});
        }
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const TerrainChunkStreamingCandidate& lhs,
           const TerrainChunkStreamingCandidate& rhs) {
            if (lhs.distanceCm != rhs.distanceCm)
                return lhs.distanceCm < rhs.distanceCm;
            return lhs.id < rhs.id;
        });
    return candidates;
}

bool ParseTerrainChunkLodGroupId(
    const std::string& groupId,
    std::string& placementId,
    uint32_t& chunkX,
    uint32_t& chunkZ)
{
    const size_t last = groupId.rfind('|');
    if (last == std::string::npos || last == 0)
        return false;
    const size_t previous = groupId.rfind('|', last - 1);
    if (previous == std::string::npos || previous == 0)
        return false;
    try
    {
        const std::string chunkXText =
            groupId.substr(previous + 1, last - previous - 1);
        const std::string chunkZText = groupId.substr(last + 1);
        size_t consumedX = 0;
        size_t consumedZ = 0;
        const unsigned long parsedX = std::stoul(chunkXText, &consumedX);
        const unsigned long parsedZ = std::stoul(chunkZText, &consumedZ);
        if (consumedX != chunkXText.size() || consumedZ != chunkZText.size())
            return false;
        if (parsedX > std::numeric_limits<uint32_t>::max() ||
            parsedZ > std::numeric_limits<uint32_t>::max())
            return false;
        placementId = groupId.substr(0, previous);
        chunkX = static_cast<uint32_t>(parsedX);
        chunkZ = static_cast<uint32_t>(parsedZ);
        return !placementId.empty();
    }
    catch (...)
    {
        return false;
    }
}

} // namespace VL
