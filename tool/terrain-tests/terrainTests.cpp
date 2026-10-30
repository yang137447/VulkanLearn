#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "materialInstance.h"
#include "resource/image/hostImage.h"
#include "resource/image/textureIO.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "scene/validation/sceneJsonValidator.h"
#include "terrain/terrainAssetLoader.h"
#include "terrain/terrainFeedback.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainLayerTable.h"
#include "terrain/terrainMeshBuilder.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMap.h"
#include "terrain/validation/terrainAssetValidator.h"

// terrain_tests 不链接 renderer/material 实现；缓存级用例只验证 package 的
// 继承策略，不会构造 MaterialInstance 或 RendererObjectResourceEntry。
// 下面三个符号是 rendererResourceCache.cpp 的链接依赖，用最小替身满足。
MaterialInstanceStateSnapshot MaterialInstance::CaptureStateSnapshot() const
{
    return MaterialInstanceStateSnapshot(std::string(), {}, {});
}

void MaterialInstance::ShutdownRenderResources()
{
}

void VL::RendererObjectResourceEntry::Shutdown()
{
}

namespace
{

HostImage MakeHeightmap(
    uint32_t width,
    uint32_t height,
    const std::function<float(uint32_t, uint32_t)>& value)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float sample = value(x, y);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            pixels[index + 0] = sample;
            pixels[index + 1] = sample;
            pixels[index + 2] = sample;
            pixels[index + 3] = 1.0f;
        }
    }
    return image;
}

VL::TerrainAssetDesc MakeDesc()
{
    VL::TerrainAssetDesc desc;
    desc.assetPath = "Generated/Validation/TerrainTests/TR_terrain_tests.json";
    desc.name = "TR_terrain_tests";
    desc.heightmapPath = "Generated/Validation/TerrainTests/heightmap.png";
    desc.size = Eigen::Vector2f(1000.0f, 1000.0f);
    desc.heightScale = 100.0f;
    desc.heightOffset = 0.0f;
    desc.gridResolution = 4;
    desc.chunkQuads = 2;
    desc.materialInstancePath = "Generated/Validation/TerrainTests/MI_terrain_tests.json";
    return desc;
}

VL::TerrainBuildPlan MakePlan(const HostImage& heightmap)
{
    VL::TerrainBuildPlan plan;
    plan.desc = MakeDesc();
    plan.heightmap = std::make_shared<const HostImage>(heightmap);
    return plan;
}

VL::TerrainBuildPlan MakePagePlan(uint32_t tilesPerSide)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 0.5f; }));
    plan.desc.layers = {
        {"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f},
        {"b", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    plan.desc.pageOrigin = Eigen::Vector2i(0, 0);
    plan.desc.pageTilesPerSide = tilesPerSide;
    return plan;
}

VL::TerrainPageManager::PageKey MakePageKey(
    const std::string& path,
    uint32_t tileX,
    uint32_t tileZ)
{
    VL::TerrainPageManager::PageKey key;
    key.placementId = path;
    key.tileX = tileX;
    key.tileZ = tileZ;
    return key;
}

size_t PageTableEntryOffset(uint32_t virtualPageX, uint32_t virtualPageY)
{
    return (static_cast<size_t>(virtualPageY) * VL::TerrainPageManager::PageTableSize +
        virtualPageX) * 4;
}

size_t AtlasPageByteOffset(uint32_t physicalPageX, uint32_t physicalPageY)
{
    return ((static_cast<size_t>(physicalPageY) * VL::TerrainPageManager::PageResolution) *
                VL::TerrainPageManager::AtlasResolution +
            static_cast<size_t>(physicalPageX) * VL::TerrainPageManager::PageResolution) * 4;
}

size_t TerrainPageByteSize()
{
    return static_cast<size_t>(VL::TerrainPageManager::PageResolution) *
        VL::TerrainPageManager::PageResolution * 4;
}

bool BatchContainsPhysicalSlot(
    const VL::TerrainUploadBatch& batch,
    uint32_t physicalPageX,
    uint32_t physicalPageY)
{
    for (const VL::TerrainPageManager::AtlasPageUpload& upload : batch.atlasUploads)
    {
        if (upload.physicalPageX == physicalPageX &&
            upload.physicalPageY == physicalPageY)
        {
            return true;
        }
    }
    return false;
}

nlohmann::json BuildTerrainJson()
{
    return {
        {"name", "TR_terrain_tests"},
        {"type", "terrain"},
        {"heightmapPath", "Generated/Validation/TerrainTests/heightmap.png"},
        {"size", {1000.0, 1000.0}},
        {"heightScale", 100.0},
        {"heightOffset", 0.0},
        {"gridResolution", 4},
        {"chunkQuads", 2},
        {"materialInstancePath", "Generated/Validation/TerrainTests/MI_terrain_tests.json"},
        {"layers", {
            {{"name", "leaf"}, {"heightRange", {0.0, 1.0}}, {"slopeRange", {0.0, 30.0}}, {"noise", 0.2}},
            {{"name", "rock"}, {"heightRange", {0.2, 1.0}}, {"slopeRange", {25.0, 90.0}}, {"noise", 0.3}}}}};
}

nlohmann::json BuildTerrainSceneJson()
{
    return {
        {"name", "Terrain schema test"},
        {"type", "scene"},
        {"worldUnit", "cm"},
        {"description", "Terrain runtime smoke schema validation."},
        {"objects", {
            {{"name", "Terrain"}, {"type", "terrain"}, {"terrainPath", "Generated/Validation/TerrainTests/TR.json"},
             {"position", {0.0, 0.0, 0.0}}, {"rotation", {0.0, 0.0, 0.0}}, {"scale", {1.0, 1.0, 1.0}}},
            {{"name", "Sun"}, {"type", "directionalLight"}, {"position", {0.0, 100.0, 0.0}},
             {"rotation", {-30.0, 0.0, 0.0}}, {"color", {1.0, 1.0, 1.0}},
             {"intensity", 10000.0}, {"intensityUnit", "lux"}},
            {{"name", "Camera"}, {"type", "camera"}, {"fov", 50.0}, {"near_clip", 1.0}, {"far_clip", 10000.0},
             {"position", {0.0, 100.0, 500.0}}, {"rotation", {0.0, 0.0, 0.0}}, {"scale", {1.0, 1.0, 1.0}},
             {"exposure", {
                 {"mode", "ev100"}, {"ev100", 0.0}, {"aperture", 4.0},
                 {"shutterSeconds", 0.008}, {"iso", 100.0}, {"compensation", 0.0}}}},
        }}};
}

class TerrainTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("vulkanlearn_terrain_" + std::to_string(id));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }

    void TearDown() override
    {
        std::filesystem::remove_all(directory);
    }

    std::filesystem::path directory;
};

} // namespace

TEST(TerrainHeightmap, BilinearSampleUsesAlphaCenteredUv)
{
    HostImage image = MakeHeightmap(2, 2, [](uint32_t x, uint32_t y) {
        return static_cast<float>(y * 2 + x) * 0.25f;
    });
    EXPECT_FLOAT_EQ(VL::TerrainHeightmap::SampleNormalized(image, 0.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(VL::TerrainHeightmap::SampleNormalized(image, 1.0f, 1.0f), 0.75f);
    EXPECT_FLOAT_EQ(VL::TerrainHeightmap::SampleNormalized(image, 0.5f, 0.5f), 0.375f);
}

TEST(TerrainHeightmap, AppliesScaleAndOffset)
{
    HostImage image = MakeHeightmap(2, 2, [](uint32_t, uint32_t) { return 0.5f; });
    VL::TerrainAssetDesc desc = MakeDesc();
    desc.heightScale = 100.0f;
    desc.heightOffset = 25.0f;
    EXPECT_FLOAT_EQ(VL::TerrainHeightmap::SampleHeightCm(desc, image, 0.5f, 0.5f), 75.0f);
}

TEST(TerrainHeightmap, RejectsNonFiniteSamples)
{
    HostImage image = MakeHeightmap(2, 2, [](uint32_t, uint32_t) { return 0.5f; });
    float* pixels = reinterpret_cast<float*>(image.data.data());
    pixels[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(
        VL::TerrainHeightmap::SampleNormalized(image, 0.0f, 0.0f),
        std::exception);
}

TEST(TerrainMeshBuilder, SplitsGridIntoChunksWithExpectedCounts)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t, uint32_t) { return 0.5f; }));
    const std::vector<VL::TerrainMeshChunk> chunks = VL::TerrainMeshBuilder::BuildChunks(plan);
    ASSERT_EQ(chunks.size(), 4u);
    for (const VL::TerrainMeshChunk& chunk : chunks)
    {
        EXPECT_EQ(chunk.vertices.size(), 9u);
        EXPECT_EQ(chunk.indices.size(), 24u);
        for (uint32_t index : chunk.indices)
            EXPECT_LT(index, chunk.vertices.size());
    }
}

TEST(TerrainMeshBuilder, FlatHeightmapProducesUpNormalsAndExpectedPositions)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t, uint32_t) { return 0.5f; }));
    const std::vector<VL::TerrainMeshChunk> chunks = VL::TerrainMeshBuilder::BuildChunks(plan);
    const VL::TerrainMeshChunk& first = chunks.front();
    EXPECT_TRUE(first.boundsMin.isApprox(Eigen::Vector3f(-500.0f, 50.0f, -500.0f)));
    EXPECT_TRUE(first.boundsMax.isApprox(Eigen::Vector3f(0.0f, 50.0f, 0.0f)));
    for (const Vertex& vertex : first.vertices)
    {
        EXPECT_FLOAT_EQ(vertex.position.y(), 50.0f);
        EXPECT_TRUE(vertex.normal.isApprox(Eigen::Vector3f(0.0f, 1.0f, 0.0f)));
    }
}

TEST(TerrainMeshBuilder, SlopeProducesGradientNormal)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t x, uint32_t) {
            return static_cast<float>(x) * 0.25f;
        }));
    const std::vector<VL::TerrainMeshChunk> chunks = VL::TerrainMeshBuilder::BuildChunks(plan);
    // u=0.25 的中心顶点，高度从 0 升到 100，坡度 0.1。
    const Vertex& vertex = chunks.front().vertices[1];
    const Eigen::Vector3f expected =
        Eigen::Vector3f(-0.1f, 1.0f, 0.0f).normalized();
    EXPECT_TRUE(vertex.normal.isApprox(expected, 1.0e-4f));
}

TEST(TerrainMeshBuilder, ChunkEdgeNormalsStayContinuous)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t x, uint32_t y) {
            return static_cast<float>(x + y) * 0.1f;
        }));
    const std::vector<VL::TerrainMeshChunk> chunks = VL::TerrainMeshBuilder::BuildChunks(plan);
    ASSERT_EQ(chunks.size(), 4u);
    // chunks 顺序为 (0,0),(1,0),(0,1),(1,1)；边界 u=0.5 的共享顶点应使用同一全局梯度。
    const Vertex& leftEdge = chunks[0].vertices[2];
    const Vertex& rightEdge = chunks[1].vertices[0];
    EXPECT_TRUE(leftEdge.normal.isApprox(rightEdge.normal, 1.0e-5f));
}

TEST(TerrainAssetTypes, OnlyLayeredTerrainUsesVirtualPages)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t, uint32_t) { return 0.5f; }));
    // 没有 layers 的地形走普通材质，不参与共享 VT 页表。
    EXPECT_FALSE(VL::TerrainUsesVirtualPages(plan));

    plan.desc.layers = {{"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    EXPECT_TRUE(VL::TerrainUsesVirtualPages(plan));
}

TEST(TerrainAssetTypes, FeedbackEligibilityRequiresLayersAndLayerTable)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t, uint32_t) { return 0.5f; }));

    // 两个条件必须同时成立：layerless M_pbr 即使材质意外声明 layerTable，
    // 也不能进入 feedback；反之只有 layers 但材质没绑定层表也不进入。
    EXPECT_FALSE(VL::TerrainParticipatesInFeedback(plan, false));
    EXPECT_FALSE(VL::TerrainParticipatesInFeedback(plan, true));

    plan.desc.layers = {{"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    EXPECT_FALSE(VL::TerrainParticipatesInFeedback(plan, false));
    EXPECT_TRUE(VL::TerrainParticipatesInFeedback(plan, true));
}

TEST(TerrainMeshBuilder, PageUvInsetsOnlyTheGlobalLastColumnAndRow)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(5, 5, [](uint32_t, uint32_t) { return 0.5f; }));
    plan.desc.gridResolution = 4;
    plan.desc.chunkQuads = 2;
    plan.desc.pageOrigin = Eigen::Vector2i(4, 6);
    plan.desc.pageTilesPerSide = 8;

    const std::vector<VL::TerrainMeshChunk> chunks =
        VL::TerrainMeshBuilder::BuildChunks(plan);
    ASSERT_EQ(chunks.size(), 4u);

    const float pageInset =
        0.5f / (static_cast<float>(VL::TerrainPageResolution) *
            static_cast<float>(VL::TerrainPageTableSize));
    // globalX/globalZ=4 是页边界上的最后一列/行，必须内缩半个页 texel。
    const float expectedMaxU = (4.0f + 8.0f) / 32.0f - pageInset;
    const float expectedMaxV = (6.0f + 8.0f) / 32.0f - pageInset;
    EXPECT_NEAR(chunks[1].vertices[2].texCoord1.x(), expectedMaxU, 1.0e-7f);
    EXPECT_NEAR(chunks[2].vertices[6].texCoord1.y(), expectedMaxV, 1.0e-7f);
    EXPECT_NEAR(chunks[3].vertices[8].texCoord1.x(), expectedMaxU, 1.0e-7f);
    EXPECT_NEAR(chunks[3].vertices[8].texCoord1.y(), expectedMaxV, 1.0e-7f);

    // 内部列 globalX=1 / 内部行 globalZ=1 保持精确的页内坐标，不得内缩。
    EXPECT_NEAR(
        chunks[0].vertices[1].texCoord1.x(), (4.0f + 2.0f) / 32.0f, 1.0e-7f);
    EXPECT_NEAR(
        chunks[0].vertices[3].texCoord1.y(), (6.0f + 2.0f) / 32.0f, 1.0e-7f);
}

TEST(TerrainLayerTable, PacksHeightSlopeAndNoiseIntoFourRows)
{
    std::vector<VL::TerrainLayerDesc> layers(8);
    for (uint32_t index = 0; index < 8; ++index)
    {
        layers[index].name = "layer" + std::to_string(index);
        layers[index].heightMin = 0.1f * index;
        layers[index].heightMax = 0.2f + 0.1f * index;
        layers[index].slopeMin = 1.0f * index;
        layers[index].slopeMax = 10.0f + 1.0f * index;
        layers[index].noise = 0.05f * index;
        layers[index].uvScale = 1.0f + 0.1f * index;
        layers[index].uvRotation = 10.0f * index;
    }
    const HostImage image = VL::TerrainLayerTable::BuildImage(layers);
    ASSERT_EQ(image.width, 256u);
    ASSERT_EQ(image.height, 4u);
    ASSERT_EQ(image.format, HostImage::PixelFormat::RGBA32_FLOAT);
    const float* pixels = reinterpret_cast<const float*>(image.data.data());
    const size_t row0 = 3 * 4;
    EXPECT_FLOAT_EQ(pixels[row0 + 0], 0.3f);
    EXPECT_FLOAT_EQ(pixels[row0 + 1], 0.5f);
    EXPECT_FLOAT_EQ(pixels[row0 + 2], 3.0f);
    EXPECT_FLOAT_EQ(pixels[row0 + 3], 13.0f);
    const size_t row1 = static_cast<size_t>(256) * 4 + 3 * 4;
    EXPECT_FLOAT_EQ(pixels[row1 + 0], 0.15f);
    EXPECT_FLOAT_EQ(pixels[row1 + 1], 0.05f);
    EXPECT_FLOAT_EQ(pixels[row1 + 2], 1.3f);
    EXPECT_FLOAT_EQ(pixels[row1 + 3], 30.0f * 3.14159265358979323846f / 180.0f);
    // row2：默认 1K -> 8 页/边；pageTableBase 是前 3 层的 85 页累加。
    const size_t row2 = static_cast<size_t>(256) * 8 + 3 * 4;
    EXPECT_FLOAT_EQ(pixels[row2 + 0], 8.0f);
    EXPECT_FLOAT_EQ(pixels[row2 + 1], 3.0f * 85.0f);
    EXPECT_FLOAT_EQ(pixels[row2 + 2], 0.0f);
    EXPECT_FLOAT_EQ(pixels[row2 + 3], 0.0f);
    // row3：未写 avgColor 时取中性灰 0.18。全黑会让 mode 1 的缺页块
    // 在光照下像空洞，因此默认值必须是中性回退色而不是 0。
    const size_t row3 = static_cast<size_t>(256) * 12 + 3 * 4;
    EXPECT_FLOAT_EQ(pixels[row3 + 0], 0.18f);
    EXPECT_FLOAT_EQ(pixels[row3 + 1], 0.18f);
    EXPECT_FLOAT_EQ(pixels[row3 + 2], 0.18f);
    EXPECT_FLOAT_EQ(pixels[row3 + 3], 0.0f);
}

TEST(TerrainLayerTable, RejectsMoreThanMaxLayers)
{
    std::vector<VL::TerrainLayerDesc> layers(257);
    EXPECT_THROW(VL::TerrainLayerTable::BuildImage(layers), std::exception);
}

TEST(TerrainLayerTable, DetectsLayerDefinitionMismatch)
{
    VL::TerrainLayerDesc layer;
    layer.name = "grass";
    layer.heightMin = 0.1f;
    layer.heightMax = 0.6f;
    layer.slopeMin = 0.0f;
    layer.slopeMax = 30.0f;
    layer.noise = 0.2f;
    const std::vector<VL::TerrainLayerDesc> first{layer};
    EXPECT_TRUE(VL::TerrainLayerTable::AreLayersIdentical(first, first));

    VL::TerrainLayerDesc changed = layer;
    changed.heightMax = 0.7f;
    EXPECT_FALSE(VL::TerrainLayerTable::AreLayersIdentical(first, {changed}));
}

TEST(TerrainWeightMap, PacksTopFourIdsAndNormalizedWeights)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 0.5f; }));
    plan.desc.layers = {
        {"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f},
        {"b", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f},
        {"c", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f},
        {"d", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    const VL::TerrainWeightMap map = VL::TerrainWeightMapBuilder::Build(plan, 8, 2);
    ASSERT_EQ(map.width, 8u);
    ASSERT_EQ(map.height, 8u);
    ASSERT_EQ(map.ids.size(), 8u * 8u * 4u);
    ASSERT_EQ(map.weights.size(), map.ids.size());
    ASSERT_EQ(map.tiles.size(), 4u);
    for (size_t texel = 0; texel < 8u * 8u; ++texel)
    {
        int weightSum = 0;
        for (size_t slot = 0; slot < 4; ++slot)
        {
            EXPECT_LT(map.ids[texel * 4 + slot], 4);
            weightSum += map.weights[texel * 4 + slot];
        }
        EXPECT_GE(weightSum, 250);
    }
}

TEST(TerrainWeightMap, TileCandidatesFollowHeightBands)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(17, 17, [](uint32_t x, uint32_t) {
            return x < 8 ? 0.0f : 1.0f;
        }));
    plan.desc.layers = {
        {"low", 0.0f, 0.4f, 0.0f, 90.0f, 0.0f},
        {"high", 0.6f, 1.0f, 0.0f, 90.0f, 0.0f}};
    const VL::TerrainWeightMap map = VL::TerrainWeightMapBuilder::Build(plan, 8, 2);
    ASSERT_EQ(map.tiles.size(), 4u);
    // 过渡带的 tile 可以同时包含低/高两层；这里只断言每个 tile 至少包含预期层。
    EXPECT_NE(std::find(map.tiles[0].layerIds.begin(), map.tiles[0].layerIds.end(), 0),
        map.tiles[0].layerIds.end());
    EXPECT_NE(std::find(map.tiles[1].layerIds.begin(), map.tiles[1].layerIds.end(), 1),
        map.tiles[1].layerIds.end());
    EXPECT_NE(std::find(map.tiles[2].layerIds.begin(), map.tiles[2].layerIds.end(), 0),
        map.tiles[2].layerIds.end());
    EXPECT_NE(std::find(map.tiles[3].layerIds.begin(), map.tiles[3].layerIds.end(), 1),
        map.tiles[3].layerIds.end());
}

TEST(TerrainWeightMap, PageTableMapsTilesToAtlasPages)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 0.5f; }));
    plan.desc.layers = {
        {"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f},
        {"b", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    const VL::TerrainVirtualTexture vt =
        VL::TerrainWeightMapBuilder::BuildVirtualTexture(plan, 2, 4, 4);
    EXPECT_EQ(vt.pageTableSize, 4u);
    EXPECT_EQ(vt.tilesPerSide, 2u);
    EXPECT_EQ(vt.pageResolution, 4u);
    EXPECT_EQ(vt.atlasResolution, 16u);
    ASSERT_EQ(vt.pageTable.size(), 4u * 4u * 4u);
    EXPECT_EQ(vt.pageTable[0], 0u);
    EXPECT_EQ(vt.pageTable[1], 0u);
    EXPECT_EQ(vt.pageTable[4], 1u);
    EXPECT_EQ(vt.pageTable[5], 0u);
    EXPECT_EQ(vt.pageTable[16], 0u);
    EXPECT_EQ(vt.pageTable[17], 1u);
    EXPECT_EQ(vt.pageTable[20], 1u);
    EXPECT_EQ(vt.pageTable[21], 1u);
    EXPECT_EQ(vt.ids.size(), 16u * 16u * 4u);
    EXPECT_EQ(vt.weights.size(), vt.ids.size());
}

TEST(TerrainWeightMap, BuildPageMatchesVirtualTextureTile)
{
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(17, 17, [](uint32_t x, uint32_t y) {
            return 0.5f + 0.25f * static_cast<float>(x + y) / 32.0f;
        }));
    plan.desc.layers = {
        {"low", 0.0f, 0.55f, 0.0f, 90.0f, 0.1f},
        {"high", 0.5f, 1.0f, 0.0f, 90.0f, 0.1f}};
    plan.desc.pageOrigin = Eigen::Vector2i(0, 0);

    constexpr uint32_t pageResolution = 4;
    const VL::TerrainPageData page =
        VL::TerrainWeightMapBuilder::BuildPage(plan, 2, pageResolution, 1, 1);
    const VL::TerrainVirtualTexture vt =
        VL::TerrainWeightMapBuilder::BuildVirtualTexture(plan, 2, pageResolution, 4);

    ASSERT_EQ(page.ids.size(), pageResolution * pageResolution * 4);
    ASSERT_EQ(page.weights.size(), page.ids.size());
    ASSERT_EQ(vt.tiles.size(), 4u);
    // tile 顺序是 row-major，(1,1) 对应 index 3。
    EXPECT_EQ(page.candidates.layerIds, vt.tiles[3].layerIds);

    const uint32_t globalX = 1;
    const uint32_t globalY = 1;
    for (uint32_t y = 0; y < pageResolution; ++y)
    {
        const size_t pageOffset = static_cast<size_t>(y) * pageResolution * 4;
        const size_t atlasOffset =
            ((static_cast<size_t>(globalY) * pageResolution + y) * vt.atlasResolution +
             static_cast<size_t>(globalX) * pageResolution) * 4;
        EXPECT_EQ(
            std::memcmp(page.ids.data() + pageOffset, vt.ids.data() + atlasOffset,
                static_cast<size_t>(pageResolution) * 4),
            0);
        EXPECT_EQ(
            std::memcmp(page.weights.data() + pageOffset, vt.weights.data() + atlasOffset,
                static_cast<size_t>(pageResolution) * 4),
            0);
    }
}

TEST(TerrainPageManager, AccumulatesNonOverlappingTerrainPages)
{
    VL::TerrainBuildPlan first = MakePlan(
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 0.5f; }));
    first.desc.layers = {
        {"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    first.desc.pageTilesPerSide = 2;
    first.desc.pageOrigin = Eigen::Vector2i(0, 0);

    VL::TerrainBuildPlan second = first;
    second.desc.assetPath = "Generated/Validation/TerrainTests/TR_second.json";
    second.desc.pageOrigin = Eigen::Vector2i(2, 1);

    VL::TerrainPageManager manager;
    manager.RegisterTerrain(first);
    manager.RegisterTerrain(second);
    EXPECT_EQ(manager.Registrations().size(), 2u);
    EXPECT_TRUE(manager.IsResident(0, 0));
    EXPECT_TRUE(manager.IsResident(1, 1));
    EXPECT_TRUE(manager.IsResident(2, 1));
    EXPECT_TRUE(manager.IsResident(3, 2));
    EXPECT_FALSE(manager.IsResident(4, 4));
    // 第二个地形从物理槽位 4 开始（第一个地形占 0..3），
    // 因此虚拟页 (2,1) 的页表项内容是物理坐标 (4,0)，而不是虚拟坐标 (2,1)。
    const size_t pageEntry =
        (static_cast<size_t>(1) * VL::TerrainPageManager::PageTableSize + 2) * 4;
    EXPECT_EQ(manager.PageTable()[pageEntry + 0], 4u);
    EXPECT_EQ(manager.PageTable()[pageEntry + 1], 0u);
    EXPECT_EQ(manager.PageTable()[pageEntry + 3], 255u);
    EXPECT_THROW(manager.RegisterTerrain(first), std::exception);
}

TEST(TerrainPageManager, FallbackKeepsPhysicalSlotButNotVirtualPage)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(8);
    const std::string placement = plan.desc.assetPath;
    // (31,31) 不再被 fallback 预留，因此覆盖最后一格的 8×8 区域可以注册。
    plan.desc.pageOrigin = Eigen::Vector2i(24, 24);
    manager.RegisterTerrain(plan, false);

    const size_t fallbackSlot =
        static_cast<size_t>(VL::TerrainPageManager::FallbackPageY) *
            VL::TerrainPageManager::PageTableSize +
        VL::TerrainPageManager::FallbackPageX;
    // 页表没有 fallback 保留项；虚拟 (31,31) 是普通页。
    EXPECT_EQ(manager.PageTable()[fallbackSlot * 4 + 3], 0u);
    EXPECT_FALSE(manager.IsResident(
        VL::TerrainPageManager::FallbackPageX,
        VL::TerrainPageManager::FallbackPageY));
    // fallback 只占物理 atlas 槽位，因此可用物理槽位仍比总槽位少 1。
    EXPECT_EQ(
        manager.AvailablePhysicalSlotCount(),
        VL::TerrainPageManager::UsablePhysicalSlotCount);

    // 该区域的最后一个 tile 正好落在虚拟 (31,31)，可以正常驻留/淘汰。
    ASSERT_TRUE(manager.RequestPage(placement, 7, 7, 1));
    EXPECT_TRUE(manager.IsResident(31, 31));
    const VL::TerrainPageManager::PageState* page =
        manager.FindPage(MakePageKey(placement, 7, 7));
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->virtualPageX, 31u);
    EXPECT_EQ(page->virtualPageY, 31u);
    // fallback 物理槽位 (31,31) 仍保留给缺页采样，普通页从槽位 0 分配。
    EXPECT_EQ(page->physicalPageX, 0u);
    EXPECT_EQ(page->physicalPageY, 0u);
    ASSERT_TRUE(manager.EvictPage(placement, 7, 7));
    EXPECT_FALSE(manager.IsResident(31, 31));

    // 重叠的虚拟区域仍拒绝，即使它只覆盖 (31,31)。
    VL::TerrainBuildPlan overlapping = plan;
    overlapping.placementId = "terrain|Overlapping";
    overlapping.desc.pageOrigin = Eigen::Vector2i(31, 31);
    overlapping.desc.pageTilesPerSide = 1;
    EXPECT_THROW(manager.RegisterTerrain(overlapping, false), std::exception);
}

TEST(TerrainPageManager, DistinguishesVirtualPageFromPhysicalAtlasSlot)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    plan.desc.pageOrigin = Eigen::Vector2i(5, 3);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 1, 2));

    const VL::TerrainPageManager::PageState* first =
        manager.FindPage(MakePageKey(path, 0, 0));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->virtualPageX, 5u);
    EXPECT_EQ(first->virtualPageY, 3u);
    EXPECT_EQ(first->physicalPageX, 0u);
    EXPECT_EQ(first->physicalPageY, 0u);

    const VL::TerrainPageManager::PageState* second =
        manager.FindPage(MakePageKey(path, 1, 1));
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->virtualPageX, 6u);
    EXPECT_EQ(second->virtualPageY, 4u);
    EXPECT_EQ(second->physicalPageX, 1u);
    EXPECT_EQ(second->physicalPageY, 0u);

    // 页表项写在虚拟坐标 (6,4)，内容是物理槽位 (1,0)。
    const size_t virtualEntry = PageTableEntryOffset(6, 4);
    EXPECT_EQ(manager.PageTable()[virtualEntry + 0], 1u);
    EXPECT_EQ(manager.PageTable()[virtualEntry + 1], 0u);
    EXPECT_EQ(manager.PageTable()[virtualEntry + 3], 255u);

    // atlas 数据只出现在物理槽位 (1,0)，虚拟坐标 (6,4) 处仍为空。
    EXPECT_NE(manager.WeightAtlas()[AtlasPageByteOffset(1, 0)], 0u);
    EXPECT_EQ(manager.WeightAtlas()[AtlasPageByteOffset(6, 4)], 0u);
}

TEST(TerrainPageManager, EvictedPhysicalSlotIsReusedByAnotherVirtualPage)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    plan.desc.pageOrigin = Eigen::Vector2i(9, 9);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 2));
    EXPECT_EQ(
        manager.AvailablePhysicalSlotCount(),
        VL::TerrainPageManager::UsablePhysicalSlotCount - 2);

    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    EXPECT_EQ(
        manager.AvailablePhysicalSlotCount(),
        VL::TerrainPageManager::UsablePhysicalSlotCount - 2);
    // 淘汰后的槽位先进入 quarantine，不能立刻算作可分配空闲槽。
    EXPECT_EQ(manager.QuarantinedPhysicalSlotCount(), 1u);

    // (0,1) 应复用刚释放的物理槽位 0，而不是顺序拿到槽位 2。
    ASSERT_TRUE(manager.RequestPage(path, 0, 1, 3));
    const VL::TerrainPageManager::PageState* reused =
        manager.FindPage(MakePageKey(path, 0, 1));
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->virtualPageX, 9u);
    EXPECT_EQ(reused->virtualPageY, 10u);
    EXPECT_EQ(reused->physicalPageX, 0u);
    EXPECT_EQ(reused->physicalPageY, 0u);
    EXPECT_EQ(
        manager.AvailablePhysicalSlotCount(),
        VL::TerrainPageManager::UsablePhysicalSlotCount - 2);

    // 旧虚拟页失效，新虚拟页映射到物理槽位 0。
    EXPECT_FALSE(manager.IsResident(9, 9));
    EXPECT_TRUE(manager.IsResident(9, 10));
    for (size_t channel = 0; channel < 4; ++channel)
        EXPECT_EQ(manager.PageTable()[PageTableEntryOffset(9, 9) + channel], 0u);
    EXPECT_EQ(manager.PageTable()[PageTableEntryOffset(9, 10) + 0], 0u);
    EXPECT_EQ(manager.PageTable()[PageTableEntryOffset(9, 10) + 1], 0u);
    EXPECT_EQ(manager.PageTable()[PageTableEntryOffset(9, 10) + 3], 255u);
    EXPECT_NE(manager.WeightAtlas()[AtlasPageByteOffset(0, 0)], 0u);
}

TEST(TerrainPageManager, UploadDescriptorsSeparateVirtualAndPhysicalCoordinates)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    plan.desc.pageOrigin = Eigen::Vector2i(5, 3);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    // 非 preload 注册不再产生 fallback 页表项，也没有其他待上传内容。
    EXPECT_FALSE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_TRUE(pageTableUploads.empty());

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 1, 2));
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));

    ASSERT_EQ(pageTableUploads.size(), 2u);
    EXPECT_EQ(pageTableUploads[0].virtualPageX, 5u);
    EXPECT_EQ(pageTableUploads[0].virtualPageY, 3u);
    EXPECT_EQ(pageTableUploads[0].entry[0], 0u);
    EXPECT_EQ(pageTableUploads[0].entry[1], 0u);
    EXPECT_EQ(pageTableUploads[1].virtualPageX, 6u);
    EXPECT_EQ(pageTableUploads[1].virtualPageY, 4u);
    EXPECT_EQ(pageTableUploads[1].entry[0], 1u);
    EXPECT_EQ(pageTableUploads[1].entry[1], 0u);
    EXPECT_EQ(pageTableUploads[1].entry[3], 255u);

    ASSERT_EQ(atlasUploads.size(), 2u);
    EXPECT_EQ(atlasUploads[0].physicalPageX, 0u);
    EXPECT_EQ(atlasUploads[0].physicalPageY, 0u);
    EXPECT_EQ(atlasUploads[1].physicalPageX, 1u);
    EXPECT_EQ(atlasUploads[1].physicalPageY, 0u);
    for (const auto& upload : atlasUploads)
    {
        EXPECT_FALSE(
            upload.physicalPageX == 6u && upload.physicalPageY == 4u);
    }
}

TEST(TerrainPageManager, LoweringResidentCapacityEvictsImmediately)
{
    VL::TerrainPageManager manager;
    manager.SetResidentCapacity(4);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 2));
    ASSERT_TRUE(manager.RequestPage(path, 0, 1, 3));
    ASSERT_TRUE(manager.RequestPage(path, 1, 1, 4));
    ASSERT_EQ(manager.ResidentCount(), 4u);

    manager.SetResidentCapacity(2);
    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_EQ(manager.EvictionCount(), 2u);
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 1, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 1)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 1, 1)));
    // 两个被淘汰的槽位进入 quarantine，free 计数不会立即回升。
    EXPECT_EQ(
        manager.AvailablePhysicalSlotCount(),
        VL::TerrainPageManager::UsablePhysicalSlotCount - 4);
    EXPECT_EQ(manager.QuarantinedPhysicalSlotCount(), 2u);
}

TEST(TerrainPageManager, ResidentCapacityClampsToUsablePhysicalSlots)
{
    VL::TerrainPageManager manager;
    manager.SetResidentCapacity(0);
    EXPECT_EQ(manager.ResidentCapacity(), 1u);
    manager.SetResidentCapacity(VL::TerrainPageManager::PageSlotCount + 100);
    EXPECT_EQ(
        manager.ResidentCapacity(),
        VL::TerrainPageManager::UsablePhysicalSlotCount);
}

TEST(TerrainPageManager, RequestTracksHitAndMissCounts)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    EXPECT_EQ(manager.ResidentCount(), 0u);

    EXPECT_TRUE(manager.RequestPage(path, 0, 0, 10));
    EXPECT_EQ(manager.RequestCount(), 1u);
    EXPECT_EQ(manager.PageMissCount(), 1u);
    EXPECT_EQ(manager.PageHitCount(), 0u);
    EXPECT_EQ(manager.ResidentCount(), 1u);

    EXPECT_TRUE(manager.RequestPage(path, 0, 0, 11));
    EXPECT_EQ(manager.RequestCount(), 2u);
    EXPECT_EQ(manager.PageMissCount(), 1u);
    EXPECT_EQ(manager.PageHitCount(), 1u);
    EXPECT_EQ(manager.ResidentCount(), 1u);

    EXPECT_TRUE(manager.RequestPage(path, 1, 0, 12));
    EXPECT_EQ(manager.RequestCount(), 3u);
    EXPECT_EQ(manager.PageMissCount(), 2u);
    EXPECT_EQ(manager.ResidentCount(), 2u);
}

TEST(TerrainPageManager, EvictionInvalidatesPageTableAndQuarantinesSlot)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));

    const size_t entry = 0; // 全局页表坐标 (0,0)
    EXPECT_EQ(manager.PageTable()[entry + 0], 0u);
    EXPECT_EQ(manager.PageTable()[entry + 1], 0u);
    EXPECT_EQ(manager.PageTable()[entry + 3], 255u);
    EXPECT_NE(manager.WeightAtlas()[0], 0u);

    EXPECT_TRUE(manager.EvictPage(path, 0, 0));
    EXPECT_EQ(manager.ResidentCount(), 0u);
    EXPECT_EQ(manager.EvictionCount(), 1u);
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_FALSE(manager.IsResident(0, 0));
    for (size_t index = 0; index < 4; ++index)
        EXPECT_EQ(manager.PageTable()[entry + index], 0u);
    // 淘汰不清 atlas：旧 in-flight 帧可能仍采样该物理槽位，内容必须保留到
    // quarantine 结束；页表 A=0 已让它对新帧不可达。
    EXPECT_NE(manager.WeightAtlas()[0], 0u);
    EXPECT_EQ(manager.QuarantinedPhysicalSlotCount(), 1u);
    EXPECT_EQ(manager.PhysicalSlotReleaseEpoch(0, 0), 1u);

    EXPECT_FALSE(manager.EvictPage(path, 0, 0));
    EXPECT_EQ(manager.EvictionCount(), 1u);

    // 释放后的槽位可以由同一虚拟页重新占用。
    EXPECT_TRUE(manager.RequestPage(path, 0, 0, 2));
    EXPECT_EQ(manager.ResidentCount(), 1u);
    EXPECT_EQ(manager.PageTable()[entry + 3], 255u);
}

TEST(TerrainPageManager, LeastRecentlyUsedPageIsEvictedAtCapacity)
{
    VL::TerrainPageManager manager;
    manager.SetResidentCapacity(2);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 2));
    // (0,0) 最近被使用，(1,0) 成为 LRU。
    ASSERT_TRUE(manager.MarkUsed(path, 0, 0, 3));
    ASSERT_TRUE(manager.RequestPage(path, 0, 1, 4));

    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_EQ(manager.EvictionCount(), 1u);
    EXPECT_EQ(manager.PageMissCount(), 3u);
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 1)));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 1, 0)));

    const size_t evictedEntry =
        (static_cast<size_t>(0) * VL::TerrainPageManager::PageTableSize + 1) * 4;
    for (size_t index = 0; index < 4; ++index)
        EXPECT_EQ(manager.PageTable()[evictedEntry + index], 0u);
    const size_t liveEntry =
        (static_cast<size_t>(0) * VL::TerrainPageManager::PageTableSize + 0) * 4;
    EXPECT_EQ(manager.PageTable()[liveEntry + 3], 255u);
    EXPECT_EQ(manager.PageTable()[liveEntry + 0], 0u);
    EXPECT_EQ(manager.PageTable()[liveEntry + 1], 0u);
    // (0,1) 复用了被淘汰的 (1,0) 的物理槽位 1，虚拟坐标与物理槽位因此不同。
    const size_t reusedEntry =
        (static_cast<size_t>(1) * VL::TerrainPageManager::PageTableSize + 0) * 4;
    EXPECT_EQ(manager.PageTable()[reusedEntry + 0], 1u);
    EXPECT_EQ(manager.PageTable()[reusedEntry + 1], 0u);
    EXPECT_EQ(manager.PageTable()[reusedEntry + 3], 255u);
}

TEST(TerrainPageManager, PriorityProtectsPageFromLruEviction)
{
    VL::TerrainPageManager manager;
    manager.SetResidentCapacity(2);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 10, 5));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 20, 1));
    // 预算满时优先淘汰优先级最低的 (1,0)，而不是更旧的 (0,0)。
    ASSERT_TRUE(manager.RequestPage(path, 0, 1, 30, 0));

    EXPECT_EQ(manager.EvictionCount(), 1u);
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 1, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 1)));
}

TEST(TerrainPageManager, RepeatedRequestDoesNotAllocateAnotherPage)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 2));
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 3));

    EXPECT_EQ(manager.ResidentCount(), 1u);
    EXPECT_EQ(manager.RequestCount(), 3u);
    EXPECT_EQ(manager.PageMissCount(), 1u);
    EXPECT_EQ(manager.PageHitCount(), 2u);
    EXPECT_EQ(manager.EvictionCount(), 0u);

    // 再请求第二个页时仍有预算，不应触发淘汰。
    ASSERT_TRUE(manager.RequestPage(path, 1, 1, 4));
    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_EQ(manager.EvictionCount(), 0u);
}

TEST(TerrainPageManager, FirstPreloadUploadContainsEveryRegisteredPage)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    manager.RegisterTerrain(plan, true);
    ASSERT_EQ(manager.ResidentCount(), 4u);
    EXPECT_TRUE(manager.HasDirtyUploads());

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_FALSE(manager.HasDirtyUploads());

    // 2×2 地形页：fallback 已与虚拟页表解耦，首次上传只包含真实地形页。
    ASSERT_EQ(pageTableUploads.size(), 4u);
    for (const auto& upload : pageTableUploads)
    {
        const size_t entry =
            PageTableEntryOffset(upload.virtualPageX, upload.virtualPageY);
        for (size_t channel = 0; channel < 4; ++channel)
            EXPECT_EQ(upload.entry[channel], manager.PageTable()[entry + channel]);
        EXPECT_EQ(upload.entry[3], 255u);
        // 上传项必须是真实虚拟页；页表里不存在 fallback 保留项。
        EXPECT_FALSE(
            upload.virtualPageX == VL::TerrainPageManager::FallbackPageX &&
            upload.virtualPageY == VL::TerrainPageManager::FallbackPageY);
    }

    // 每个驻留页各输出一次完整 128×128 RGBA 页，不再整张 atlas 全量上传。
    ASSERT_EQ(atlasUploads.size(), 4u);
    const size_t pageBytes = TerrainPageByteSize();
    const size_t rowBytes = static_cast<size_t>(VL::TerrainPageManager::PageResolution) * 4;
    for (const auto& upload : atlasUploads)
    {
        ASSERT_EQ(upload.ids.size(), pageBytes);
        ASSERT_EQ(upload.weights.size(), pageBytes);
        const size_t atlasOffset =
            AtlasPageByteOffset(upload.physicalPageX, upload.physicalPageY);
        EXPECT_EQ(std::memcmp(
                      upload.ids.data(), manager.IdAtlas().data() + atlasOffset, rowBytes),
            0);
        EXPECT_EQ(std::memcmp(
                      upload.weights.data(), manager.WeightAtlas().data() + atlasOffset, rowBytes),
            0);
    }
}

TEST(TerrainPageManager, RequestUploadsOnlyTheNewPage)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    // 非 preload 注册不再产生 fallback 页表项，也没有 atlas 页。
    EXPECT_FALSE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_TRUE(pageTableUploads.empty());
    EXPECT_TRUE(atlasUploads.empty());

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    // 只上传刚变常驻的一页，不重复输出未变化的页。
    ASSERT_EQ(pageTableUploads.size(), 1u);
    EXPECT_EQ(pageTableUploads[0].virtualPageX, 0u);
    EXPECT_EQ(pageTableUploads[0].virtualPageY, 0u);
    EXPECT_EQ(pageTableUploads[0].entry[3], 255u);
    ASSERT_EQ(atlasUploads.size(), 1u);
    EXPECT_EQ(atlasUploads[0].physicalPageX, 0u);
    EXPECT_EQ(atlasUploads[0].physicalPageY, 0u);
    EXPECT_EQ(atlasUploads[0].ids.size(), TerrainPageByteSize());
    EXPECT_EQ(atlasUploads[0].weights.size(), TerrainPageByteSize());

    // 命中已驻留页不改变 atlas/页表内容，不产生上传。
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 2));
    EXPECT_FALSE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_TRUE(pageTableUploads.empty());
    EXPECT_TRUE(atlasUploads.empty());
}

TEST(TerrainPageManager, ConsumeIsEmptyUntilPageStateChanges)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, true);

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_FALSE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_FALSE(manager.HasDirtyUploads());

    // 使用时间与优先级只影响 CPU 侧淘汰策略，不改变 GPU 内容。
    ASSERT_TRUE(manager.MarkUsed(path, 0, 0, 10));
    ASSERT_TRUE(manager.SetPagePriority(path, 1, 1, 5));
    EXPECT_FALSE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
}

TEST(TerrainPageManager, DirtyRemainsUntilCommit)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    manager.RegisterTerrain(plan, true);

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    ASSERT_TRUE(manager.CollectDirtyUploads(pageTableUploads, atlasUploads));
    const size_t pageTableCount = pageTableUploads.size();
    const size_t atlasCount = atlasUploads.size();
    ASSERT_GT(pageTableCount, 0u);
    ASSERT_GT(atlasCount, 0u);

    // 未 Commit 时重复收集仍返回同一批 dirty 页，上传失败可以安全重试。
    ASSERT_TRUE(manager.CollectDirtyUploads(pageTableUploads, atlasUploads));
    EXPECT_EQ(pageTableUploads.size(), pageTableCount);
    EXPECT_EQ(atlasUploads.size(), atlasCount);

    manager.CommitDirtyUploads(pageTableUploads, atlasUploads);
    EXPECT_FALSE(manager.CollectDirtyUploads(pageTableUploads, atlasUploads));
}

TEST(TerrainPageManager, UploadBatchPinsInvolvedPagesUntilCommit)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, true);

    // 首次上传只包含 2×2 地形页；Build 不直接清 dirty。
    VL::TerrainUploadBatch batch = manager.BuildUploadBatch();
    ASSERT_FALSE(batch.Empty());
    EXPECT_EQ(batch.batchId, 1u);
    EXPECT_EQ(batch.pageTableUploads.size(), 4u);
    EXPECT_EQ(batch.atlasUploads.size(), 4u);
    EXPECT_EQ(batch.newlyPinnedPages.size(), 4u);
    EXPECT_TRUE(manager.HasDirtyUploads());
    for (uint32_t tileZ = 0; tileZ < 2; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < 2; ++tileX)
            EXPECT_TRUE(manager.IsPagePinned(path, tileX, tileZ));
    }

    // 同一份 dirty 再次 Build 会产生第二个独立批次；它必须持有自己的 pin，
    // 否则先结束的批次会把仍在飞行中的页提前解 pin。
    VL::TerrainUploadBatch overlapping = manager.BuildUploadBatch();
    EXPECT_EQ(overlapping.pageTableUploads.size(), batch.pageTableUploads.size());
    EXPECT_EQ(overlapping.atlasUploads.size(), batch.atlasUploads.size());
    EXPECT_EQ(overlapping.newlyPinnedPages.size(), 4u);

    manager.CommitUploadBatch(batch);
    EXPECT_FALSE(manager.HasDirtyUploads());
    for (uint32_t tileZ = 0; tileZ < 2; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < 2; ++tileX)
            EXPECT_TRUE(manager.IsPagePinned(path, tileX, tileZ));
    }

    // overlapping 仍持有第二份 pin；它结束后才真正解 pin。
    manager.AbortUploadBatch(overlapping);
    for (uint32_t tileZ = 0; tileZ < 2; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < 2; ++tileX)
            EXPECT_FALSE(manager.IsPagePinned(path, tileX, tileZ));
    }
}

TEST(TerrainPageManager, UploadBatchAbortKeepsDirtyAndUnpins)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, true);

    VL::TerrainUploadBatch failed = manager.BuildUploadBatch();
    ASSERT_FALSE(failed.Empty());
    manager.AbortUploadBatch(failed);

    // 失败不改变 dirty：下一批仍能拿到同样的内容，并且重新 pin。
    EXPECT_TRUE(manager.HasDirtyUploads());
    for (uint32_t tileZ = 0; tileZ < 2; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < 2; ++tileX)
            EXPECT_FALSE(manager.IsPagePinned(path, tileX, tileZ));
    }

    VL::TerrainUploadBatch retried = manager.BuildUploadBatch();
    EXPECT_EQ(retried.pageTableUploads.size(), failed.pageTableUploads.size());
    EXPECT_EQ(retried.atlasUploads.size(), failed.atlasUploads.size());
    EXPECT_EQ(retried.newlyPinnedPages.size(), 4u);

    manager.CommitUploadBatch(retried);
    EXPECT_FALSE(manager.HasDirtyUploads());
}

TEST(TerrainPageManager, UploadBatchCommitKeepsPageRedirtiedAfterCollection)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    // 非 preload 注册不产生任何待上传内容。
    VL::TerrainUploadBatch initial = manager.BuildUploadBatch();
    EXPECT_TRUE(initial.Empty());
    ASSERT_FALSE(manager.HasDirtyUploads());

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    VL::TerrainUploadBatch resident = manager.BuildUploadBatch();
    ASSERT_FALSE(resident.Empty());
    manager.CommitUploadBatch(resident);
    ASSERT_FALSE(manager.HasDirtyUploads());

    // 淘汰页 (0,0)：页表失效 + atlas 清零进入 dirty，但此时没有常驻页可 pin。
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    VL::TerrainUploadBatch stale = manager.BuildUploadBatch();
    ASSERT_FALSE(stale.Empty());
    EXPECT_TRUE(stale.newlyPinnedPages.empty());

    // 收集后又重新驻留同一页（复用同一物理槽位），revision 前进。
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 2));
    manager.CommitUploadBatch(stale);

    // 旧批次不能把新内容标记成已上传：两个槽位都必须保持 dirty。
    EXPECT_TRUE(manager.HasDirtyUploads());
    VL::TerrainUploadBatch fresh = manager.BuildUploadBatch();
    ASSERT_EQ(fresh.pageTableUploads.size(), 1u);
    EXPECT_EQ(fresh.pageTableUploads[0].virtualPageX, 0u);
    EXPECT_EQ(fresh.pageTableUploads[0].entry[3], 255u);
    ASSERT_EQ(fresh.atlasUploads.size(), 1u);
    EXPECT_EQ(fresh.atlasUploads[0].physicalPageX, 0u);

    manager.CommitUploadBatch(fresh);
    EXPECT_FALSE(manager.HasDirtyUploads());
}

TEST(TerrainPageManager, UploadBatchAtlasLimitTruncatesConsistently)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    manager.RegisterTerrain(plan, true);
    ASSERT_EQ(manager.ResidentCount(), 4u);

    // 只允许本批写 2 个物理 atlas 页；页表必须同步截断为“这 2 页”，
    // 不能先发布指向未上传 atlas 页的有效项。
    VL::TerrainUploadBatch first = manager.BuildUploadBatch(2);
    ASSERT_EQ(first.atlasUploads.size(), 2u);
    ASSERT_EQ(first.pageTableUploads.size(), 2u);
    for (const VL::TerrainPageManager::PageTableUpload& upload :
         first.pageTableUploads)
    {
        ASSERT_NE(upload.entry[3], 0u);
        EXPECT_TRUE(BatchContainsPhysicalSlot(
            first, upload.entry[0], upload.entry[1]));
    }

    manager.CommitUploadBatch(first);
    EXPECT_TRUE(manager.HasDirtyUploads());

    // 剩余 2 个 atlas 页与它们对应的页表项在下一批继续，最终收敛。
    VL::TerrainUploadBatch second = manager.BuildUploadBatch(2);
    ASSERT_EQ(second.atlasUploads.size(), 2u);
    ASSERT_EQ(second.pageTableUploads.size(), 2u);
    for (const VL::TerrainPageManager::PageTableUpload& upload :
         second.pageTableUploads)
    {
        ASSERT_NE(upload.entry[3], 0u);
        EXPECT_TRUE(BatchContainsPhysicalSlot(
            second, upload.entry[0], upload.entry[1]));
    }
    manager.CommitUploadBatch(second);
    EXPECT_FALSE(manager.HasDirtyUploads());
}

TEST(TerrainPageManager, EvictionUploadsOnlyInvalidEntry)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));

    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    // 淘汰只上传页表失效项；atlas 页保持旧内容，由 quarantine 保护到复用。
    ASSERT_EQ(pageTableUploads.size(), 1u);
    EXPECT_EQ(pageTableUploads[0].virtualPageX, 0u);
    EXPECT_EQ(pageTableUploads[0].virtualPageY, 0u);
    for (size_t channel = 0; channel < 4; ++channel)
        EXPECT_EQ(pageTableUploads[0].entry[channel], 0u);

    EXPECT_TRUE(atlasUploads.empty());
}

TEST(TerrainPageManager, ReRequestAfterEvictionUploadsAgain)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));

    // 淘汰保留的旧 atlas 内容必须在重新驻留后被新内容覆盖上传。
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 2));
    ASSERT_TRUE(manager.HasDirtyUploads());
    ASSERT_TRUE(manager.ConsumeDirtyUploads(pageTableUploads, atlasUploads));
    ASSERT_EQ(pageTableUploads.size(), 1u);
    EXPECT_EQ(pageTableUploads[0].entry[3], 255u);
    ASSERT_EQ(atlasUploads.size(), 1u);
    ASSERT_EQ(atlasUploads[0].ids.size(), TerrainPageByteSize());
    EXPECT_NE(atlasUploads[0].weights[0], 0u);
}

TEST(TerrainPageManager, EpochHysteresisDelaysSafeEviction)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.MarkPageUsedAtEpoch(path, 0, 0, 10, 4));

    // 安全窗内不允许淘汰：lastSeenEpoch(10) + 2 > completedEpoch(11)。
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 11));
    EXPECT_FALSE(manager.EvictLeastRecentlyUsed(11));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 0)));

    EXPECT_TRUE(manager.CanEvictAt(path, 0, 0, 12));
    EXPECT_TRUE(manager.EvictLeastRecentlyUsed(12));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 0, 0)));
}

TEST(TerrainPageManager, PinnedPageCannotBeEvicted)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.MarkPageUsedAtEpoch(path, 0, 0, 10));
    ASSERT_TRUE(manager.PinPageForUpload(path, 0, 0));

    EXPECT_TRUE(manager.IsPagePinned(path, 0, 0));
    // 即使已经过了安全窗，pin 的页也拒绝显式和自动淘汰。
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 100));
    EXPECT_FALSE(manager.EvictPage(path, 0, 0));
    EXPECT_FALSE(manager.EvictLeastRecentlyUsed(100));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 0)));

    ASSERT_TRUE(manager.UnpinPage(path, 0, 0));
    EXPECT_TRUE(manager.CanEvictAt(path, 0, 0, 100));
    EXPECT_TRUE(manager.EvictPage(path, 0, 0));
}

TEST(TerrainPageManager, ContentRevisionAdvancesUntilUploadCommits)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));

    const VL::TerrainPageManager::PageState* page =
        manager.FindPageByVirtual(0, 0);
    ASSERT_NE(page, nullptr);
    const uint64_t residentRevision = page->contentRevision;
    EXPECT_GT(residentRevision, 0u);
    EXPECT_LT(page->uploadedRevision, residentRevision);

    std::vector<VL::TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<VL::TerrainPageManager::AtlasPageUpload> atlasUploads;
    ASSERT_TRUE(manager.CollectDirtyUploads(pageTableUploads, atlasUploads));
    manager.CommitDirtyUploads(pageTableUploads, atlasUploads);
    EXPECT_EQ(manager.FindPageByVirtual(0, 0)->uploadedRevision, residentRevision);
    EXPECT_FALSE(manager.HasDirtyUploads());

    // 淘汰同样是一次内容变更，会再次产生待上传版本。
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    EXPECT_GT(manager.FindPage(MakePageKey(path, 0, 0))->contentRevision, residentRevision);
    EXPECT_TRUE(manager.HasDirtyUploads());
}

TEST(TerrainStreamingState, RevisionsAreMonotonicAndMarkUploadedIsIdempotent)
{
    VL::TerrainStreamingState streaming;
    EXPECT_FALSE(streaming.HasPendingUpload());

    streaming.NotifyContentChanged();
    streaming.NotifyContentChanged();
    EXPECT_EQ(streaming.pendingUploadRevision, 2u);
    EXPECT_TRUE(streaming.HasPendingUpload());

    streaming.MarkUploaded(1);
    EXPECT_EQ(streaming.uploadedRevision, 1u);
    EXPECT_TRUE(streaming.HasPendingUpload());

    // 重复确认旧版本不能回退 uploadedRevision。
    streaming.MarkUploaded(1);
    EXPECT_EQ(streaming.uploadedRevision, 1u);

    streaming.MarkUploaded(2);
    EXPECT_EQ(streaming.uploadedRevision, 2u);
    EXPECT_FALSE(streaming.HasPendingUpload());
    streaming.MarkUploaded(1);
    EXPECT_EQ(streaming.uploadedRevision, 2u);
}

TEST(TerrainPageManager, SetGpuEpochContextDoesNotChangeExistingLru)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(50, 40, 2);
    manager.SetResidentCapacity(2);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 2));
    ASSERT_TRUE(manager.MarkUsed(path, 0, 0, 3));
    ASSERT_TRUE(manager.RequestPage(path, 0, 1, 4));

    // RequestPage 仍走旧 LRU；epoch 上下文只供 streaming 安全窗使用。
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 1, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 1)));
    EXPECT_EQ(manager.LastSubmittedEpoch(), 50u);
    EXPECT_EQ(manager.LastConsumedFeedbackEpoch(), 40u);
    EXPECT_EQ(manager.FramesInFlight(), 2u);
}

TEST(TerrainPageManager, NewResidentPageStartsAtConsumedEpoch)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(100, 100, 2);
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    // 新页从当前已消费 feedback 的 epoch 起算，不能在出生当帧或下一帧被淘汰。
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 100));
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 101));
    // framesInFlight=2 时有效安全窗是 max(2, 2+1)=3。
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 102));
    EXPECT_TRUE(manager.CanEvictAt(path, 0, 0, 103));
}

TEST(TerrainPageManager, PlacementIdFallsBackToAssetPath)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    ASSERT_TRUE(plan.placementId.empty());
    manager.RegisterTerrain(plan, false);

    ASSERT_EQ(manager.Registrations().size(), 1u);
    // 旧测试/旧资产不写 placementId 时，注册身份仍回退为 TR 路径。
    EXPECT_EQ(manager.Registrations()[0].placementId, plan.desc.assetPath);
    EXPECT_TRUE(manager.RequestPage(plan.desc.assetPath, 0, 0, 1));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(plan.desc.assetPath, 0, 0)));
}

TEST(TerrainPageManager, SameTerrainCanBeRegisteredTwiceWithDifferentPlacements)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan first = MakePagePlan(2);
    first.placementId = "terrain|TerrainA";
    first.desc.pageOrigin = Eigen::Vector2i(0, 0);

    VL::TerrainBuildPlan second = first;
    second.placementId = "terrain|TerrainB";
    second.desc.pageOrigin = Eigen::Vector2i(8, 0);

    manager.RegisterTerrain(first, false);
    manager.RegisterTerrain(second, false);
    ASSERT_EQ(manager.Registrations().size(), 2u);
    EXPECT_EQ(manager.Registrations()[0].terrainAssetPath, first.desc.assetPath);
    EXPECT_EQ(manager.Registrations()[1].terrainAssetPath, first.desc.assetPath);

    ASSERT_TRUE(manager.RequestPage("terrain|TerrainA", 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage("terrain|TerrainB", 0, 0, 2));
    const VL::TerrainPageManager::PageState* pageA =
        manager.FindPage(MakePageKey("terrain|TerrainA", 0, 0));
    const VL::TerrainPageManager::PageState* pageB =
        manager.FindPage(MakePageKey("terrain|TerrainB", 0, 0));
    ASSERT_NE(pageA, nullptr);
    ASSERT_NE(pageB, nullptr);
    EXPECT_EQ(pageA->virtualPageX, 0u);
    EXPECT_EQ(pageB->virtualPageX, 8u);
    // 两个 placement 各有独立物理槽位，页内容相同但生命周期互不影响。
    EXPECT_EQ(pageA->physicalPageX, 0u);
    EXPECT_EQ(pageB->physicalPageX, 1u);

    ASSERT_TRUE(manager.EvictPage("terrain|TerrainA", 0, 0));
    EXPECT_FALSE(manager.IsResident(0, 0));
    EXPECT_TRUE(manager.IsResident(8, 0));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey("terrain|TerrainB", 0, 0)));

    // 同一 placementId 重复注册仍被拒绝，即使换了 pageOrigin。
    VL::TerrainBuildPlan duplicate = second;
    duplicate.desc.pageOrigin = Eigen::Vector2i(16, 0);
    EXPECT_THROW(manager.RegisterTerrain(duplicate, false), std::exception);
}

TEST(TerrainPageManager, FullPageTableRegistersAndUsesLastVirtualPage)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(VL::TerrainPageManager::PageTableSize);
    const std::string placement = plan.desc.assetPath;
    // 32×32 覆盖整张页表；fallback 不再占虚拟坐标，注册必须成功。
    manager.RegisterTerrain(plan, false);

    ASSERT_EQ(manager.Registrations().size(), 1u);
    EXPECT_EQ(
        manager.Pages().size(),
        static_cast<size_t>(VL::TerrainPageManager::PageSlotCount));

    // 最后一格虚拟 (31,31) 是普通页，可以正常驻留并映射到物理槽位。
    ASSERT_TRUE(manager.RequestPage(placement, 31, 31, 1));
    EXPECT_TRUE(manager.IsResident(31, 31));
    const VL::TerrainPageManager::PageState* last =
        manager.FindPage(MakePageKey(placement, 31, 31));
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->virtualPageX, 31u);
    EXPECT_EQ(last->virtualPageY, 31u);
    EXPECT_EQ(last->physicalPageX, 0u);
    EXPECT_EQ(last->physicalPageY, 0u);
}

TEST(TerrainPageManager, PreloadDefersInsteadOfLoopingWhenCapacityCannotEvict)
{
    VL::TerrainPageManager manager;
    manager.SetResidentCapacity(1);

    VL::TerrainBuildPlan held = MakePagePlan(1);
    held.placementId = "terrain|Held";
    const std::string heldPlacement = held.placementId;
    manager.RegisterTerrain(held, false);
    ASSERT_TRUE(manager.RequestPage(heldPlacement, 0, 0, 1));
    ASSERT_TRUE(manager.PinPageForUpload(heldPlacement, 0, 0));

    VL::TerrainBuildPlan blocked = MakePagePlan(1);
    blocked.placementId = "terrain|Blocked";
    blocked.desc.pageOrigin = Eigen::Vector2i(4, 0);
    // 唯一的驻留候选被 pin，preload 无法腾位；必须记录 deferred 后返回，
    // 不能无限重试，也不能把异常抛到注册调用之外。
    EXPECT_NO_THROW(manager.RegisterTerrain(blocked, true));
    EXPECT_EQ(manager.PreloadDeferredCount(), 1u);
    EXPECT_EQ(manager.ResidentCount(), 1u);
    EXPECT_LE(
        manager.ResidentCount(),
        VL::TerrainPageManager::UsablePhysicalSlotCount);
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(heldPlacement, 0, 0)));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(blocked.placementId, 0, 0)));
}

TEST(TerrainMeshBuilder, ChunkResourceKeyIncludesPlacementIdentity)
{
    const HostImage heightmap =
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 0.5f; });
    VL::TerrainBuildPlan first = MakePlan(heightmap);
    first.placementId = "terrain|TerrainA";
    VL::TerrainBuildPlan second = first;
    second.placementId = "terrain|TerrainB";

    // 同一 TR 的两次摆放不能共享 chunk 资源 key。
    EXPECT_NE(
        VL::TerrainChunkResourceKey(first, 0, 0),
        VL::TerrainChunkResourceKey(second, 0, 0));
    EXPECT_NE(
        VL::TerrainChunkResourceKey(first, 0, 0),
        VL::TerrainChunkResourceKey(first, 1, 0));
    EXPECT_EQ(
        VL::TerrainChunkResourceKey(first, 0, 0),
        VL::TerrainChunkResourceKey(first, 0, 0));

    // placementId 为空时回退 assetPath，与旧资产 key 语义一致。
    VL::TerrainBuildPlan legacy = first;
    legacy.placementId.clear();
    EXPECT_EQ(
        legacy.desc.assetPath,
        VL::TerrainPlacementIdentity(legacy));
    EXPECT_NE(
        VL::TerrainChunkResourceKey(legacy, 0, 0),
        VL::TerrainChunkResourceKey(first, 0, 0));
}

TEST(TerrainValidator, AcceptsCurrentTerrainSchema)
{
    EXPECT_NO_THROW(VL::TerrainAssetValidator::Validate(
        BuildTerrainJson(), "TR_terrain_tests.json"));
}

TEST(TerrainValidator, RejectsUnknownField)
{
    nlohmann::json asset = BuildTerrainJson();
    asset["unknown"] = true;
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);
}

TEST(TerrainValidator, RejectsGridThatIsNotDivisibleByChunk)
{
    nlohmann::json asset = BuildTerrainJson();
    asset["gridResolution"] = 5;
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);
}

TEST(TerrainValidator, RejectsInvalidLayerRanges)
{
    nlohmann::json asset = BuildTerrainJson();
    asset["layers"][0]["heightRange"] = {0.5, 0.1};
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);

    asset = BuildTerrainJson();
    asset["layers"][0]["slopeRange"] = {0.0, 91.0};
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);

    asset = BuildTerrainJson();
    asset["layers"][0]["noise"] = -1.0;
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);
}

TEST(TerrainValidator, RejectsPageOriginOutsideGlobalTable)
{
    nlohmann::json asset = BuildTerrainJson();
    asset["pageOrigin"] = {30, 0};
    asset["pageTilesPerSide"] = 8;
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);

    asset = BuildTerrainJson();
    asset["pageOrigin"] = {8, 8};
    asset["pageTilesPerSide"] = 8;
    EXPECT_NO_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"));
}

TEST(TerrainValidator, RejectsMoreThanMaxLayers)
{
    nlohmann::json asset = BuildTerrainJson();
    asset["layers"] = nlohmann::json::array();
    for (int index = 0; index < 257; ++index)
    {
        asset["layers"].push_back({
            {"name", "layer" + std::to_string(index)},
            {"heightRange", {0.0, 1.0}},
            {"slopeRange", {0.0, 90.0}}});
    }
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(asset, "TR_terrain_tests.json"), std::exception);
}

TEST(TerrainValidator, AcceptsOptionalLayerCatalogFields)
{
    nlohmann::json asset = BuildTerrainJson();
    asset["layers"][0]["sourceResolution"] = 2048;
    asset["layers"][0]["avgColor"] = {0.25, 0.5, 0.75};
    asset["layers"][0]["sourceSet"] = 3;
    asset["layers"][0]["flags"] = 5;
    EXPECT_NO_THROW(VL::TerrainAssetValidator::Validate(
        asset, "TR_terrain_tests.json"));

    // 三档源分辨率都必须接受。
    for (const uint32_t resolution : {512u, 1024u, 2048u})
    {
        nlohmann::json probe = BuildTerrainJson();
        probe["layers"][0]["sourceResolution"] = resolution;
        EXPECT_NO_THROW(VL::TerrainAssetValidator::Validate(
            probe, "TR_terrain_tests.json"));
    }
}

TEST(TerrainValidator, RejectsInvalidOptionalLayerCatalogFields)
{
    // 只允许 512/1024/2048：256 太小、4096 超出设计档位、1536 不是 128 的整数页。
    for (const int resolution : {256, 1536, 4096})
    {
        nlohmann::json asset = BuildTerrainJson();
        asset["layers"][0]["sourceResolution"] = resolution;
        EXPECT_THROW(VL::TerrainAssetValidator::Validate(
            asset, "TR_terrain_tests.json"), std::exception);
    }

    nlohmann::json asset = BuildTerrainJson();
    asset["layers"][0]["avgColor"] = {0.5, 0.5};
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(
        asset, "TR_terrain_tests.json"), std::exception);
    asset = BuildTerrainJson();
    asset["layers"][0]["avgColor"] = {0.5, 0.5, 1.5};
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(
        asset, "TR_terrain_tests.json"), std::exception);

    asset = BuildTerrainJson();
    asset["layers"][0]["sourceSet"] = 256;
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(
        asset, "TR_terrain_tests.json"), std::exception);
    asset = BuildTerrainJson();
    asset["layers"][0]["flags"] = 65536;
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(
        asset, "TR_terrain_tests.json"), std::exception);

    asset = BuildTerrainJson();
    asset["layers"][0]["compression"] = "bc7";
    EXPECT_THROW(VL::TerrainAssetValidator::Validate(
        asset, "TR_terrain_tests.json"), std::exception);
}

TEST_F(TerrainTest, LoadsDescriptorAndDecodesHeightmap)
{
    HostImage heightmap;
    heightmap.width = 2;
    heightmap.height = 2;
    heightmap.channels = 4;
    heightmap.format = HostImage::PixelFormat::RGBA16_FLOAT;
    heightmap.semantic = HostImage::TextureSemantic::Data;
    heightmap.rowStrideBytes = 16;
    const uint16_t halfPixels[4][4] = {
        {0x0000, 0x0000, 0x0000, 0x3C00},
        {0x3400, 0x3400, 0x3400, 0x3C00},
        {0x3800, 0x3800, 0x3800, 0x3C00},
        {0x3C00, 0x3C00, 0x3C00, 0x3C00}};
    heightmap.data.resize(sizeof(halfPixels));
    std::memcpy(heightmap.data.data(), halfPixels, sizeof(halfPixels));
    const auto heightmapPath = directory / "heightmap.exr";
    TextureIO::SaveOptions saveOptions;
    saveOptions.format = TextureIO::FileFormat::Exr;
    ASSERT_TRUE(TextureIO::Save(heightmapPath, heightmap, saveOptions));

    const auto materialPath = directory / "MI_terrain_tests.json";
    std::ofstream materialStream(materialPath);
    materialStream << nlohmann::json({
        {"name", "MI_terrain_tests"},
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_unlit.json"}}).dump(2);
    materialStream.close();

    nlohmann::json asset = BuildTerrainJson();
    asset["layers"][0]["sourceResolution"] = 2048;
    asset["layers"][0]["avgColor"] = {0.25, 0.5, 0.75};
    asset["layers"][0]["sourceSet"] = 3;
    asset["layers"][0]["flags"] = 9;
    asset["heightmapPath"] = heightmapPath.string();
    asset["materialInstancePath"] = materialPath.string();
    asset["gridResolution"] = 2;
    asset["chunkQuads"] = 1;
    const auto terrainPath = directory / "TR_terrain_tests.json";
    std::ofstream terrainStream(terrainPath);
    terrainStream << asset.dump(2);
    terrainStream.close();

    const VL::TerrainBuildPlan plan = VL::TerrainAssetLoader::Prepare(terrainPath.string());
    EXPECT_EQ(plan.desc.name, "TR_terrain_tests");
    EXPECT_EQ(plan.desc.gridResolution, 2u);
    EXPECT_EQ(plan.desc.chunkQuads, 1u);
    ASSERT_EQ(plan.desc.layers.size(), 2u);
    // L1 可选字段往返；第二层没写时保持默认值，证明旧 TR 兼容。
    EXPECT_EQ(plan.desc.layers[0].sourceResolution, 2048u);
    EXPECT_EQ(plan.desc.layers[0].sourceSet, 3u);
    EXPECT_EQ(plan.desc.layers[0].flags, 9u);
    EXPECT_FLOAT_EQ(plan.desc.layers[0].avgColor.x(), 0.25f);
    EXPECT_FLOAT_EQ(plan.desc.layers[0].avgColor.y(), 0.5f);
    EXPECT_FLOAT_EQ(plan.desc.layers[0].avgColor.z(), 0.75f);
    EXPECT_EQ(plan.desc.layers[1].sourceResolution, 1024u);
    EXPECT_EQ(plan.desc.layers[1].sourceSet, 0u);
    EXPECT_EQ(plan.desc.layers[1].flags, 0u);
    EXPECT_FLOAT_EQ(plan.desc.layers[1].avgColor.x(), 0.18f);
    ASSERT_TRUE(plan.heightmap != nullptr);
    EXPECT_EQ(plan.heightmap->width, 2u);
    EXPECT_EQ(plan.heightmap->height, 2u);
    EXPECT_EQ(plan.heightmap->format, HostImage::PixelFormat::RGBA32_FLOAT);
}

TEST(TerrainSceneSchema, AcceptsTerrainSceneObject)
{
    const nlohmann::json scene = BuildTerrainSceneJson();
    EXPECT_NO_THROW(VL::ValidateSceneAssetJson(scene));
}

TEST(TerrainSceneSchema, AcceptsPageRegionOverrides)
{
    nlohmann::json scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageOrigin"] = {24, 24};
    scene["objects"][0]["pageTilesPerSide"] = 8;
    EXPECT_NO_THROW(VL::ValidateSceneAssetJson(scene));

    // 只覆盖 pageTilesPerSide 也合法；与 TR pageOrigin 的组合由页管理器最终裁定。
    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageTilesPerSide"] = 32;
    EXPECT_NO_THROW(VL::ValidateSceneAssetJson(scene));
}

TEST(TerrainSceneSchema, RejectsInvalidPageRegionOverrides)
{
    nlohmann::json scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageOrigin"] = {-1, 0};
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);

    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageOrigin"] = {32, 0};
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);

    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageOrigin"] = {0, 0, 0};
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);

    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageOrigin"] = {0.5, 0};
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);

    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageTilesPerSide"] = 0;
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);

    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageTilesPerSide"] = 33;
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);

    // pageOrigin + pageTilesPerSide 必须整体落在 32×32 页表内。
    scene = BuildTerrainSceneJson();
    scene["objects"][0]["pageOrigin"] = {24, 24};
    scene["objects"][0]["pageTilesPerSide"] = 9;
    EXPECT_THROW(VL::ValidateSceneAssetJson(scene), std::invalid_argument);
}

TEST(TerrainFeedback, TexelPackingRoundTripsAllFields)
{
    VL::TerrainFeedbackTexel source;
    source.virtualPageX = 17;
    source.virtualPageY = 29;
    source.priority = 203;
    source.frameTag = 77;
    source.miss = true;
    source.valid = true;

    uint8_t bytes[VL::TerrainFeedbackTexelBytes] = {};
    VL::EncodeTerrainFeedbackTexel(source, bytes);
    const VL::TerrainFeedbackTexel decoded =
        VL::DecodeTerrainFeedbackTexel(bytes);

    EXPECT_EQ(decoded.virtualPageX, 17u);
    EXPECT_EQ(decoded.virtualPageY, 29u);
    EXPECT_EQ(decoded.priority, 203u);
    EXPECT_EQ(decoded.frameTag, 77u);
    EXPECT_TRUE(decoded.miss);
    EXPECT_TRUE(decoded.valid);
    // R 的 bit5/bit6 与页坐标互不重叠。
    EXPECT_EQ(bytes[0], 17u | 0x20u | 0x40u);
    EXPECT_EQ(bytes[1], 29u);
}

TEST(TerrainFeedback, FrameTagIsOneBasedAndWrapsBefore255)
{
    EXPECT_EQ(VL::TerrainFeedbackFrameTagForEpoch(0), 1u);
    EXPECT_EQ(VL::TerrainFeedbackFrameTagForEpoch(254), 255u);
    EXPECT_EQ(VL::TerrainFeedbackFrameTagForEpoch(255), 1u);
    EXPECT_EQ(VL::TerrainFeedbackFrameTagForEpoch(256), 2u);
}

TEST(TerrainFeedback, SummarySkipsInvalidAndStaleTexels)
{
    std::vector<uint8_t> texels(7 * VL::TerrainFeedbackTexelBytes, 0);
    const auto write = [&](size_t index, const VL::TerrainFeedbackTexel& texel)
    {
        VL::EncodeTerrainFeedbackTexel(
            texel,
            texels.data() + index * VL::TerrainFeedbackTexelBytes);
    };

    // 0：clear 值（valid=0）里的非零 page 坐标必须被忽略。
    write(0, VL::TerrainFeedbackTexel{
        19, 19, 51, 99, false, false });
    // 1：有效但帧 tag 过期。
    write(1, VL::TerrainFeedbackTexel{
        4, 5, 200, 76, false, true });
    // 2/3：同一页重复命中，miss 只要出现一次就记 miss 页。
    write(2, VL::TerrainFeedbackTexel{
        4, 5, 80, 77, false, true });
    write(3, VL::TerrainFeedbackTexel{
        4, 5, 190, 77, true, true });
    // 4：另一页，无 miss。
    write(4, VL::TerrainFeedbackTexel{
        6, 7, 120, 77, false, true });
    // 5：同样有效但 tag 不匹配。
    write(5, VL::TerrainFeedbackTexel{
        8, 9, 255, 1, true, true });
    // 6：未写入。
    write(6, VL::TerrainFeedbackTexel{});

    const VL::TerrainFeedbackFrameSummary summary =
        VL::SummarizeTerrainFeedback(
            texels.data(),
            texels.size(),
            77);

    EXPECT_EQ(summary.frameTag, 77u);
    EXPECT_EQ(summary.validTexelCount, 3u);
    EXPECT_EQ(summary.invalidTexelCount, 2u);
    EXPECT_EQ(summary.staleTexelCount, 2u);
    EXPECT_EQ(summary.distinctPageCount, 2u);
    EXPECT_EQ(summary.missPageCount, 1u);
    EXPECT_EQ(summary.maxPriority, 190u);
    ASSERT_EQ(summary.pages.size(), 2u);
    EXPECT_EQ(summary.pages[0].virtualPageX, 4u);
    EXPECT_EQ(summary.pages[0].virtualPageY, 5u);
    EXPECT_TRUE(summary.pages[0].miss);
    EXPECT_EQ(summary.pages[0].sampleCount, 2u);
    EXPECT_EQ(summary.pages[1].virtualPageX, 6u);
    EXPECT_EQ(summary.pages[1].virtualPageY, 7u);
    EXPECT_FALSE(summary.pages[1].miss);
}

TEST(TerrainFeedback, RingWaitsForCompletedEpochThenConsumesOnce)
{
    VL::TerrainFeedbackRingState ring(2);
    ring.Record(0, 9, 10);
    EXPECT_TRUE(ring.IsPending(0));

    // 旧 epoch 未完成：不得解析。
    EXPECT_FALSE(ring.TryConsume(0, 8).has_value());
    EXPECT_TRUE(ring.IsPending(0));

    const auto completed = ring.TryConsume(0, 9);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->copiedEpoch, 9u);
    EXPECT_EQ(completed->frameTag, 10u);
    EXPECT_FALSE(ring.IsPending(0));
    // 完成后只能消费一次。
    EXPECT_FALSE(ring.TryConsume(0, 9).has_value());

    // 槽位可复用；新 epoch 继续等待自己的 fence。
    ring.Record(0, 11, 12);
    EXPECT_TRUE(ring.IsPending(0));
    EXPECT_FALSE(ring.TryConsume(0, 10).has_value());
    ASSERT_TRUE(ring.TryConsume(0, 11).has_value());
}

TEST(TerrainFeedback, RingReportsPendingTagRangeAndRejectsZeroTag)
{
    VL::TerrainFeedbackRingState ring(2);
    uint8_t oldest = 0;
    uint8_t newest = 0;
    EXPECT_FALSE(ring.PendingTagRange(oldest, newest));

    ring.Record(0, 3, 4);
    ring.Record(1, 4, 9);
    ASSERT_TRUE(ring.PendingTagRange(oldest, newest));
    EXPECT_EQ(oldest, 4u);
    EXPECT_EQ(newest, 9u);

    EXPECT_THROW(ring.Record(0, 5, 0), std::invalid_argument);
    EXPECT_THROW(ring.Record(2, 5, 3), std::out_of_range);
}

TEST(TerrainPageManager, NewResidentPageStartsAtSubmittedEpoch)
{
    VL::TerrainPageManager manager;
    // 本帧已提交 120、最近消费的 feedback 仍是 100：新页必须从 120 起算，
    // 否则它一出生就落在安全窗之外。
    manager.SetGpuEpochContext(120, 100, 2);
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 121));
    EXPECT_FALSE(manager.CanEvictAt(path, 0, 0, 122));
    EXPECT_TRUE(manager.CanEvictAt(path, 0, 0, 123));
}

TEST(TerrainPageManager, RequestPageAtEpochWaitsForSafeEvictionWindow)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(20, 18, 2);
    manager.SetResidentCapacity(1);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    // 页 (0,0) 在 epoch 20 出生；容量已满，安全窗内必须拒绝腾位。
    EXPECT_FALSE(manager.RequestPageAtEpoch(path, 1, 0, 2, 0, 21));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 1, 0)));
    EXPECT_EQ(manager.EvictionCount(), 0u);

    // framesInFlight=2 时安全窗为 3；epoch 22 仍在窗口内，epoch 23 才允许淘汰。
    EXPECT_FALSE(manager.RequestPageAtEpoch(path, 1, 0, 3, 0, 22));
    EXPECT_TRUE(manager.RequestPageAtEpoch(path, 1, 0, 3, 0, 23));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 1, 0)));
    EXPECT_EQ(manager.EvictionCount(), 1u);
}

TEST(TerrainPageManager, LegacyRequestPageKeepsUnwindowedLru)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(20, 18, 2);
    manager.SetResidentCapacity(1);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    // 旧 RequestPage 是 preload/加载期入口，仍走无安全窗 LRU；
    // streaming 的安全窗只由 RequestPageAtEpoch 消费。
    EXPECT_TRUE(manager.RequestPage(path, 1, 0, 2));
    EXPECT_FALSE(manager.IsPageResident(MakePageKey(path, 0, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 1, 0)));
}

TEST(TerrainPageManager, OverlappingBatchesHoldIndependentPinsUntilBothRelease)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, true);

    VL::TerrainUploadBatch first = manager.BuildUploadBatch();
    ASSERT_EQ(first.newlyPinnedPages.size(), 1u);
    ASSERT_TRUE(manager.IsPagePinned(path, 0, 0));
    VL::TerrainUploadBatch second = manager.BuildUploadBatch();
    ASSERT_EQ(second.newlyPinnedPages.size(), 1u);

    // 先结束第二批：第一批仍持有 pin，页不能提前解 pin。
    manager.CommitUploadBatch(second);
    EXPECT_TRUE(manager.IsPagePinned(path, 0, 0));
    manager.AbortUploadBatch(first);
    EXPECT_FALSE(manager.IsPagePinned(path, 0, 0));
}

TEST(TerrainPageManager, AbortedBatchDoesNotReleaseAnotherBatchPin)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, true);

    VL::TerrainUploadBatch first = manager.BuildUploadBatch();
    VL::TerrainUploadBatch second = manager.BuildUploadBatch();
    ASSERT_EQ(first.newlyPinnedPages.size(), 1u);
    ASSERT_EQ(second.newlyPinnedPages.size(), 1u);
    ASSERT_TRUE(manager.IsPagePinned(path, 0, 0));

    manager.AbortUploadBatch(first);
    EXPECT_TRUE(manager.IsPagePinned(path, 0, 0));
    manager.CommitUploadBatch(second);
    EXPECT_FALSE(manager.IsPagePinned(path, 0, 0));
}

TEST(TerrainWeightMap, ZeroHeightScaleUsesMidHeightInsteadOfThrowing)
{
    VL::TerrainBuildPlan plan = MakePagePlan(1);
    plan.desc.heightScale = 0.0f;
    plan.desc.heightOffset = 1234.0f;
    plan.desc.layers = {
        {"low", 0.0f, 0.4f, 0.0f, 90.0f, 0.0f},
        {"mid", 0.45f, 0.55f, 0.0f, 90.0f, 0.0f},
        {"high", 0.6f, 1.0f, 0.0f, 90.0f, 0.0f}};

    VL::TerrainPageData page;
    ASSERT_NO_THROW(
        page = VL::TerrainWeightMapBuilder::BuildPage(plan, 1, 32, 0, 0));
    // 平地归一化高度固定 0.5，正好落在 mid 层窗口内。
    EXPECT_EQ(page.ids[0], 1u);
    EXPECT_EQ(page.weights[0], 255u);
}

TEST(TerrainWeightMap, NegativeHeightScaleClampsNormalizedHeight)
{
    // float 高度图允许样本超出 [0,1]；负 heightScale 让几何翻转，但归一化
    // 高度仍是样本值 (height-offset)/scale == sample，必须钳制到 [0,1]，
    // 否则所有层窗口都可能不命中而静默退化为 layer 0。
    VL::TerrainBuildPlan plan = MakePlan(
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 1.5f; }));
    plan.desc.heightScale = -100.0f;
    plan.desc.heightOffset = 0.0f;
    plan.desc.pageOrigin = Eigen::Vector2i(0, 0);
    plan.desc.pageTilesPerSide = 1;
    plan.desc.layers = {
        {"low", 0.0f, 0.4f, 0.0f, 90.0f, 0.0f},
        {"high", 0.6f, 1.0f, 0.0f, 90.0f, 0.0f}};

    VL::TerrainPageData page;
    ASSERT_NO_THROW(
        page = VL::TerrainWeightMapBuilder::BuildPage(plan, 1, 32, 0, 0));
    EXPECT_EQ(page.ids[0], 1u);
    EXPECT_EQ(page.weights[0], 255u);
}

TEST(TerrainResourceCache, MaterialPreviewKeepsActiveTerrainStreamingState)
{
    VL::RendererResourceCache& cache = VL::RendererResourceCache::GetInstance();
    cache.Clear();

    auto streaming = std::make_shared<VL::TerrainStreamingState>();
    streaming->pendingUploadRevision = 7;
    cache.BindTerrainStreaming(streaming);

    VL::RendererResourceCache candidate = cache.BeginActiveWorldCandidate(11);
    ASSERT_EQ(candidate.GetTerrainStreaming().get(), streaming.get());

    VL::RendererResourceCache::WorldLocalResourcePackageHandle retired =
        cache.CommitCandidate(std::move(candidate));
    // MI 预览只替换材质/MI，提交后 active World 仍必须指向同一个 streaming state。
    ASSERT_NE(cache.GetTerrainStreaming(), nullptr);
    EXPECT_EQ(cache.GetTerrainStreaming().get(), streaming.get());
    EXPECT_EQ(cache.GetTerrainStreaming()->pendingUploadRevision, 7u);
    ASSERT_NE(retired, nullptr);
    EXPECT_EQ(retired->terrainStreaming.get(), streaming.get());

    // Blueprint 扩展候选仍是只读共享，不受本次修复影响。
    VL::RendererResourceCache extension =
        cache.BeginWorldExtensionCandidate(12);
    EXPECT_EQ(extension.GetTerrainStreaming().get(), streaming.get());

    cache.Clear();
}

TEST(TerrainFeedbackApply, MarksHitsQueuesMissesAndSkipsUnregisteredPages)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(10, 10, 2);
    VL::TerrainBuildPlan plan = MakePagePlan(4);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 12;
    summary.pages = {
        {0, 0, 40, false, 3},
        {1, 0, 200, true, 5},
        // 未注册的虚拟页（例如同一 World 里 M_pbr 地形）必须跳过而不是抛错。
        {31, 31, 255, true, 1}};

    std::vector<VL::TerrainPageRequest> pending;
    const VL::TerrainFeedbackApplyStats stats =
        manager.ApplyFeedback(summary, 11, 4, pending);

    EXPECT_EQ(stats.markedPages, 1u);
    EXPECT_EQ(stats.missPagesSeen, 1u);
    EXPECT_EQ(stats.queuedRequests, 1u);
    EXPECT_EQ(stats.attemptedRequests, 1u);
    EXPECT_EQ(stats.succeededRequests, 1u);
    EXPECT_EQ(stats.deferredRequests, 0u);
    EXPECT_EQ(stats.unregisteredPages, 1u);
    EXPECT_TRUE(pending.empty());

    const VL::TerrainPageManager::PageState* hit =
        manager.FindPage(MakePageKey(path, 0, 0));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->lastSeenEpoch, 12u);
    EXPECT_EQ(hit->priority, 40u);
    EXPECT_TRUE(manager.IsPageResident(MakePageKey(path, 1, 0)));
}

TEST(TerrainFeedbackApply, HonorsPerFrameLimitAndRotatesDeferredRequests)
{
    VL::TerrainPageManager manager;
    // capacity=1 且页出生在 epoch 0：completed=0 时安全窗不成立，请求必然失败。
    manager.SetGpuEpochContext(0, 0, 0);
    manager.SetResidentCapacity(1);
    VL::TerrainBuildPlan plan = MakePagePlan(4);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 0));

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 1;
    summary.pages = {
        {1, 0, 200, true, 4},
        {2, 0, 200, true, 4},
        {3, 0, 200, true, 4}};

    std::vector<VL::TerrainPageRequest> pending;
    const VL::TerrainFeedbackApplyStats first =
        manager.ApplyFeedback(summary, 0, 1, pending);
    EXPECT_EQ(first.queuedRequests, 3u);
    EXPECT_EQ(first.attemptedRequests, 1u);
    EXPECT_EQ(first.succeededRequests, 0u);
    EXPECT_EQ(first.deferredRequests, 1u);
    ASSERT_EQ(pending.size(), 3u);
    // 失败页 deferred+1，下一帧被排到同优先级队尾，避免永远抢第一位。
    EXPECT_FALSE(pending.front().tileX == 1u &&
        pending.front().tileZ == 0u);

    const VL::TerrainFeedbackApplyStats second =
        manager.ApplyFeedback(summary, 0, 1, pending);
    EXPECT_EQ(second.queuedRequests, 0u);
    EXPECT_EQ(second.attemptedRequests, 1u);
    EXPECT_EQ(second.deferredRequests, 1u);
    EXPECT_EQ(pending.size(), 3u);
    EXPECT_EQ(pending.front().deferredRequestCount, 0u);
}

TEST(TerrainPageManager, QuarantineBlocksSlotUntilReleaseEpoch)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(20, 18, 2);
    VL::TerrainBuildPlan plan = MakePagePlan(2);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    // release = max(lastSeen=20, submitted=20) + framesInFlight(2) + 1
    EXPECT_EQ(manager.PhysicalSlotReleaseEpoch(0, 0), 23u);
    EXPECT_FALSE(manager.IsPhysicalSlotAvailable(0, 0, 22));
    EXPECT_TRUE(manager.IsPhysicalSlotAvailable(0, 0, 23));
    // fallback 物理槽位永远不参与分配。
    EXPECT_FALSE(manager.IsPhysicalSlotAvailable(
        VL::TerrainPageManager::FallbackPageX,
        VL::TerrainPageManager::FallbackPageY,
        std::numeric_limits<uint64_t>::max()));

    // quarantine 未过期时请求会拿下一个空闲槽位；旧槽位内容不被覆盖。
    ASSERT_TRUE(manager.RequestPageAtEpoch(path, 1, 0, 2, 0, 22));
    const VL::TerrainPageManager::PageState* page =
        manager.FindPage(MakePageKey(path, 1, 0));
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->physicalPageX, 1u);
    EXPECT_EQ(page->physicalPageY, 0u);
}

TEST(TerrainFeedbackApply, LayerlessPlacementInSameWorldDoesNotPolluteFeedback)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan layered = MakePagePlan(2);
    layered.placementId = "terrain|Layered";
    layered.desc.pageOrigin = Eigen::Vector2i(0, 0);
    manager.RegisterTerrain(layered, false);
    // 命中页必须已驻留，MarkPageUsedAtEpoch 才会推进 recency。
    ASSERT_TRUE(manager.RequestPage(layered.placementId, 0, 0, 1));

    VL::TerrainBuildPlan layerless = MakePagePlan(2);
    layerless.placementId = "terrain|Layerless";
    layerless.desc.assetPath =
        "Generated/Validation/TerrainTests/TR_layerless.json";
    layerless.desc.layers.clear();
    layerless.desc.pageOrigin = Eigen::Vector2i(2, 2);
    // rendererResourceLoadCoordinator 会跳过没有 layers 的摆放，因此该地形
    // 在页表里没有注册项；它的 feedback 页只能计入 unregistered。
    ASSERT_FALSE(VL::TerrainUsesVirtualPages(layerless));
    ASSERT_EQ(manager.Registrations().size(), 1u);

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 12;
    summary.pages = {
        {0, 0, 40, false, 3},
        {2, 2, 200, true, 3}};

    std::vector<VL::TerrainPageRequest> pending;
    const VL::TerrainFeedbackApplyStats stats =
        manager.ApplyFeedback(summary, 11, 4, pending);

    EXPECT_EQ(stats.markedPages, 1u);
    EXPECT_EQ(stats.missPagesSeen, 0u);
    EXPECT_EQ(stats.queuedRequests, 0u);
    EXPECT_EQ(stats.unregisteredPages, 1u);
    EXPECT_TRUE(pending.empty());
    EXPECT_FALSE(manager.IsResident(2, 2));
    EXPECT_EQ(manager.EvictionCount(), 0u);
}

TEST(TerrainFeedbackApply, DropsPendingRequestsThatLeaveTheView)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(0, 0, 0);
    manager.SetResidentCapacity(1);
    VL::TerrainBuildPlan plan = MakePagePlan(4);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 0));

    VL::TerrainFeedbackFrameSummary firstView;
    firstView.copiedEpoch = 1;
    firstView.pages = {
        {1, 0, 200, true, 4},
        {2, 0, 200, true, 4},
        {3, 0, 200, true, 4}};
    std::vector<VL::TerrainPageRequest> pending;
    manager.ApplyFeedback(firstView, 0, 3, pending);
    ASSERT_EQ(pending.size(), 3u);

    // 相机转走后只剩 (2,0) 可见；不可见的 pending 必须丢弃，不能继续烘焙。
    VL::TerrainFeedbackFrameSummary secondView;
    secondView.copiedEpoch = 2;
    secondView.pages = {{2, 0, 200, true, 4}};
    manager.ApplyFeedback(secondView, 0, 3, pending);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].tileX, 2u);
    EXPECT_EQ(pending[0].tileZ, 0u);
}

TEST(TerrainPageManager, UploadBatchCapConvergesAcrossFrames)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(4);
    const std::string path = plan.desc.assetPath;
    manager.RegisterTerrain(plan, false);
    for (uint32_t tileZ = 0; tileZ < 4; ++tileZ)
        for (uint32_t tileX = 0; tileX < 4; ++tileX)
            ASSERT_TRUE(manager.RequestPage(path, tileX, tileZ, 1));

    uint32_t uploadedPages = 0;
    uint32_t batchCount = 0;
    while (manager.HasDirtyUploads() && batchCount < 8)
    {
        const VL::TerrainUploadBatch batch = manager.BuildUploadBatch(4);
        ASSERT_FALSE(batch.Empty());
        EXPECT_LE(batch.atlasUploads.size(), 4u);
        for (const VL::TerrainPageManager::PageTableUpload& upload :
             batch.pageTableUploads)
        {
            if (upload.entry[3] == 0)
                continue;
            EXPECT_TRUE(BatchContainsPhysicalSlot(
                batch, upload.entry[0], upload.entry[1]));
        }
        uploadedPages +=
            static_cast<uint32_t>(batch.atlasUploads.size());
        manager.CommitUploadBatch(batch);
        ++batchCount;
    }
    EXPECT_FALSE(manager.HasDirtyUploads());
    EXPECT_EQ(uploadedPages, 16u);
    EXPECT_EQ(batchCount, 4u);
}

TEST(TerrainPageManager, PreloadFalseRegistersWithoutBakingPages)
{
    VL::TerrainPageManager manager;
    VL::TerrainBuildPlan plan = MakePagePlan(8);
    manager.RegisterTerrain(plan, false);

    EXPECT_EQ(manager.ResidentCount(), 0u);
    EXPECT_FALSE(manager.HasDirtyUploads());
    EXPECT_TRUE(manager.BuildUploadBatch(4).Empty());

    // 加载期“中心 2×2 初始集合”：只请求 4 页，剩余页交给 feedback。
    ASSERT_EQ(manager.Registrations().size(), 1u);
    EXPECT_EQ(
        manager.RequestInitialTerrainPages(manager.Registrations()[0]),
        4u);
    EXPECT_EQ(manager.ResidentCount(), 4u);
    const VL::TerrainUploadBatch batch = manager.BuildUploadBatch(8);
    EXPECT_EQ(batch.atlasUploads.size(), 4u);
    manager.CommitUploadBatch(batch);
    EXPECT_FALSE(manager.HasDirtyUploads());
}
