#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainChunkResidency.h"
#include "terrain/terrainMeshBuilder.h"
#include "vertexDataStruct.h"

using namespace VL;

namespace
{

HostImage MakeHeightmap(
    uint32_t size,
    const std::function<float(uint32_t, uint32_t)>& sample)
{
    HostImage image;
    image.width = size;
    image.height = size;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = size * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * size);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const size_t index =
                (static_cast<size_t>(y) * size + x) * 4;
            pixels[index + 0] = sample(x, y);
            pixels[index + 1] = sample(x, y);
            pixels[index + 2] = sample(x, y);
            pixels[index + 3] = 1.0f;
        }
    }
    return image;
}

float RampHeight(uint32_t x, uint32_t y)
{
    return static_cast<float>(x + y) / 8.0f;
}

TerrainAssetDesc MakeDesc()
{
    TerrainAssetDesc desc;
    desc.assetPath = "Generated/Validation/TerrainTests/TR_chunk_residency.json";
    desc.name = "TR_chunk_residency";
    desc.heightmapPath = "Generated/Validation/TerrainTests/heightmap.png";
    desc.size = Eigen::Vector2f(1000.0f, 1000.0f);
    desc.heightScale = 100.0f;
    desc.gridResolution = 8;
    desc.chunkQuads = 4;
    desc.materialInstancePath =
        "Generated/Validation/TerrainTests/MI_terrain_tests.json";
    return desc;
}

TerrainBuildPlan MakePlan()
{
    TerrainBuildPlan plan;
    plan.desc = MakeDesc();
    plan.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(9, RampHeight));
    return plan;
}

} // namespace

TEST(TerrainChunkResidency, DerivesWindowsFromLodDistances)
{
    TerrainAssetDesc desc = MakeDesc();
    desc.lodLevels = 3;
    desc.lodDistancesCm = {1000.0f, 2000.0f};

    const std::vector<float> windows =
        TerrainChunkResidencyDistancesCm(desc);
    ASSERT_EQ(windows.size(), 2u);
    EXPECT_FLOAT_EQ(
        windows[0],
        1000.0f * (1.0f + TerrainLodHysteresisFraction));
    EXPECT_FLOAT_EQ(
        windows[1],
        2000.0f * (1.0f + TerrainLodHysteresisFraction));

    // 显式窗口优先，且不参与 LOD 选择阈值。
    desc.chunkResidencyDistancesCm = {1500.0f, 4000.0f};
    const std::vector<float> explicitWindows =
        TerrainChunkResidencyDistancesCm(desc);
    ASSERT_EQ(explicitWindows.size(), 2u);
    EXPECT_FLOAT_EQ(explicitWindows[0], 1500.0f);
    EXPECT_FLOAT_EQ(explicitWindows[1], 4000.0f);
}

TEST(TerrainChunkResidency, CoarsestLevelAlwaysResident)
{
    const std::vector<float> windows = {100.0f, 200.0f};
    // 最粗一级（index 2）不受距离限制。
    EXPECT_TRUE(TerrainChunkLodIsResident(2, 3, 1.0e9f, windows));
    // 单级地形同样恒为 resident，即使窗口表为空。
    EXPECT_TRUE(TerrainChunkLodIsResident(0, 1, 1.0e9f, {}));
}

TEST(TerrainChunkResidency, WindowIsInclusiveAndMonotonic)
{
    const std::vector<float> windows = {100.0f, 200.0f};
    EXPECT_TRUE(TerrainChunkLodIsResident(0, 3, 99.0f, windows));
    EXPECT_TRUE(TerrainChunkLodIsResident(0, 3, 100.0f, windows));
    EXPECT_FALSE(TerrainChunkLodIsResident(0, 3, 100.5f, windows));
    EXPECT_FALSE(TerrainChunkLodIsResident(0, 3, 250.0f, windows));
    EXPECT_TRUE(TerrainChunkLodIsResident(1, 3, 200.0f, windows));
    EXPECT_FALSE(TerrainChunkLodIsResident(1, 3, 200.5f, windows));
    // 负距离按 0 处理。
    EXPECT_TRUE(TerrainChunkLodIsResident(0, 3, -50.0f, windows));

    // 表长度不足时按“不驻留”处理，最粗一级仍然驻留。
    EXPECT_FALSE(TerrainChunkLodIsResident(1, 3, 1.0f, {100.0f}));
    EXPECT_TRUE(TerrainChunkLodIsResident(2, 3, 1.0e9f, {100.0f}));
}

TEST(TerrainChunkResidency, UsesWorldAabbDistance)
{
    TerrainChunkResidencyDesc residency;
    residency.cameraKnown = true;
    residency.cameraPositionCm = Eigen::Vector3f(5000.0f, 0.0f, 0.0f);
    const std::vector<float> windows = {1000.0f};
    // 局部 chunk [-100,100]³ 平移 +5000 后与相机重合 -> 距离 0。
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 3) = 5000.0f;
    EXPECT_TRUE(TerrainChunkLevelIsResident(
        residency,
        windows,
        model,
        Eigen::Vector3f(-100.0f, -10.0f, -100.0f),
        Eigen::Vector3f(100.0f, 10.0f, 100.0f),
        0,
        2));

    // 相机移到 +5000 之外仍然用世界空间 AABB：局部块在 -5000..-4800。
    Eigen::Matrix4f farModel = Eigen::Matrix4f::Identity();
    farModel(0, 3) = -5000.0f;
    EXPECT_FALSE(TerrainChunkLevelIsResident(
        residency,
        windows,
        farModel,
        Eigen::Vector3f(-100.0f, -10.0f, -100.0f),
        Eigen::Vector3f(100.0f, 10.0f, 100.0f),
        0,
        2));

    // 相机未知时退回“全部创建”，离线/测试路径不受影响。
    residency.cameraKnown = false;
    EXPECT_TRUE(TerrainChunkLevelIsResident(
        residency,
        windows,
        farModel,
        Eigen::Vector3f(-100.0f, -10.0f, -100.0f),
        Eigen::Vector3f(100.0f, 10.0f, 100.0f),
        0,
        2));
}

TEST(TerrainChunkResidency, StreamingSetKeepsCoarsestAndNearFineLevels)
{
    TerrainChunkResidencyDesc residency;
    residency.cameraKnown = true;
    residency.cameraPositionCm = Eigen::Vector3f::Zero();
    const Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    const std::vector<TerrainChunkBounds> bounds = {
        TerrainChunkBounds{
            0, 0,
            Eigen::Vector3f(-100.0f, 0.0f, -100.0f),
            Eigen::Vector3f(100.0f, 10.0f, 100.0f)},
        TerrainChunkBounds{
            1, 0,
            Eigen::Vector3f(900.0f, 0.0f, -100.0f),
            Eigen::Vector3f(1100.0f, 10.0f, 100.0f)}};
    const std::vector<float> windows = {200.0f, 500.0f};
    const std::vector<TerrainChunkStreamingCandidate> candidates =
        ComputeTerrainChunkStreamingSet(
            residency, windows, model, bounds, 3, 1.0f);

    // 近处 chunk 的三级都在窗口内；远处 chunk 只保留最粗一级。
    ASSERT_EQ(candidates.size(), 4u);
    EXPECT_EQ(candidates[0].id, (TerrainChunkId{0, 0, 0}));
    EXPECT_EQ(candidates[1].id, (TerrainChunkId{0, 0, 1}));
    EXPECT_EQ(candidates[2].id, (TerrainChunkId{0, 0, 2}));
    EXPECT_EQ(candidates[3].id, (TerrainChunkId{1, 0, 2}));
    EXPECT_LE(candidates[0].distanceCm, candidates[1].distanceCm);
    EXPECT_LE(candidates[2].distanceCm, candidates[3].distanceCm);
}

TEST(TerrainChunkResidency, StreamingSetAppliesHysteresis)
{
    TerrainChunkResidencyDesc residency;
    residency.cameraKnown = true;
    residency.cameraPositionCm = Eigen::Vector3f::Zero();
    const Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    const std::vector<TerrainChunkBounds> bounds = {
        TerrainChunkBounds{
            0, 0,
            Eigen::Vector3f(500.0f, 0.0f, -100.0f),
            Eigen::Vector3f(700.0f, 10.0f, 100.0f)}};
    const std::vector<float> windows = {400.0f};

    const std::vector<TerrainChunkStreamingCandidate> exact =
        ComputeTerrainChunkStreamingSet(
            residency, windows, model, bounds, 2, 1.0f);
    ASSERT_EQ(exact.size(), 1u);
    EXPECT_EQ(exact[0].id.lodLevel, 1u);

    const std::vector<TerrainChunkStreamingCandidate> hysteretic =
        ComputeTerrainChunkStreamingSet(
            residency, windows, model, bounds, 2, 1.5f);
    ASSERT_EQ(hysteretic.size(), 2u);
    EXPECT_EQ(hysteretic[0].id.lodLevel, 0u);
    EXPECT_EQ(hysteretic[1].id.lodLevel, 1u);
}

TEST(TerrainChunkResidency, ParsesChunkGroupIdWithPipesInPlacement)
{
    std::string placementId;
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    EXPECT_TRUE(ParseTerrainChunkLodGroupId(
        "terrain|mountain_a|12|7", placementId, chunkX, chunkZ));
    EXPECT_EQ(placementId, "terrain|mountain_a");
    EXPECT_EQ(chunkX, 12u);
    EXPECT_EQ(chunkZ, 7u);

    EXPECT_FALSE(ParseTerrainChunkLodGroupId(
        "terrain|mountain_a|12", placementId, chunkX, chunkZ));
    EXPECT_FALSE(ParseTerrainChunkLodGroupId(
        "terrain|mountain_a|12|x", placementId, chunkX, chunkZ));
    EXPECT_FALSE(ParseTerrainChunkLodGroupId(
        "|12|7", placementId, chunkX, chunkZ));
}

TEST(TerrainChunkResidency, EstimatesMatchBuiltChunkCounts)
{
    const TerrainBuildPlan plan = MakePlan();
    for (uint32_t lodLevel = 0; lodLevel < 3; ++lodLevel)
    {
        const bool stitched = true;
        const std::vector<TerrainMeshChunk> chunks =
            TerrainMeshBuilder::BuildChunks(plan, lodLevel);
        ASSERT_FALSE(chunks.empty());
        for (const TerrainMeshChunk& chunk : chunks)
        {
            EXPECT_EQ(
                chunk.vertices.size(),
                TerrainChunkVertexCount(plan.desc, lodLevel, stitched));
            EXPECT_EQ(
                chunk.indices.size(),
                TerrainChunkIndexCount(plan.desc, lodLevel, stitched));
        }
    }

    // 不带缝合带的路径（LOD0）同样一致。
    const std::vector<TerrainMeshChunk> legacy =
        TerrainMeshBuilder::BuildChunks(plan, 0);
    ASSERT_FALSE(legacy.empty());
    EXPECT_EQ(
        legacy.front().vertices.size(),
        TerrainChunkVertexCount(plan.desc, 0, false));
    EXPECT_EQ(
        legacy.front().indices.size(),
        TerrainChunkIndexCount(plan.desc, 0, false));
}

TEST(TerrainMeshBuilder, ComputedBoundsMatchLod0Geometry)
{
    const TerrainBuildPlan plan = MakePlan();
    const std::vector<TerrainChunkBounds> bounds =
        TerrainMeshBuilder::ComputeChunkBounds(plan);
    const std::vector<TerrainMeshChunk> chunks =
        TerrainMeshBuilder::BuildChunks(plan, 0);
    ASSERT_EQ(bounds.size(), chunks.size());
    for (size_t index = 0; index < bounds.size(); ++index)
    {
        EXPECT_EQ(bounds[index].chunkX, chunks[index].chunkX);
        EXPECT_EQ(bounds[index].chunkZ, chunks[index].chunkZ);
        EXPECT_TRUE(bounds[index].boundsMin.isApprox(
            chunks[index].boundsMin, 1.0e-4f));
        EXPECT_TRUE(bounds[index].boundsMax.isApprox(
            chunks[index].boundsMax, 1.0e-4f));
    }
}

TEST(TerrainMeshBuilder, FilterSkipsChunksWithoutChangingTheRest)
{
    const TerrainBuildPlan plan = MakePlan();
    const std::vector<TerrainMeshChunk> all =
        TerrainMeshBuilder::BuildChunks(plan, 0);
    ASSERT_EQ(all.size(), 4u);

    // 只保留 (1,0)：mask 顺序是 chunkZ 外层、chunkX 内层。
    TerrainChunkBuildFilter filter;
    filter.chunkMask = {0, 1, 0, 0};
    const std::vector<TerrainMeshChunk> filtered =
        TerrainMeshBuilder::BuildChunks(plan, 0, &filter);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].chunkX, 1u);
    EXPECT_EQ(filtered[0].chunkZ, 0u);

    const TerrainMeshChunk& reference = all[1];
    ASSERT_EQ(filtered[0].vertices.size(), reference.vertices.size());
    ASSERT_EQ(filtered[0].indices.size(), reference.indices.size());
    // 逐字段比较：Vertex 含 Eigen 定长类型，整体 memcmp 会把未定义的对齐
    // 填充也计入，无法表达“几何内容相同”。
    for (size_t index = 0; index < reference.vertices.size(); ++index)
    {
        const Vertex& left = filtered[0].vertices[index];
        const Vertex& right = reference.vertices[index];
        EXPECT_TRUE(left.position.isApprox(right.position, 0.0f));
        EXPECT_TRUE(left.normal.isApprox(right.normal, 0.0f));
        EXPECT_TRUE(left.color.isApprox(right.color, 0.0f));
        EXPECT_TRUE(left.texCoord.isApprox(right.texCoord, 0.0f));
        EXPECT_TRUE(left.texCoord1.isApprox(right.texCoord1, 0.0f));
        EXPECT_TRUE(left.tangent.isApprox(right.tangent, 0.0f));
    }
    EXPECT_EQ(
        0,
        std::memcmp(
            filtered[0].indices.data(),
            reference.indices.data(),
            reference.indices.size() * sizeof(uint32_t)));

    // 空 mask 表示全部生成，保持旧调用方行为。
    const TerrainChunkBuildFilter noFilter;
    EXPECT_TRUE(noFilter.Accepts(0, 0, 2));
    EXPECT_EQ(
        TerrainMeshBuilder::BuildChunks(plan, 0, &noFilter).size(),
        4u);
}

TEST(TerrainChunkResidency, StatsAggregateCreatedAndSkippedBytes)
{
    TerrainChunkResidencyStats stats;
    stats.valid = true;
    TerrainChunkPlacementStats placement;
    placement.placementId = "terrain|Terrain";
    placement.chunksPerSide = 2;
    placement.levels.resize(2);
    placement.levels[0].lodLevel = 0;
    placement.levels[0].chunkCount = 1;
    placement.levels[0].skippedChunkCount = 3;
    placement.levels[0].vertexBytes = 100;
    placement.levels[0].indexBytes = 40;
    placement.levels[0].skippedVertexBytes = 300;
    placement.levels[0].skippedIndexBytes = 120;
    placement.levels[1].lodLevel = 1;
    placement.levels[1].chunkCount = 4;
    placement.levels[1].vertexBytes = 80;
    placement.levels[1].indexBytes = 20;
    stats.placements.push_back(std::move(placement));

    EXPECT_EQ(stats.TotalChunks(), 5u);
    EXPECT_EQ(stats.TotalSkippedChunks(), 3u);
    EXPECT_EQ(stats.TotalVertexBytes(), 180u);
    EXPECT_EQ(stats.TotalIndexBytes(), 60u);
    EXPECT_EQ(stats.TotalSkippedVertexBytes(), 300u);
    EXPECT_EQ(stats.TotalSkippedIndexBytes(), 120u);
}
