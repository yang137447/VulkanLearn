#include <gtest/gtest.h>
#include "terrain/terrainCompositedSourceSnapshots.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainCompositedPageSourceBatch.h"

namespace VL
{
namespace
{
TerrainBuildPlan SourcePlan()
{
    TerrainBuildPlan plan;
    plan.placementId = "terrain|source-integration";
    plan.desc.assetPath = "TR_source-integration.json";
    plan.desc.size = Eigen::Vector2f(1000, 1000);
    plan.desc.gridResolution = 4;
    plan.desc.chunkQuads = 2;
    plan.desc.pageTilesPerSide = 1;
    plan.desc.heightScale = 100;
    plan.desc.layers = {
        {"first", 0, 1, 0, 90, 0},
        {"second", 0, 1, 0, 90, 0}};
    auto height = std::make_shared<HostImage>();
    height->width = height->height = 8;
    height->channels = 1;
    height->format = HostImage::PixelFormat::R32_FLOAT;
    height->rowStrideBytes = 8 * sizeof(float);
    height->data.resize(8 * 8 * sizeof(float), 0);
    plan.heightmap = height;
    auto paint = std::make_shared<TerrainLayerPaintData>();
    paint->width = paint->height = 8;
    paint->layerCount = 2;
    paint->weights.resize(128, 0.5f);
    paint->opacity.resize(128, 255);
    plan.layerPaint = paint;
    return plan;
}

TerrainRvtBudgetCaps SourceCaps()
{
    TerrainRvtBudgetCaps caps;
    caps.totalCpuBytes = 128 * 1024 * 1024;
    caps.workspaceCpuBytes = caps.totalCpuBytes;
    return caps;
}
}

TEST(TerrainCompositedSourceIntegration, PageManagerKeepsChargedAliasesAfterPreparationReleased)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    auto geometry = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(), parent);
    TerrainCompositedSourceSnapshots snapshots(geometry);
    auto manager = std::make_unique<TerrainPageManager>();
    auto prepared = snapshots.Retain(SourcePlan());
    manager->RegisterTerrain(prepared, false);
    const auto charged = geometry->Statistics().current.cpuBytes;
    ASSERT_GT(charged, 0u);
    prepared = {};
    EXPECT_EQ(snapshots.LiveSources(), 2u);
    EXPECT_EQ(geometry->Statistics().current.cpuBytes, charged);
    EXPECT_GT(parent->Statistics().current.cpuBytes, charged);
    manager.reset();
    EXPECT_EQ(snapshots.LiveSources(), 0u);
    EXPECT_EQ(geometry->Statistics().current.cpuBytes, 0u);
    EXPECT_EQ(parent->Statistics().current.cpuBytes, 0u);
}

TEST(TerrainCompositedSourceIntegration, ChangedPixelsOverlapUntilManagerAndCandidateRelease)
{
    auto parent = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    auto geometry = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(), parent);
    TerrainCompositedSourceSnapshots snapshots(geometry);
    auto manager = std::make_unique<TerrainPageManager>();
    auto active = snapshots.Retain(SourcePlan(), TerrainRvtResourcePhase::Active);
    manager->RegisterTerrain(active, false);
    const auto initial = geometry->Statistics().current.cpuBytes;
    auto queued = snapshots.Retain(SourcePlan());
    ASSERT_EQ(snapshots.LiveSources(), 4u);
    EXPECT_EQ(geometry->Statistics().current.cpuBytes, initial * 2);
    auto candidate = queued;
    queued = {};
    manager->UpdateHeightmap(active.placementId, candidate.heightmap);
    manager->UpdateLayerPaint(active.placementId, candidate.layerPaint);
    snapshots.Promote(candidate, TerrainRvtResourcePhase::Active);
    active = {};
    EXPECT_EQ(snapshots.LiveSources(), 2u);
    EXPECT_EQ(geometry->Statistics().current.cpuBytes, initial);
    candidate = {};
    EXPECT_EQ(snapshots.LiveSources(), 2u);
    manager.reset();
    EXPECT_EQ(snapshots.LiveSources(), 0u);
    EXPECT_EQ(parent->Statistics().current.cpuBytes, 0u);
}

TEST(TerrainCompositedSourceIntegration, SamePixelAddressCanRebindUnchargedManagerReference)
{
    auto geometry = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    TerrainCompositedSourceSnapshots snapshots(geometry);
    auto manager = std::make_unique<TerrainPageManager>();
    auto original = SourcePlan();
    manager->RegisterTerrain(original, false);
    auto charged = snapshots.Retain(original);
    ASSERT_EQ(charged.heightmap.get(), original.heightmap.get());
    ASSERT_EQ(charged.layerPaint.get(), original.layerPaint.get());
    ASSERT_TRUE(charged.heightmap.owner_before(original.heightmap) ||
        original.heightmap.owner_before(charged.heightmap));
    manager->UpdateHeightmap(charged.placementId, charged.heightmap);
    manager->UpdateLayerPaint(charged.placementId, charged.layerPaint);
    charged = {};
    original = {};
    EXPECT_EQ(snapshots.LiveSources(), 2u);
    EXPECT_GT(geometry->Statistics().current.cpuBytes, 0u);
    manager.reset();
    EXPECT_EQ(geometry->Statistics().current.cpuBytes, 0u);
}

TEST(TerrainCompositedSourceIntegration, RepeatedEditsDoNotKeepAllOldSourceVersions)
{
    auto geometry = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    TerrainCompositedSourceSnapshots snapshots(geometry);
    auto manager = std::make_unique<TerrainPageManager>();
    auto active = snapshots.Retain(SourcePlan(), TerrainRvtResourcePhase::Active);
    manager->RegisterTerrain(active, false);
    const auto single = geometry->Statistics().current.cpuBytes;
    for (uint32_t edit = 0; edit < 24; ++edit)
    {
        auto next = snapshots.Retain(SourcePlan());
        EXPECT_EQ(snapshots.LiveSources(), 4u);
        EXPECT_EQ(geometry->Statistics().current.cpuBytes, single * 2);
        manager->UpdateHeightmap(active.placementId, next.heightmap);
        manager->UpdateLayerPaint(active.placementId, next.layerPaint);
        snapshots.Promote(next, TerrainRvtResourcePhase::Active);
        active = std::move(next);
        EXPECT_EQ(snapshots.LiveSources(), 2u);
        EXPECT_EQ(geometry->Statistics().current.cpuBytes, single);
    }
    active = {};
    manager.reset();
    EXPECT_EQ(geometry->Statistics().current.cpuBytes, 0u);
    EXPECT_EQ(geometry->Statistics().peak.cpuBytes, single * 2);
}

TEST(TerrainCompositedSourceIntegration, PreparedPageSourcesLeaveCurrentUntouchedUntilCommit)
{
    TerrainPageManager manager;
    auto original = SourcePlan();
    manager.RegisterTerrain(original, false);
    auto next = SourcePlan();
    TerrainCompositedPageSourceBatch batch(manager, {next});
    ASSERT_TRUE(batch.CanCommitNoThrow());
    EXPECT_EQ(manager.Plans().front().heightmap, original.heightmap);
    EXPECT_EQ(manager.Plans().front().layerPaint, original.layerPaint);
    batch.CommitNoThrow();
    EXPECT_EQ(manager.Plans().front().heightmap, next.heightmap);
    EXPECT_EQ(manager.Plans().front().layerPaint, next.layerPaint);
    EXPECT_FALSE(batch.CanCommitNoThrow());
}

TEST(TerrainCompositedSourceIntegration, AbandonedPageSourcePreparationRetainsOriginal)
{
    TerrainPageManager manager;
    auto original = SourcePlan();
    manager.RegisterTerrain(original, false);
    {
        TerrainCompositedPageSourceBatch batch(manager, {SourcePlan()});
        EXPECT_TRUE(batch.CanCommitNoThrow());
    }
    EXPECT_EQ(manager.Plans().front().heightmap, original.heightmap);
    EXPECT_EQ(manager.Plans().front().layerPaint, original.layerPaint);
}

TEST(TerrainCompositedSourceIntegration, PageSourceRecheckRejectsInterveningSafePointEdit)
{
    TerrainPageManager manager;
    auto original = SourcePlan();
    manager.RegisterTerrain(original, false);
    TerrainCompositedPageSourceBatch batch(manager, {SourcePlan()});
    auto newer = SourcePlan();
    manager.UpdateLayerPaint(original.placementId, newer.layerPaint);
    EXPECT_FALSE(batch.CanCommitNoThrow());
    batch.CommitNoThrow();
    EXPECT_EQ(manager.Plans().front().heightmap, original.heightmap);
    EXPECT_EQ(manager.Plans().front().layerPaint, newer.layerPaint);
}
}
