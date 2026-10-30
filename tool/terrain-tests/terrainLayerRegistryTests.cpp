#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "resource/image/hostImage.h"
#include "terrain/terrainLayerRegistry.h"
#include "terrain/terrainLayerSources.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMap.h"

namespace
{

using VL::TerrainBuildPlan;
using VL::TerrainLayerDesc;
using VL::TerrainLayerRegistry;

HostImage MakeFlatHeightmap(uint32_t width, uint32_t height)
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
    for (size_t index = 0; index < static_cast<size_t>(width) * height; ++index)
    {
        pixels[index * 4 + 0] = 0.5f;
        pixels[index * 4 + 1] = 0.5f;
        pixels[index * 4 + 2] = 0.5f;
        pixels[index * 4 + 3] = 1.0f;
    }
    return image;
}

TerrainBuildPlan MakePlan(
    const std::string& assetPath,
    const std::string& placementId,
    const std::vector<TerrainLayerDesc>& layers,
    int pageOriginX = 0)
{
    TerrainBuildPlan plan;
    plan.desc.assetPath = assetPath;
    plan.desc.name = assetPath;
    plan.desc.heightmapPath = assetPath + ".heightmap.png";
    plan.desc.size = Eigen::Vector2f(1000.0f, 1000.0f);
    plan.desc.heightScale = 100.0f;
    plan.desc.gridResolution = 8;
    plan.desc.chunkQuads = 4;
    plan.desc.pageOrigin = Eigen::Vector2i(pageOriginX, 0);
    plan.desc.pageTilesPerSide = 2;
    plan.desc.materialInstancePath = assetPath + ".material.json";
    plan.desc.layers = layers;
    plan.placementId = placementId;
    plan.heightmap = std::make_shared<const HostImage>(
        MakeFlatHeightmap(9, 9));
    return plan;
}

TerrainLayerDesc MakeLayer(
    const std::string& name,
    const std::string& key,
    float heightMin = 0.0f,
    uint32_t sourceSet = 0)
{
    TerrainLayerDesc layer;
    layer.name = name;
    layer.key = key;
    layer.heightMin = heightMin;
    layer.heightMax = 1.0f;
    layer.slopeMin = 0.0f;
    layer.slopeMax = 90.0f;
    layer.sourceSet = sourceSet;
    return layer;
}

} // namespace

TEST(TerrainLayerRegistry, SingleTerrainMapsIdentity)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan plan = MakePlan(
        "TR_a.json",
        "terrain|A",
        {MakeLayer("grass", ""), MakeLayer("rock", ""), MakeLayer("snow", "")});

    const std::vector<uint8_t>& map = registry.RegisterTerrain(plan);
    ASSERT_EQ(map.size(), 3u);
    EXPECT_EQ(map[0], 0u);
    EXPECT_EQ(map[1], 1u);
    EXPECT_EQ(map[2], 2u);
    EXPECT_EQ(registry.GlobalLayerCount(), 3u);
    EXPECT_EQ(registry.DedupedLayerCount(), 0u);
    EXPECT_EQ(registry.Layers()[0].key, "TR_a.json#grass");
}

TEST(TerrainLayerRegistry, SharedKeyDeduplicatesAcrossTerrains)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan forest = MakePlan(
        "TR_forest.json",
        "terrain|Forest",
        {MakeLayer("grass", "biome/grass"), MakeLayer("pine", "biome/pine")});
    const TerrainBuildPlan desert = MakePlan(
        "TR_desert.json",
        "terrain|Desert",
        {MakeLayer("grass", "biome/grass"), MakeLayer("sand", "biome/sand")},
        8);

    const std::vector<uint8_t>& forestMap = registry.RegisterTerrain(forest);
    const std::vector<uint8_t>& desertMap = registry.RegisterTerrain(desert);
    ASSERT_EQ(forestMap.size(), 2u);
    ASSERT_EQ(desertMap.size(), 2u);
    // grass 跨 TR 共享 globalId 0；其余两层各占一个新 ID。
    EXPECT_EQ(forestMap[0], 0u);
    EXPECT_EQ(desertMap[0], 0u);
    EXPECT_EQ(forestMap[1], 1u);
    EXPECT_EQ(desertMap[1], 2u);
    EXPECT_EQ(registry.GlobalLayerCount(), 3u);
    EXPECT_EQ(registry.DedupedLayerCount(), 1u);
    ASSERT_EQ(registry.Layers()[0].assetPaths.size(), 2u);
    EXPECT_EQ(registry.Layers()[0].placementCount, 2u);
}

TEST(TerrainLayerRegistry, SharedKeyWithDifferentParametersFails)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan first = MakePlan(
        "TR_first.json",
        "terrain|First",
        {MakeLayer("grass", "biome/grass", 0.0f)});
    const TerrainBuildPlan second = MakePlan(
        "TR_second.json",
        "terrain|Second",
        {MakeLayer("grass", "biome/grass", 0.4f)},
        8);
    registry.RegisterTerrain(first);

    try
    {
        registry.RegisterTerrain(second);
        FAIL() << "expected a layer key conflict";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        // 冲突必须同时指出两个资产路径，作者才能定位到具体 TR。
        EXPECT_NE(message.find("TR_first.json"), std::string::npos);
        EXPECT_NE(message.find("TR_second.json"), std::string::npos);
        EXPECT_NE(message.find("biome/grass"), std::string::npos);
    }
}

TEST(TerrainLayerRegistry, SameNameWithDifferentKeysCoexists)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan first = MakePlan(
        "TR_first.json", "terrain|First", {MakeLayer("grass", "")});
    const TerrainBuildPlan second = MakePlan(
        "TR_second.json", "terrain|Second", {MakeLayer("grass", "")}, 8);

    const std::vector<uint8_t>& firstMap = registry.RegisterTerrain(first);
    const std::vector<uint8_t>& secondMap = registry.RegisterTerrain(second);
    EXPECT_EQ(firstMap[0], 0u);
    EXPECT_EQ(secondMap[0], 1u);
    EXPECT_EQ(registry.GlobalLayerCount(), 2u);
    EXPECT_EQ(registry.Layers()[0].layer.name, "grass");
    EXPECT_EQ(registry.Layers()[1].layer.name, "grass");
}

TEST(TerrainLayerRegistry, CapacityAccepts256AndRejects257)
{
    TerrainLayerRegistry registry;
    std::vector<TerrainLayerDesc> layers;
    layers.reserve(256);
    for (uint32_t index = 0; index < 256; ++index)
    {
        layers.push_back(MakeLayer(
            "layer" + std::to_string(index),
            "cap/" + std::to_string(index)));
    }
    const TerrainBuildPlan full = MakePlan("TR_full.json", "terrain|Full", layers);
    const std::vector<uint8_t>& map = registry.RegisterTerrain(full);
    ASSERT_EQ(map.size(), 256u);
    EXPECT_EQ(map[255], 255u);
    EXPECT_EQ(registry.GlobalLayerCount(), 256u);

    TerrainLayerRegistry overflow;
    layers.push_back(MakeLayer("layer256", "cap/256"));
    const TerrainBuildPlan tooMany = MakePlan(
        "TR_overflow.json", "terrain|Overflow", layers);
    EXPECT_THROW(overflow.RegisterTerrain(tooMany), std::runtime_error);
}

TEST(TerrainLayerRegistry, DuplicateKeyInsideOneTerrainFails)
{
    TerrainLayerRegistry registry;
    // 显式 key 重复，或缺省 key 因同名而重复，都必须在注册期失败。
    const TerrainBuildPlan explicitDuplicate = MakePlan(
        "TR_dup.json",
        "terrain|Dup",
        {MakeLayer("a", "shared/key"), MakeLayer("b", "shared/key")});
    EXPECT_THROW(
        registry.RegisterTerrain(explicitDuplicate), std::runtime_error);

    TerrainLayerRegistry sameName;
    const TerrainBuildPlan defaultDuplicate = MakePlan(
        "TR_dup2.json",
        "terrain|Dup2",
        {MakeLayer("a", ""), MakeLayer("a", "")});
    EXPECT_THROW(
        sameName.RegisterTerrain(defaultDuplicate), std::runtime_error);
}

TEST(TerrainLayerRegistry, RegistersPlacementTwiceFails)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan plan = MakePlan(
        "TR_a.json", "terrain|A", {MakeLayer("grass", "")});
    registry.RegisterTerrain(plan);
    EXPECT_THROW(registry.RegisterTerrain(plan), std::runtime_error);
}

TEST(TerrainLayerRegistry, WeightMapBakesGlobalLayerIds)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan forest = MakePlan(
        "TR_forest.json",
        "terrain|Forest",
        {MakeLayer("grass", "biome/grass"), MakeLayer("pine", "biome/pine")});
    const TerrainBuildPlan desert = MakePlan(
        "TR_desert.json",
        "terrain|Desert",
        {MakeLayer("sand", "biome/sand"), MakeLayer("rock", "biome/rock")},
        8);
    registry.RegisterTerrain(forest);
    const std::vector<uint8_t>& desertMap = registry.RegisterTerrain(desert);
    ASSERT_EQ(desertMap[0], 2u);
    ASSERT_EQ(desertMap[1], 3u);

    const VL::TerrainPageData page = VL::TerrainWeightMapBuilder::BuildPage(
        desert, 2, 8, 0, 0, desertMap);
    ASSERT_EQ(page.ids.size(), 8u * 8u * 4u);
    // 两层都满足 height/slope 窗口，占满 top-2；未使用的 slot 保持 0 且权重为 0。
    for (size_t texel = 0; texel < page.ids.size() / 4; ++texel)
    {
        EXPECT_TRUE(page.ids[texel * 4 + 0] == 2u ||
                    page.ids[texel * 4 + 0] == 3u);
        EXPECT_TRUE(page.ids[texel * 4 + 1] == 2u ||
                    page.ids[texel * 4 + 1] == 3u);
        // 空 slot 的 ID 仍是候选列表首项（weight=0 不进混合），
        // 关键不变量是权重为 0，且 ID 都落在本 TR 的全局范围内。
        EXPECT_TRUE(page.ids[texel * 4 + 2] == 2u ||
                    page.ids[texel * 4 + 2] == 3u);
        EXPECT_TRUE(page.ids[texel * 4 + 3] == 2u ||
                    page.ids[texel * 4 + 3] == 3u);
        EXPECT_EQ(page.weights[texel * 4 + 2], 0u);
        EXPECT_EQ(page.weights[texel * 4 + 3], 0u);
    }
    for (const uint8_t id : page.candidates.layerIds)
        EXPECT_TRUE(id == 2u || id == 3u);
}

TEST(TerrainLayerRegistry, PageManagerBakesGlobalLayerIds)
{
    TerrainLayerRegistry registry;
    const TerrainBuildPlan forest = MakePlan(
        "TR_forest.json",
        "terrain|Forest",
        {MakeLayer("grass", "biome/grass"), MakeLayer("pine", "biome/pine")});
    const TerrainBuildPlan desert = MakePlan(
        "TR_desert.json",
        "terrain|Desert",
        {MakeLayer("sand", "biome/sand")},
        8);
    const std::vector<uint8_t>& forestMap = registry.RegisterTerrain(forest);
    registry.RegisterTerrain(desert);

    VL::TerrainPageManager pageManager;
    pageManager.RegisterTerrain(forest, forestMap, false);
    // 重复注册同一 placement 已被 registry 拒绝；这里直接用缓存映射。
    const std::vector<uint8_t>& desertMap =
        *registry.FindLayerIdMap("terrain|Desert");
    pageManager.RegisterTerrain(desert, desertMap, true);

    VL::TerrainPageManager::PageKey key;
    key.placementId = "terrain|Desert";
    key.tileX = 0;
    key.tileZ = 0;
    std::vector<uint8_t> ids;
    std::vector<uint8_t> weights;
    ASSERT_TRUE(pageManager.ReadPageLayers(key, true, ids, weights));
    ASSERT_EQ(ids.size(), 128u * 128u * 4u);
    for (size_t texel = 0; texel < ids.size() / 4; ++texel)
    {
        // 唯一层（Desert 的 sand）是 globalId 2；其余 slot 权重为 0。
        EXPECT_EQ(ids[texel * 4 + 0], 2u);
        for (uint32_t slot = 1; slot < 4; ++slot)
            EXPECT_EQ(weights[texel * 4 + slot], 0u);
    }
}

TEST(TerrainSourceSets, FindAndMipHelpers)
{
    VL::TerrainLayerSourceSets sources;
    VL::TerrainSourceSetRecord first;
    first.sourceSet = 0;
    first.albedoMipLevels = 4;
    first.normalMipLevels = 3;
    first.pbrMipLevels = 2;
    first.layerSlices.emplace(0u, 0u);
    first.layerSlices.emplace(3u, 1u);
    VL::TerrainSourceSetRecord second = first;
    second.sourceSet = 1;
    second.albedoMipLevels = 5;
    second.normalMipLevels = 5;
    second.pbrMipLevels = 5;
    sources.emplace(0u, first);
    sources.emplace(1u, second);

    EXPECT_EQ(first.MinMipLevels(), 2u);
    // 没有绑定真实纹理时全局最小值视为“无可用源”，由调用方按 0 处理。
    EXPECT_EQ(VL::TerrainSourceMinMipLevels(sources), 0u);
    EXPECT_EQ(VL::TerrainSourceMipLevelsForLayer(sources, 1u), 5u);
    EXPECT_EQ(VL::TerrainSourceMipLevelsForLayer(sources, 7u), 0u);
    EXPECT_EQ(VL::FindTerrainSourceSet(sources, 3u), nullptr);
    uint32_t slice = 99;
    EXPECT_TRUE(first.TryGetLayerSlice(3u, slice));
    EXPECT_EQ(slice, 1u);
    EXPECT_FALSE(first.TryGetLayerSlice(1u, slice));
}
