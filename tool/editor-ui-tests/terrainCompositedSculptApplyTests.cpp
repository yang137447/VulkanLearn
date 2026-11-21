#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>

#include "editor/terrain/terrainCompositedSculptApply.h"
#include "editor/terrain/terrainEditApply.h"
#include "renderableObject.h"
#include "world/world.h"

namespace
{
using VL::Editor::TerrainCompositedSculptApply;
using VL::Editor::TerrainMeshUpdate;
static_assert(TerrainCompositedSculptApply::FrameSlotCount ==
    RenderableObject::DynamicMeshFrameSlotCount);

TerrainMeshUpdate Baseline()
{
    TerrainMeshUpdate update;
    update.placementId = "terrain|Landscape";
    update.assetPath = "TR_test.json";
    update.desc.compositedVT.enabled = true;
    update.heightmap = std::make_shared<const HostImage>();
    for (uint32_t lod = 0; lod < 2; ++lod)
    {
        VL::Editor::TerrainChunkMeshUpdate chunk;
        chunk.lodLevel = lod;
        chunk.objectName = "Landscape::chunk_0_0::lod" + std::to_string(lod);
        chunk.resourceKey = "terrain-test-lod" + std::to_string(lod);
        chunk.vertices.emplace_back(Eigen::Vector3f::Zero(),
            Eigen::Vector3f::UnitY(), Eigen::Vector4f::Ones(),
            Eigen::Vector2f::Zero());
        chunk.indices = {0, 0, 0};
        chunk.patchIndices = {0, 0, 0, 0};
        chunk.boundsMax = Eigen::Vector3f::Ones() * 100.0f;
        update.chunks.push_back(std::move(chunk));
    }
    return update;
}

bool Accept(const TerrainMeshUpdate&, std::string&) { return true; }

void PopulateWorld(VL::World& world, const TerrainMeshUpdate& baseline)
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = baseline.placementId;
    instance.plan.placementId = baseline.placementId;
    instance.plan.desc = baseline.desc;
    instance.plan.heightmap = baseline.heightmap;
    world.AddTerrainCollisionInstance(instance);
    for (const auto& chunk : baseline.chunks)
    {
        VL::WorldMeshObject object;
        object.isTerrain = true;
        object.terrainPlacementId = baseline.placementId;
        object.meshKey = chunk.resourceKey;
        object.lodLevel = chunk.lodLevel;
        object.lodLevelCount = 2;
        object.lodGroupId = "landscape-0-0";
        object.localBoundsMin = chunk.boundsMin;
        object.localBoundsMax = chunk.boundsMax;
        object.lodGroupBoundsMin = baseline.chunks.front().boundsMin;
        object.lodGroupBoundsMax = baseline.chunks.front().boundsMax;
        world.AddMeshObject(chunk.objectName, std::move(object));
    }
}

TEST(TerrainCompositedSculptApply, RejectsStaticInitialLoadAndWrongMode)
{
    TerrainCompositedSculptApply sculpt;
    std::string error;
    auto update = Baseline();
    EXPECT_FALSE(sculpt.Register(7, update, false, error));
    EXPECT_NE(error.find("load time"), std::string::npos);
    update.desc.compositedVT.enabled = false;
    EXPECT_FALSE(sculpt.Register(7, update, true, error));

    auto heightmap = std::make_shared<HostImage>();
    heightmap->width = 4;
    heightmap->height = 4;
    heightmap->channels = 1;
    heightmap->format = HostImage::PixelFormat::R32_FLOAT;
    heightmap->data.resize(4 * 4 * sizeof(float), 0);
    VL::TerrainCollisionInstance instance;
    instance.placementId = update.placementId;
    instance.plan.heightmap = heightmap;
    instance.plan.desc.assetPath = update.assetPath;
    instance.plan.desc.size = Eigen::Vector2f(100.0f, 100.0f);
    instance.plan.desc.gridResolution = 4;
    instance.plan.desc.chunkQuads = 2;
    instance.plan.desc.lodLevels = 2;
    instance.plan.desc.compositedVT.enabled = true;
    VL::Editor::TerrainEditSession session;
    ASSERT_TRUE(VL::Editor::TerrainEditSession::Create(instance, session, error)) << error;
    session.ResetDirty();
    EXPECT_TRUE(session.BuildUpdate().chunks.empty());
    const auto complete = session.BuildUpdate(true);
    ASSERT_EQ(complete.chunks.size(), 8u);
    EXPECT_TRUE(session.BuildUpdate().chunks.empty());
    ASSERT_TRUE(sculpt.Register(7, complete, true, error)) << error;
    VL::TerrainSculptBrush brush;
    brush.centerUv = Eigen::Vector2f(0.5f, 0.5f);
    brush.radiusUv = 1.0f;
    brush.strength = 0.1f;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(brush));
    const auto dirtyBefore = session.BuildUpdate();
    const auto editedComplete = session.BuildUpdate(true);
    const auto dirtyAfter = session.BuildUpdate();
    ASSERT_EQ(editedComplete.chunks.size(), complete.chunks.size());
    ASSERT_EQ(dirtyBefore.chunks.size(), dirtyAfter.chunks.size());
    for (size_t index = 0; index < complete.chunks.size(); ++index)
    {
        EXPECT_EQ(editedComplete.chunks[index].indices, complete.chunks[index].indices);
        EXPECT_EQ(editedComplete.chunks[index].patchIndices, complete.chunks[index].patchIndices);
        EXPECT_EQ(editedComplete.chunks[index].resourceKey, complete.chunks[index].resourceKey);
    }
    ASSERT_TRUE(sculpt.Apply(7, dirtyAfter, Accept, error)) << error;
}

TEST(TerrainCompositedSculptApply, RejectsWorldPlacementAndTopologyChangesBeforeCommit)
{
    TerrainCompositedSculptApply sculpt;
    std::string error;
    const auto baseline = Baseline();
    ASSERT_TRUE(sculpt.Register(7, baseline, true, error)) << error;
    size_t commits = 0;
    const auto commit = [&](const TerrainMeshUpdate&, std::string&)
    {
        ++commits;
        return true;
    };
    EXPECT_FALSE(sculpt.Apply(8, baseline, commit, error));
    auto update = baseline;
    update.placementId = "terrain|Other";
    EXPECT_FALSE(sculpt.Apply(7, update, commit, error));
    update = baseline;
    update.chunks[1].patchIndices.pop_back();
    EXPECT_FALSE(sculpt.Apply(7, update, commit, error));
    update = baseline;
    update.chunks[1].indices.pop_back();
    EXPECT_FALSE(sculpt.Apply(7, update, commit, error));
    update = baseline;
    update.chunks[1].vertices.push_back(update.chunks[1].vertices.front());
    EXPECT_FALSE(sculpt.Apply(7, update, commit, error));
    update = baseline;
    update.chunks[1].resourceKey = "not-resident";
    EXPECT_FALSE(sculpt.Apply(7, update, commit, error));
    update = baseline;
    update.chunks.push_back(update.chunks.front());
    EXPECT_FALSE(sculpt.Apply(7, update, commit, error));
    EXPECT_EQ(commits, 0u);
}

TEST(TerrainCompositedSculptApply, RejectsInvalidBoundsAndVertexData)
{
    TerrainCompositedSculptApply sculpt;
    std::string error;
    auto update = Baseline();
    ASSERT_TRUE(sculpt.Register(7, update, true, error));
    update.chunks.front().vertices.front().position.y() = 101.0f;
    EXPECT_FALSE(sculpt.Apply(7, update, Accept, error));
    update = Baseline();
    update.chunks.front().boundsMin.x() = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(sculpt.Apply(7, update, Accept, error));
    update = Baseline();
    update.chunks.front().vertices.front().normal.x() =
        std::numeric_limits<float>::infinity();
    EXPECT_FALSE(sculpt.Apply(7, update, Accept, error));
}

TEST(TerrainCompositedSculptApply, ReplaysFrozenLatestVerticesAcrossSlotsAndHiddenLods)
{
    TerrainCompositedSculptApply sculpt;
    std::string error;
    auto update = Baseline();
    ASSERT_TRUE(sculpt.Register(7, update, true, error));
    update.chunks.front().vertices.front().position.y() = 10.0f;
    update.chunks.back().vertices.front().position.y() = 20.0f;
    ASSERT_TRUE(sculpt.Apply(7, update, Accept, error));
    update.chunks.front().vertices.front().position.y() = 99.0f;

    std::array<float, 3> slots{};
    std::array<float, 3> hiddenLodSlots{};
    size_t visibleCalls = 0;
    size_t hiddenCalls = 0;
    for (size_t frame = 0; frame < 12; ++frame)
    {
        sculpt.ReplayForFrame(7, uint32_t(frame % 3),
            [&](const std::string& key, const std::vector<Vertex>& vertices)
        {
            if (key == "terrain-test-lod0")
            {
                ++visibleCalls;
                slots[frame % 3] = vertices.front().position.y();
                return true;
            }
            ++hiddenCalls;
            if (frame < 6)
                return false;
            hiddenLodSlots[frame % 3] = vertices.front().position.y();
            return true;
        });
    }
    EXPECT_EQ(visibleCalls, 3u);
    EXPECT_EQ(hiddenCalls, 9u);
    EXPECT_EQ(slots, (std::array<float, 3>{10.0f, 10.0f, 10.0f}));
    EXPECT_EQ(hiddenLodSlots, (std::array<float, 3>{20.0f, 20.0f, 20.0f}));

    update.chunks.resize(1);
    update.chunks.front().vertices.front().position.y() = 30.0f;
    ASSERT_TRUE(sculpt.Apply(7, update, Accept, error));
    size_t changedCalls = 0;
    for (uint32_t slot : {2u, 2u, 0u, 1u, 0u, 1u, 2u})
        sculpt.ReplayForFrame(7, slot,
            [&](const std::string& key, const std::vector<Vertex>& vertices)
        {
            EXPECT_EQ(key, "terrain-test-lod0");
            EXPECT_FLOAT_EQ(vertices.front().position.y(), 30.0f);
            ++changedCalls;
            return true;
        });
    EXPECT_EQ(changedCalls, 3u);
    size_t count = 0;
    sculpt.Replay(7, [&](const std::string& key, const std::vector<Vertex>& vertices)
    {
        ++count;
        EXPECT_FLOAT_EQ(vertices.front().position.y(),
            key == "terrain-test-lod0" ? 30.0f : 20.0f);
    });
    EXPECT_EQ(count, 2u);
}

TEST(TerrainCompositedSculptApply, FailedCommitKeepsPreviousReplayAndResetRejectsOldWorld)
{
    TerrainCompositedSculptApply sculpt;
    std::string error;
    auto update = Baseline();
    ASSERT_TRUE(sculpt.Register(7, update, true, error));
    ASSERT_TRUE(sculpt.Apply(7, update, Accept, error));
    update.chunks.front().vertices.front().position.y() = 30.0f;
    EXPECT_FALSE(sculpt.Apply(7, update,
        [](const TerrainMeshUpdate&, std::string&) -> bool
        { throw std::runtime_error("refresh rejected"); }, error));
    sculpt.Replay(7, [](const std::string&, const std::vector<Vertex>& vertices)
    {
        EXPECT_FLOAT_EQ(vertices.front().position.y(), 0.0f);
    });
    sculpt.Replay(8, [](const std::string&, const std::vector<Vertex>&)
    {
        FAIL() << "Old World replay leaked into replacement";
    });
    const TerrainCompositedSculptApply::FrameQueueVertices unexpected =
        [](const std::string&, const std::vector<Vertex>&)
        {
            ADD_FAILURE() << "Unexpected replay";
            return true;
        };
    sculpt.ReplayForFrame(8, 0, unexpected);
    sculpt.ReplayForFrame(7, 3, unexpected);
    EXPECT_THROW(sculpt.ReplayForFrame(7, 0,
        [](const std::string&, const std::vector<Vertex>&) -> bool
        { throw std::runtime_error("queue failed"); }), std::runtime_error);
    size_t retried = 0;
    sculpt.ReplayForFrame(7, 0, [&](const std::string&, const std::vector<Vertex>&)
    {
        ++retried;
        return true;
    });
    EXPECT_EQ(retried, 2u);
    sculpt.ReplayForFrame(7, 0, unexpected);
    sculpt.Reset();
    EXPECT_FALSE(sculpt.Apply(7, update, Accept, error));
    sculpt.Replay(7, [](const std::string&, const std::vector<Vertex>&)
    {
        FAIL() << "Reset retained stale vertices";
    });
    sculpt.ReplayForFrame(7, 1, unexpected);
}

TEST(TerrainCompositedSculptApply, EnforcesChunkAndRetainedByteBudgets)
{
    std::string error;
    auto update = Baseline();
    TerrainCompositedSculptApply tooFew(1);
    EXPECT_FALSE(tooFew.Register(7, update, true, error));
    TerrainCompositedSculptApply tooSmall(2, 8);
    EXPECT_FALSE(tooSmall.Register(7, update, true, error));
    const size_t topologyBytes = 2 * 7 * sizeof(uint32_t);
    TerrainCompositedSculptApply sculpt(2, topologyBytes + sizeof(Vertex));
    ASSERT_TRUE(sculpt.Register(7, update, true, error));
    EXPECT_FALSE(sculpt.Apply(7, update, Accept, error));
    update.chunks.resize(1);
    for (size_t stamp = 0; stamp < 100; ++stamp)
        ASSERT_TRUE(sculpt.Apply(7, update, Accept, error)) << error;
}

TEST(TerrainCompositedSculptApply, ConcurrentReplayOwnsSnapshotAndDoesNotHoldLockInCallback)
{
    TerrainCompositedSculptApply sculpt;
    std::string error;
    auto update = Baseline();
    ASSERT_TRUE(sculpt.Register(7, update, true, error));
    ASSERT_TRUE(sculpt.Apply(7, update, Accept, error));
    bool published = false;
    sculpt.ReplayForFrame(7, 0,
        [&](const std::string& key, const std::vector<Vertex>&)
    {
        if (key == "terrain-test-lod0")
        {
            auto changed = update;
            changed.chunks.resize(1);
            changed.chunks.front().vertices.front().position.y() = 1.0f;
            EXPECT_TRUE(sculpt.Apply(7, changed, Accept, error));
            published = true;
        }
        return true;
    });
    EXPECT_TRUE(published);
    size_t newVersionCalls = 0;
    sculpt.ReplayForFrame(7, 0,
        [&](const std::string& key, const std::vector<Vertex>& vertices)
    {
        ++newVersionCalls;
        EXPECT_EQ(key, "terrain-test-lod0");
        EXPECT_FLOAT_EQ(vertices.front().position.y(), 1.0f);
        return true;
    });
    EXPECT_EQ(newVersionCalls, 1u);
    std::atomic<bool> done{false};
    std::thread reader([&]
    {
        while (!done.load())
            sculpt.ReplayForFrame(7, 1, [&](const std::string&, const std::vector<Vertex>& vertices)
            {
                EXPECT_TRUE(vertices.front().position.allFinite());
                sculpt.Replay(8, {});
                return true;
            });
    });
    for (size_t stamp = 0; stamp < 100; ++stamp)
    {
        update.chunks.front().vertices.front().position.y() = float(stamp);
        EXPECT_TRUE(sculpt.Apply(7, update, Accept, error));
    }
    done.store(true);
    reader.join();
}

TEST(TerrainCompositedSculptApply, WorldCommitQueuesUpdatedPlanAndRollsBackRefreshFailure)
{
    auto baseline = Baseline();
    VL::World world(7, "test");
    VL::TerrainCollisionInstance instance;
    instance.placementId = baseline.placementId;
    instance.plan.placementId = baseline.placementId;
    instance.plan.desc = baseline.desc;
    instance.plan.heightmap = baseline.heightmap;
    world.AddTerrainCollisionInstance(instance);
    for (const auto& chunk : baseline.chunks)
    {
        VL::WorldMeshObject object;
        object.isTerrain = true;
        object.terrainPlacementId = baseline.placementId;
        object.meshKey = chunk.resourceKey;
        object.lodLevel = chunk.lodLevel;
        object.lodLevelCount = 2;
        object.lodGroupId = "landscape-0-0";
        object.localBoundsMin = chunk.boundsMin;
        object.localBoundsMax = chunk.boundsMax;
        world.AddMeshObject(chunk.objectName, std::move(object));
    }
    TerrainCompositedSculptApply sculpt;
    std::string error;
    ASSERT_TRUE(sculpt.Register(7, baseline, true, error));
    auto update = baseline;
    update.heightmap = std::make_shared<const HostImage>();
    update.chunks.front().boundsMax.y() = 200.0f;
    VL::TerrainEditApplyStats stats;
    EXPECT_FALSE(VL::ApplyCompositedTerrainSculptUpdate(world, sculpt, update,
        [](const VL::TerrainBuildPlan&)
        { throw std::runtime_error("refresh rejected"); }, stats, error));
    EXPECT_EQ(world.GetTerrainCollisionInstances().front().plan.heightmap, baseline.heightmap);
    EXPECT_FLOAT_EQ(world.GetMeshObjects().at(baseline.chunks.front().objectName)
        .localBoundsMax.y(), 100.0f);
    bool refreshed = false;
    ASSERT_TRUE(VL::ApplyCompositedTerrainSculptUpdate(world, sculpt, update,
        [&](const VL::TerrainBuildPlan& plan)
        {
            refreshed = true;
            EXPECT_EQ(plan.heightmap, update.heightmap);
        }, stats, error)) << error;
    EXPECT_TRUE(refreshed);
    EXPECT_EQ(stats.renderableObjects, 0u);
    EXPECT_EQ(stats.worldChunks, 2u);
    EXPECT_EQ(world.GetTerrainCollisionInstances().front().plan.heightmap, update.heightmap);
    EXPECT_FLOAT_EQ(world.GetMeshObjects().at(baseline.chunks.front().objectName)
        .localBoundsMax.y(), 200.0f);
}

TEST(TerrainCompositedSculptApply, WorldCommitRejectsMissingPlacementAndRefreshService)
{
    const auto baseline = Baseline();
    VL::World world(7, "test");
    TerrainCompositedSculptApply sculpt;
    std::string error;
    ASSERT_TRUE(sculpt.Register(7, baseline, true, error));
    VL::TerrainEditApplyStats stats;
    size_t refreshes = 0;
    EXPECT_FALSE(VL::ApplyCompositedTerrainSculptUpdate(world, sculpt, baseline,
        [&](const VL::TerrainBuildPlan&) { ++refreshes; }, stats, error));
    PopulateWorld(world, baseline);
    EXPECT_FALSE(VL::ApplyCompositedTerrainSculptUpdate(
        world, sculpt, baseline, {}, stats, error));
    EXPECT_EQ(refreshes, 0u);
    EXPECT_EQ(stats.worldChunks, 0u);
    EXPECT_EQ(world.GetTerrainCollisionInstances().front().plan.heightmap, baseline.heightmap);
    sculpt.Replay(7, [](const std::string&, const std::vector<Vertex>&)
    {
        FAIL() << "Rejected commit published vertices";
    });
}

TEST(TerrainCompositedSculptApply, WorldCommitPreflightsWholeBatchBeforeChangingBounds)
{
    const auto baseline = Baseline();
    VL::World world(7, "test");
    PopulateWorld(world, baseline);
    VL::WorldMeshObject mismatched = world.GetMeshObjects().at(baseline.chunks.back().objectName);
    mismatched.meshKey = "another-placement-key";
    world.AddMeshObject(baseline.chunks.back().objectName, std::move(mismatched));
    TerrainCompositedSculptApply sculpt;
    std::string error;
    ASSERT_TRUE(sculpt.Register(7, baseline, true, error));
    auto update = baseline;
    update.heightmap = std::make_shared<const HostImage>();
    update.chunks.front().boundsMax.y() = 200.0f;
    size_t refreshes = 0;
    VL::TerrainEditApplyStats stats;
    EXPECT_FALSE(VL::ApplyCompositedTerrainSculptUpdate(world, sculpt, update,
        [&](const VL::TerrainBuildPlan&) { ++refreshes; }, stats, error));
    EXPECT_EQ(refreshes, 0u);
    EXPECT_EQ(stats.worldChunks, 0u);
    EXPECT_EQ(world.GetTerrainCollisionInstances().front().plan.heightmap, baseline.heightmap);
    EXPECT_FLOAT_EQ(world.GetMeshObjects().at(baseline.chunks.front().objectName)
        .localBoundsMax.y(), 100.0f);
    sculpt.Replay(7, [](const std::string&, const std::vector<Vertex>&)
    {
        FAIL() << "Invalid World batch published vertices";
    });
}

TEST(TerrainCompositedSculptApply, WorldCommitRollsBackPartialBoundsAndLodGroupOnFailure)
{
    auto baseline = Baseline();
    baseline.chunks.back().objectName = "Landscape::invalid-chunk-name";
    VL::World world(7, "test");
    PopulateWorld(world, baseline);
    TerrainCompositedSculptApply sculpt;
    std::string error;
    ASSERT_TRUE(sculpt.Register(7, baseline, true, error));
    auto update = baseline;
    update.heightmap = std::make_shared<const HostImage>();
    update.chunks.front().boundsMax.y() = 200.0f;
    size_t refreshes = 0;
    VL::TerrainEditApplyStats stats;
    EXPECT_FALSE(VL::ApplyCompositedTerrainSculptUpdate(world, sculpt, update,
        [&](const VL::TerrainBuildPlan&) { ++refreshes; }, stats, error));
    EXPECT_NE(error.find("bounds update failed"), std::string::npos);
    EXPECT_EQ(refreshes, 0u);
    EXPECT_EQ(stats.worldChunks, 0u);
    EXPECT_EQ(world.GetTerrainCollisionInstances().front().plan.heightmap, baseline.heightmap);
    for (const auto& chunk : baseline.chunks)
    {
        const auto& object = world.GetMeshObjects().at(chunk.objectName);
        EXPECT_FLOAT_EQ(object.localBoundsMax.y(), 100.0f);
        EXPECT_FLOAT_EQ(object.lodGroupBoundsMax.y(), 100.0f);
    }
    sculpt.Replay(7, [](const std::string&, const std::vector<Vertex>&)
    {
        FAIL() << "Partially rolled back commit published vertices";
    });
}
}
