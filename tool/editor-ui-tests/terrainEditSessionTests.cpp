#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "editor/terrain/terrainEditSession.h"
#include "editor/terrain/terrainPaintSession.h"
#include "resource/image/hostImage.h"
#include "terrain/terrainHeightmap.h"
#include "world/world.h"

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
    image.mipLevels = 1;
    image.arrayLayers = 1;
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

VL::TerrainAssetDesc MakeDesc(uint32_t lodLevels)
{
    VL::TerrainAssetDesc desc;
    desc.assetPath = "Generated/Validation/TerrainEdit/TR_edit.json";
    desc.name = "TR_edit";
    desc.heightmapPath = "Generated/Validation/TerrainEdit/heightmap.exr";
    desc.size = Eigen::Vector2f(1000.0f, 500.0f);
    desc.heightScale = 200.0f;
    desc.heightOffset = 10.0f;
    desc.gridResolution = 4;
    desc.chunkQuads = 2;
    desc.lodLevels = lodLevels;
    desc.materialInstancePath =
        "Generated/Validation/TerrainEdit/MI_edit.json";
    return desc;
}

VL::TerrainCollisionInstance MakeInstance(
    uint32_t lodLevels,
    const std::string& placementId = "terrain|Landscape")
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = placementId;
    instance.plan.desc = MakeDesc(lodLevels);
    instance.plan.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(8, 8, [](uint32_t, uint32_t) { return 0.5f; }));
    instance.plan.placementId = placementId;
    return instance;
}

VL::TerrainSculptBrush MakeBrush(
    float centerUv,
    float radiusUv,
    float strength,
    float falloff = 0.0f)
{
    VL::TerrainSculptBrush brush;
    brush.mode = VL::TerrainSculptMode::Raise;
    brush.centerUv = Eigen::Vector2f(centerUv, centerUv);
    brush.radiusUv = radiusUv;
    brush.strength = strength;
    brush.falloff = falloff;
    return brush;
}

bool CreateSession(
    const VL::TerrainCollisionInstance& instance,
    VL::Editor::TerrainEditSession& session)
{
    std::string error;
    const bool created = VL::Editor::TerrainEditSession::Create(
        instance,
        session,
        error);
    EXPECT_TRUE(error.empty()) << error;
    return created;
}

float SessionSample(
    const VL::Editor::TerrainEditSession& session,
    float u,
    float v)
{
    const VL::Editor::TerrainMeshUpdate update = session.BuildUpdate();
    if (!update.heightmap)
        return std::numeric_limits<float>::quiet_NaN();
    return VL::TerrainHeightmap::SampleNormalized(
        *update.heightmap,
        u,
        v);
}

std::vector<float> SessionSamples(
    const VL::Editor::TerrainEditSession& session)
{
    const VL::Editor::TerrainMeshUpdate update = session.BuildUpdate();
    if (!update.heightmap)
        return {};
    const HostImage& image = *update.heightmap;
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(image.width) * image.height);
    for (uint32_t y = 0; y < image.height; ++y)
    {
        const float* row = reinterpret_cast<const float*>(
            image.data.data() +
            static_cast<size_t>(y) * image.rowStrideBytes);
        for (uint32_t x = 0; x < image.width; ++x)
            samples.push_back(row[x]);
    }
    return samples;
}

} // namespace

TEST(TerrainEditSessionTest, CreateRejectsEmptyPlanAndInvalidPlacement)
{
    VL::Editor::TerrainEditSession session;
    std::string error;

    VL::TerrainCollisionInstance empty = MakeInstance(1);
    empty.plan.heightmap.reset();
    EXPECT_FALSE(VL::Editor::TerrainEditSession::Create(
        empty, session, error));
    EXPECT_FALSE(error.empty());

    VL::TerrainCollisionInstance badPlacement = MakeInstance(1, "Landscape");
    EXPECT_FALSE(VL::Editor::TerrainEditSession::Create(
        badPlacement, session, error));
    EXPECT_FALSE(error.empty());

    VL::TerrainCollisionInstance badScale = MakeInstance(1);
    badScale.placement.scale.x() = 0.0f;
    EXPECT_FALSE(VL::Editor::TerrainEditSession::Create(
        badScale, session, error));
    EXPECT_FALSE(error.empty());

    VL::TerrainCollisionInstance badLod = MakeInstance(1);
    badLod.plan.desc.lodLevels = VL::TerrainMaxLodLevels + 1;
    EXPECT_FALSE(VL::Editor::TerrainEditSession::Create(
        badLod, session, error));
    EXPECT_FALSE(error.empty());
}

TEST(TerrainEditSessionTest, CreateAcceptsLayeredTerrainAndStillRejectsInvalidPlans)
{
    VL::TerrainCollisionInstance instance = MakeInstance(1);
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    VL::Editor::TerrainEditSession session;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainEditSession::Create(
        instance, session, error)) << error;
    EXPECT_EQ(session.Desc().layers.size(), 2u);

    // layers 不再是拒绝理由；真正无效的计划仍然必须失败。
    VL::TerrainCollisionInstance missingHeightmap = instance;
    missingHeightmap.plan.heightmap.reset();
    EXPECT_FALSE(VL::Editor::TerrainEditSession::Create(
        missingHeightmap, session, error));
    EXPECT_FALSE(error.empty());

    VL::TerrainCollisionInstance badSize = instance;
    badSize.plan.desc.size.x() = 0.0f;
    EXPECT_FALSE(VL::Editor::TerrainEditSession::Create(
        badSize, session, error));
    EXPECT_FALSE(error.empty());
}

TEST(TerrainEditSessionTest, StrokeCreatesOneUndoAndRedoRestoresPreview)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    EXPECT_EQ(session.PlacementId(), "terrain|Landscape");
    EXPECT_EQ(session.ActorIdentity(), "terrain|Landscape");
    EXPECT_EQ(session.Desc().assetPath, MakeDesc(1).assetPath);
    EXPECT_TRUE(session.Placement().scale.isApprox(Eigen::Vector3f::Ones()));
    EXPECT_TRUE(session.BeginStroke());
    EXPECT_TRUE(session.ApplyBrush(MakeBrush(0.5f, 0.2f, 0.2f)));
    EXPECT_FALSE(session.CanUndo());
    EXPECT_TRUE(session.EndStroke());
    EXPECT_TRUE(session.CanUndo());
    EXPECT_GT(session.Revision(), 0u);

    EXPECT_NEAR(SessionSample(session, 0.5f, 0.5f), 0.7f, 1.0e-6f);
    ASSERT_TRUE(session.Undo());
    EXPECT_NEAR(SessionSample(session, 0.5f, 0.5f), 0.5f, 1.0e-6f);
    EXPECT_TRUE(session.CanRedo());
    EXPECT_FALSE(session.CanUndo());

    ASSERT_TRUE(session.Redo());
    EXPECT_NEAR(SessionSample(session, 0.5f, 0.5f), 0.7f, 1.0e-6f);
    EXPECT_FALSE(session.CanRedo());
}

TEST(TerrainEditSessionTest, PreviewCancelRestoresWithoutHistory)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    const uint64_t revisionBefore = session.Revision();

    EXPECT_TRUE(session.BeginStroke());
    EXPECT_TRUE(session.IsStrokeActive());
    EXPECT_TRUE(session.ApplyBrush(MakeBrush(0.5f, 0.2f, 0.3f)));
    session.CancelStroke();

    EXPECT_FALSE(session.IsStrokeActive());
    EXPECT_FALSE(session.CanUndo());
    EXPECT_FALSE(session.CanRedo());
    EXPECT_NEAR(SessionSample(session, 0.5f, 0.5f), 0.5f, 1.0e-6f);
    EXPECT_GE(session.Revision(), revisionBefore);
    EXPECT_GT(session.BuildUpdate().chunks.size(), 0u);
}

TEST(TerrainEditSessionTest, RaiseThenLowerBackToTheSameSamplesDoesNotCreateHistory)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    ASSERT_TRUE(session.BeginStroke());
    VL::TerrainSculptBrush brush = MakeBrush(0.5f, 0.25f, 0.125f);
    ASSERT_TRUE(session.ApplyBrush(brush));
    brush.mode = VL::TerrainSculptMode::Lower;
    ASSERT_TRUE(session.ApplyBrush(brush));
    EXPECT_FALSE(session.EndStroke());
    EXPECT_FALSE(session.CanUndo());
    EXPECT_FALSE(session.CanRedo());
}

TEST(TerrainEditSessionTest, TwoStrokesUndoRedoAreSymmetric)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    const std::vector<float> baseline = SessionSamples(session);

    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(MakeBrush(0.0f, 0.01f, 0.1f)));
    ASSERT_TRUE(session.EndStroke());
    const std::vector<float> afterFirst = SessionSamples(session);

    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(MakeBrush(1.0f, 0.01f, 0.2f)));
    ASSERT_TRUE(session.EndStroke());
    const std::vector<float> afterSecond = SessionSamples(session);

    ASSERT_TRUE(session.Undo());
    EXPECT_EQ(SessionSamples(session), afterFirst);
    ASSERT_TRUE(session.Undo());
    EXPECT_EQ(SessionSamples(session), baseline);
    EXPECT_FALSE(session.CanUndo());

    ASSERT_TRUE(session.Redo());
    EXPECT_EQ(SessionSamples(session), afterFirst);
    ASSERT_TRUE(session.Redo());
    EXPECT_EQ(SessionSamples(session), afterSecond);
    EXPECT_FALSE(session.CanRedo());
}

TEST(TerrainEditSessionTest, HistoryIsBoundedToEightStrokes)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    for (int stroke = 0; stroke < 10; ++stroke)
    {
        ASSERT_TRUE(session.BeginStroke());
        ASSERT_TRUE(session.ApplyBrush(MakeBrush(0.5f, 0.2f, 0.01f)));
        ASSERT_TRUE(session.EndStroke());
    }

    int undoCount = 0;
    while (session.CanUndo() && undoCount < 32)
    {
        ASSERT_TRUE(session.Undo());
        ++undoCount;
    }
    EXPECT_EQ(undoCount, 8);
}

TEST(TerrainEditSessionTest, RaycastHitsFlatTerrainCenter)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));

    const VL::Editor::TerrainRaycastHit hit = session.Raycast(
        Eigen::Vector3f(0.0f, 1000.0f, 0.0f),
        Eigen::Vector3f(0.0f, -2.0f, 0.0f));
    ASSERT_TRUE(hit.valid);
    EXPECT_NEAR(hit.uv.x(), 0.5f, 1.0e-5f);
    EXPECT_NEAR(hit.uv.y(), 0.5f, 1.0e-5f);
    EXPECT_NEAR(hit.world.y(), 110.0f, 1.0e-3f);
    EXPECT_NEAR(hit.distanceCm, 890.0f, 1.0e-3f);
}

TEST(TerrainEditSessionTest, RaycastHandlesRotationAndScale)
{
    VL::TerrainCollisionInstance instance = MakeInstance(1);
    instance.placement.position = Eigen::Vector3f(100.0f, 20.0f, 50.0f);
    instance.placement.rotation = Eigen::Vector3f(0.0f, 90.0f, 0.0f);
    instance.placement.scale = Eigen::Vector3f(2.0f, 1.0f, 3.0f);
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(instance, session));

    const Eigen::Vector3f point = session.WorldPoint(0.25f, 0.75f);
    const VL::Editor::TerrainRaycastHit hit = session.Raycast(
        point + Eigen::Vector3f(0.0f, 1000.0f, 0.0f),
        Eigen::Vector3f(0.0f, -1.0f, 0.0f));
    ASSERT_TRUE(hit.valid);
    EXPECT_NEAR(hit.uv.x(), 0.25f, 1.0e-4f);
    EXPECT_NEAR(hit.uv.y(), 0.75f, 1.0e-4f);
    EXPECT_TRUE(hit.world.isApprox(point, 1.0e-2f));
    EXPECT_NEAR(hit.distanceCm, 1000.0f, 1.0e-2f);
}

TEST(TerrainEditSessionTest, RaycastRejectsDegenerateAndMisses)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    EXPECT_FALSE(session.Raycast(
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Zero()).valid);
    EXPECT_FALSE(session.Raycast(
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f(
            std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f)).valid);
    EXPECT_FALSE(session.Raycast(
        Eigen::Vector3f(0.0f, 10000.0f, 0.0f),
        Eigen::Vector3f(0.0f, 1.0f, 0.0f)).valid);
}

TEST(TerrainEditSessionTest, BuildUpdateUsesRuntimeChunkNamesForAllLods)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(2), session));
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(MakeBrush(0.0f, 0.01f, 0.1f)));
    ASSERT_TRUE(session.EndStroke());

    const VL::Editor::TerrainMeshUpdate update = session.BuildUpdate();
    ASSERT_TRUE(update.heightmap);
    EXPECT_EQ(update.placementId, "terrain|Landscape");
    EXPECT_EQ(update.actorIdentity, "terrain|Landscape");
    EXPECT_EQ(update.assetPath, MakeDesc(2).assetPath);
    ASSERT_EQ(update.chunks.size(), 2u);

    const VL::Editor::TerrainChunkMeshUpdate* lod0 = nullptr;
    const VL::Editor::TerrainChunkMeshUpdate* lod1 = nullptr;
    for (const VL::Editor::TerrainChunkMeshUpdate& chunk : update.chunks)
    {
        EXPECT_EQ(chunk.chunkX, 0u);
        EXPECT_EQ(chunk.chunkZ, 0u);
        EXPECT_NE(chunk.resourceKey.find("placement=terrain|Landscape"),
            std::string::npos);
        if (chunk.lodLevel == 0)
            lod0 = &chunk;
        else if (chunk.lodLevel == 1)
            lod1 = &chunk;
    }
    ASSERT_NE(lod0, nullptr);
    ASSERT_NE(lod1, nullptr);
    EXPECT_EQ(lod0->objectName, "Landscape::chunk_0_0::lod0");
    EXPECT_EQ(lod1->objectName, "Landscape::chunk_0_0::lod1");
    EXPECT_NE(lod0->resourceKey.find("|lod=0"), std::string::npos);
    EXPECT_NE(lod1->resourceKey.find("|lod=1"), std::string::npos);

    session.ResetDirty();
    EXPECT_TRUE(session.BuildUpdate().chunks.empty());
}

TEST(TerrainEditSessionTest, BuildUpdateUsesSingleLayerChunkName)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(MakeBrush(0.0f, 0.01f, 0.1f)));
    ASSERT_TRUE(session.EndStroke());

    const VL::Editor::TerrainMeshUpdate update = session.BuildUpdate();
    ASSERT_EQ(update.chunks.size(), 1u);
    EXPECT_EQ(update.chunks[0].lodLevel, 0u);
    EXPECT_EQ(update.chunks[0].objectName, "Landscape::chunk_0_0");
    EXPECT_NE(update.chunks[0].resourceKey.find("|lod=0"),
        std::string::npos);
}

TEST(TerrainEditSessionTest, WorldBoundsUpdateOnlyMatchesPlacementAndChunk)
{
    VL::World world(1, "Generated/Validation/TerrainEdit/SC_edit.json");
    VL::WorldMeshObject multiLod;
    multiLod.isTerrain = true;
    multiLod.terrainPlacementId = "terrain|Landscape";
    multiLod.lodLevel = 0;
    multiLod.lodLevelCount = 2;
    multiLod.lodGroupId = "Landscape|0|0";
    multiLod.localBoundsMin = Eigen::Vector3f(-1.0f, -1.0f, -1.0f);
    multiLod.localBoundsMax = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    multiLod.lodGroupBoundsMin = multiLod.localBoundsMin;
    multiLod.lodGroupBoundsMax = multiLod.localBoundsMax;
    world.AddMeshObject("Landscape::chunk_0_0::lod0", multiLod);
    VL::WorldMeshObject coarse = multiLod;
    coarse.lodLevel = 1;
    coarse.localBoundsMin = Eigen::Vector3f(-1.5f, -1.5f, -1.5f);
    coarse.localBoundsMax = Eigen::Vector3f(1.5f, 1.5f, 1.5f);
    coarse.lodGroupBoundsMin = Eigen::Vector3f(-9.0f, -9.0f, -9.0f);
    coarse.lodGroupBoundsMax = Eigen::Vector3f(9.0f, 9.0f, 9.0f);
    world.AddMeshObject("Landscape::chunk_0_0::lod1", coarse);

    VL::WorldMeshObject otherPlacement = multiLod;
    otherPlacement.terrainPlacementId = "terrain|Other";
    world.AddMeshObject("Other::chunk_0_0::lod0", otherPlacement);

    VL::WorldMeshObject singleLayer;
    singleLayer.isTerrain = true;
    singleLayer.terrainPlacementId = "terrain|Solo";
    singleLayer.localBoundsMin = Eigen::Vector3f(-2.0f, -2.0f, -2.0f);
    singleLayer.localBoundsMax = Eigen::Vector3f(2.0f, 2.0f, 2.0f);
    world.AddMeshObject("Solo::chunk_0_0", singleLayer);

    const Eigen::Vector3f newMin(-5.0f, 10.0f, -7.0f);
    const Eigen::Vector3f newMax(5.0f, 30.0f, 7.0f);
    ASSERT_TRUE(world.UpdateTerrainChunkBounds(
        "terrain|Landscape", 0, 0, 0, newMin, newMax));

    const auto& objects = world.GetMeshObjects();
    EXPECT_TRUE(objects.at("Landscape::chunk_0_0::lod0")
        .localBoundsMin.isApprox(newMin));
    EXPECT_TRUE(objects.at("Landscape::chunk_0_0::lod0")
        .lodGroupBoundsMax.isApprox(newMax));
    EXPECT_TRUE(objects.at("Landscape::chunk_0_0::lod1")
        .lodGroupBoundsMin.isApprox(newMin));
    EXPECT_TRUE(objects.at("Landscape::chunk_0_0::lod1")
        .lodGroupBoundsMax.isApprox(newMax));
    EXPECT_TRUE(objects.at("Other::chunk_0_0::lod0")
        .localBoundsMin.isApprox(Eigen::Vector3f(-1.0f, -1.0f, -1.0f)));

    ASSERT_TRUE(world.UpdateTerrainChunkBounds(
        "terrain|Solo", 0, 0, 0,
        Eigen::Vector3f(-3.0f, -3.0f, -3.0f),
        Eigen::Vector3f(3.0f, 3.0f, 3.0f)));
    EXPECT_TRUE(objects.at("Solo::chunk_0_0")
        .localBoundsMax.isApprox(Eigen::Vector3f(3.0f, 3.0f, 3.0f)));
    EXPECT_FALSE(world.UpdateTerrainChunkBounds(
        "terrain|Solo", 0, 0, 1, newMin, newMax));
    EXPECT_FALSE(world.UpdateTerrainChunkBounds(
        "terrain|Missing", 0, 0, 0, newMin, newMax));
}

TEST(TerrainEditSessionTest, SaveHeightmapWritesAPngThatTheRuntimeCanDecode)
{
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(CreateSession(MakeInstance(1), session));
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(MakeBrush(0.5f, 0.4f, 0.25f)));
    ASSERT_TRUE(session.EndStroke());

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vl-terrain-sculpt-test.png";
    std::string error;
    ASSERT_TRUE(session.SaveHeightmap(path, error)) << error;
    ASSERT_TRUE(std::filesystem::is_regular_file(path));
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, 8> signature{};
    file.read(reinterpret_cast<char*>(signature.data()), signature.size());
    EXPECT_EQ(signature, (std::array<uint8_t, 8>{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}));
    file.close();
    std::filesystem::remove(path);
}

TEST(TerrainEditSessionTest, PaintSessionBlendsLayerWeightsAndUndoesOneStroke)
{
    VL::TerrainCollisionInstance instance = MakeInstance(1);
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    VL::Editor::TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(instance, session, error)) << error;
    ASSERT_EQ(session.LayerCount(), 2u);

    VL::Editor::TerrainPaintBrush brush;
    brush.layerIndex = 1;
    brush.centerUv = Eigen::Vector2f(0.5f, 0.5f);
    brush.radiusUv = 0.5f;
    brush.strength = 1.0f;
    brush.falloff = 0.5f;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(brush));
    ASSERT_TRUE(session.EndStroke());
    const auto painted = session.PaintData();
    ASSERT_NE(painted, nullptr);
    EXPECT_GT(painted->Blend(1, 0.5f, 0.5f, 0.0f), 0.9f);
    EXPECT_FLOAT_EQ(painted->Blend(0, 0.5f, 0.5f, 0.25f), 0.25f);

    ASSERT_TRUE(session.Undo());
    EXPECT_FLOAT_EQ(session.PaintData()->Blend(1, 0.5f, 0.5f, 0.0f), 0.0f);
    ASSERT_TRUE(session.Redo());
    EXPECT_GT(session.PaintData()->Blend(1, 0.5f, 0.5f, 0.0f), 0.9f);

    brush.erase = true;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(brush));
    ASSERT_TRUE(session.EndStroke());
    EXPECT_LT(session.PaintData()->Blend(1, 0.5f, 0.5f, 0.0f), 0.5f);
}

TEST(TerrainEditSessionTest, PaintSessionAdoptsPersistedSnapshotAndKeepsItAcrossAStroke)
{
    VL::TerrainCollisionInstance instance = MakeInstance(1);
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.desc.paintPath =
        "Generated/Validation/TerrainEdit/paint.vlpaint";
    auto persisted = std::make_shared<VL::TerrainLayerPaintData>();
    persisted->width = 8;
    persisted->height = 8;
    persisted->layerCount = 2;
    persisted->weights.assign(8u * 8u * 2u, 0.0f);
    persisted->opacity.assign(8u * 8u * 2u, 0u);
    const size_t keptIndex = (static_cast<size_t>(1) * 8u + 7u) * 8u + 7u;
    persisted->weights[keptIndex] = 0.9f;
    persisted->opacity[keptIndex] = 255;
    instance.plan.layerPaint = persisted;

    VL::Editor::TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(
        instance, session, error)) << error;
    ASSERT_EQ(session.PaintPath(), instance.plan.desc.paintPath);
    ASSERT_EQ(session.PaintData()->width, 8u);
    ASSERT_EQ(session.PaintData()->height, 8u);
    ASSERT_EQ(session.PaintData()->layerCount, 2u);
    EXPECT_FLOAT_EQ(session.PaintData()->weights[keptIndex], 0.9f);

    // 会话必须持有深拷贝；外部 plan 快照的后续改动不能污染正在编辑的会话。
    persisted->weights[keptIndex] = 0.1f;
    EXPECT_FLOAT_EQ(session.PaintData()->weights[keptIndex], 0.9f);

    VL::Editor::TerrainPaintBrush brush;
    brush.layerIndex = 1;
    brush.centerUv = Eigen::Vector2f(0.5f, 0.5f);
    brush.radiusUv = 0.2f;
    brush.strength = 1.0f;
    brush.falloff = 0.5f;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(brush));
    ASSERT_TRUE(session.EndStroke());
    EXPECT_FLOAT_EQ(session.PaintData()->weights[keptIndex], 0.9f);
    EXPECT_EQ(session.PaintData()->opacity[keptIndex], 255);
}

TEST(TerrainEditSessionTest, WorldKeepsCommittedPaintForSessionRecreation)
{
    VL::TerrainCollisionInstance instance = MakeInstance(1);
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    VL::World world(1, "Generated/Validation/TerrainEdit/SC_paint.json");
    world.AddTerrainCollisionInstance(instance);

    auto paint = std::make_shared<VL::TerrainLayerPaintData>();
    paint->width = 8;
    paint->height = 8;
    paint->layerCount = 2;
    paint->weights.assign(8u * 8u * 2u, 0.0f);
    paint->opacity.assign(8u * 8u * 2u, 0u);
    const size_t paintedIndex = (static_cast<size_t>(1) * 8u + 3u) * 8u + 4u;
    paint->weights[paintedIndex] = 0.8f;
    paint->opacity[paintedIndex] = 255;

    ASSERT_TRUE(world.UpdateTerrainPlacementLayerPaint(
        instance.placementId, paint));
    ASSERT_EQ(world.GetTerrainCollisionInstances().size(), 1u);
    const VL::TerrainCollisionInstance& stored =
        world.GetTerrainCollisionInstances().front();
    ASSERT_NE(stored.plan.layerPaint, nullptr);
    EXPECT_FLOAT_EQ(stored.plan.layerPaint->weights[paintedIndex], 0.8f);

    // Play/Stop 只销毁会话；重建时必须从 World 中已提交的覆盖层继续。
    VL::Editor::TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(
        stored, session, error)) << error;
    EXPECT_FLOAT_EQ(session.PaintData()->weights[paintedIndex], 0.8f);
    EXPECT_FALSE(world.UpdateTerrainPlacementLayerPaint(
        "terrain|Missing", paint));
}

TEST(TerrainEditSessionTest, WorldHeightmapUpdateOnlyMatchesPlacement)
{
    VL::World world(1, "Generated/Validation/TerrainEdit/SC_edit.json");
    const std::shared_ptr<const HostImage> first =
        std::make_shared<const HostImage>(
            MakeHeightmap(4, 4, [](uint32_t, uint32_t) { return 0.25f; }));
    const std::shared_ptr<const HostImage> second =
        std::make_shared<const HostImage>(
            MakeHeightmap(4, 4, [](uint32_t, uint32_t) { return 0.75f; }));
    const std::shared_ptr<const HostImage> replacement =
        std::make_shared<const HostImage>(
            MakeHeightmap(4, 4, [](uint32_t, uint32_t) { return 0.5f; }));

    VL::TerrainCollisionInstance instance = MakeInstance(1);
    instance.plan.heightmap = first;
    world.AddTerrainCollisionInstance(instance);
    instance.placementId = "terrain|Other";
    instance.plan.placementId = instance.placementId;
    instance.plan.heightmap = second;
    world.AddTerrainCollisionInstance(instance);

    ASSERT_TRUE(world.UpdateTerrainPlacementHeightmap(
        "terrain|Landscape", replacement));
    const auto& instances = world.GetTerrainCollisionInstances();
    ASSERT_EQ(instances.size(), 2u);
    EXPECT_EQ(instances[0].plan.heightmap, replacement);
    EXPECT_EQ(instances[1].plan.heightmap, second);
    EXPECT_FALSE(world.UpdateTerrainPlacementHeightmap(
        "terrain|Missing", replacement));
    EXPECT_FALSE(world.UpdateTerrainPlacementHeightmap(
        "terrain|Landscape", nullptr));
}
