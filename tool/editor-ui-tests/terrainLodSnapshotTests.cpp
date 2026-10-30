#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sceneNode.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"

using namespace VL;

namespace
{

WorldMeshObject MakeLodChunk(
    const std::string& groupId,
    uint32_t level,
    uint32_t levelCount,
    const std::vector<float>& distancesCm)
{
    WorldMeshObject object;
    object.debugName =
        groupId + "::lod" + std::to_string(level);
    object.sceneObjectIdentity = "terrain|TerrainA";
    object.meshKey =
        groupId + "|lod=" + std::to_string(level);
    object.materialKey = "M_terrain";
    object.materialInstanceKey =
        "Maps/SC_test/Materials/MI_terrain.json";
    object.localBoundsMin = Eigen::Vector3f(-100.0f, -10.0f, -100.0f);
    object.localBoundsMax = Eigen::Vector3f(100.0f, 10.0f, 100.0f);
    object.lodGroupId = groupId;
    object.lodLevel = level;
    object.lodLevelCount = levelCount;
    object.lodDistancesCm = distancesCm;
    // 组 AABB 与当前级无关；这里刻意与 localBounds 相同，测试距离选择。
    object.lodGroupBoundsMin = object.localBoundsMin;
    object.lodGroupBoundsMax = object.localBoundsMax;
    return object;
}

World MakeLodWorld(float cameraZ)
{
    World world(11, "Maps/SC_test/SC_terrain_lod.json");
    auto camera = std::make_shared<Camera>();
    camera->SetProjection(60.0f, 16.0f / 9.0f, 0.1f, 100000.0f);
    camera->SetPosition(Eigen::Vector3f(0.0f, 0.0f, cameraZ));
    world.SetCamera(std::move(camera));
    return world;
}

WorldSnapshot BuildSnapshot(
    WorldSnapshotBuilder& builder,
    const World& world,
    uint64_t frameIndex)
{
    WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    desc.frameIndex = frameIndex;
    const auto result = builder.Build(world, desc);
    EXPECT_FALSE(result.IsFailure());
    return result.IsFailure() ? WorldSnapshot{} : result.Value();
}

const MeshDrawSnapshot* FindDraw(
    const WorldSnapshot& snapshot,
    const std::string& groupId)
{
    for (const MeshDrawSnapshot& draw : snapshot.meshDraws)
    {
        if (draw.lodGroupId == groupId)
            return &draw;
    }
    return nullptr;
}

void AddLodGroup(
    World& world,
    const std::string& groupId,
    const Eigen::Vector3f& center,
    const std::vector<float>& distancesCm)
{
    for (uint32_t level = 0; level < 2; ++level)
    {
        WorldMeshObject object =
            MakeLodChunk(groupId, level, 2, distancesCm);
        object.localBoundsMin =
            center + Eigen::Vector3f(-50.0f, -10.0f, -50.0f);
        object.localBoundsMax =
            center + Eigen::Vector3f(50.0f, 10.0f, 50.0f);
        object.lodGroupBoundsMin = object.localBoundsMin;
        object.lodGroupBoundsMax = object.localBoundsMax;
        world.AddMeshObject(
            groupId + "::lod" + std::to_string(level),
            std::move(object));
    }
}

} // namespace

TEST(TerrainLodSnapshot, EmitsExactlyOneLevelPerGroup)
{
    World world = MakeLodWorld(5000.0f);
    const std::vector<float> distances = {1000.0f, 2000.0f};
    const std::string groupId = "terrain|TerrainA|0|0";
    for (uint32_t level = 0; level < 3; ++level)
    {
        world.AddMeshObject(
            "TerrainA::chunk_0_0::lod" + std::to_string(level),
            MakeLodChunk(groupId, level, 3, distances));
    }

    WorldSnapshotBuilder builder;
    const WorldSnapshot far = BuildSnapshot(builder, world, 1);
    ASSERT_EQ(far.meshDraws.size(), 1u);
    ASSERT_NE(FindDraw(far, groupId), nullptr);
    EXPECT_EQ(FindDraw(far, groupId)->lodLevel, 2u);
    EXPECT_EQ(FindDraw(far, groupId)->mesh.key, groupId + "|lod=2");
    const uint64_t farObjectId = FindDraw(far, groupId)->objectId;

    // 移动相机到 chunk 内部：同一组切到 LOD0，且 RuntimeId 不随级变化。
    world.GetCamera()->SetPosition(Eigen::Vector3f::Zero());
    const WorldSnapshot near = BuildSnapshot(builder, world, 2);
    ASSERT_EQ(near.meshDraws.size(), 1u);
    ASSERT_NE(FindDraw(near, groupId), nullptr);
    EXPECT_EQ(FindDraw(near, groupId)->lodLevel, 0u);
    EXPECT_EQ(FindDraw(near, groupId)->mesh.key, groupId + "|lod=0");
    EXPECT_EQ(FindDraw(near, groupId)->objectId, farObjectId);

    const TerrainLodStatsSnapshot& stats = builder.GetTerrainLodStats();
    ASSERT_TRUE(stats.valid);
    EXPECT_EQ(stats.groupCount, 1u);
    EXPECT_EQ(stats.switchedLastFrame, 1u);
    ASSERT_EQ(stats.groups.size(), 1u);
    EXPECT_EQ(stats.groups[0].level, 0u);
    EXPECT_EQ(stats.groups[0].switchCount, 1u);

    // 编辑器大纲的 section id 与快照 draw 同源，且各级一致。
    const WorldMeshObject& lod0 =
        world.GetMeshObjects().at("TerrainA::chunk_0_0::lod0");
    const WorldMeshObject& lod2 =
        world.GetMeshObjects().at("TerrainA::chunk_0_0::lod2");
    EXPECT_EQ(
        WorldMeshObjectRuntimeId(world, "TerrainA::chunk_0_0::lod0", lod0),
        farObjectId);
    EXPECT_EQ(
        WorldMeshObjectRuntimeId(world, "TerrainA::chunk_0_0::lod2", lod2),
        farObjectId);
}

TEST(TerrainLodSnapshot, HysteresisSurvivesCameraJitter)
{
    World world = MakeLodWorld(1500.0f);
    const std::vector<float> distances = {1000.0f};
    const std::string groupId = "terrain|TerrainA|0|0";
    world.AddMeshObject(
        "TerrainA::chunk_0_0::lod0",
        MakeLodChunk(groupId, 0, 2, distances));
    world.AddMeshObject(
        "TerrainA::chunk_0_0::lod1",
        MakeLodChunk(groupId, 1, 2, distances));

    WorldSnapshotBuilder builder;
    // 距离 1400：初始选择 LOD1。
    const WorldSnapshot far = BuildSnapshot(builder, world, 1);
    ASSERT_EQ(far.meshDraws.size(), 1u);
    EXPECT_EQ(far.meshDraws[0].lodLevel, 1u);

    // 距离 950：落在 1000 的 10% 滞回带内，保持 LOD1。
    world.GetCamera()->SetPosition(
        Eigen::Vector3f(0.0f, 0.0f, 1050.0f));
    const WorldSnapshot jitter = BuildSnapshot(builder, world, 2);
    ASSERT_EQ(jitter.meshDraws.size(), 1u);
    EXPECT_EQ(jitter.meshDraws[0].lodLevel, 1u);
    EXPECT_EQ(builder.GetTerrainLodStats().switchedLastFrame, 0u);

    // 距离 850：低于 900，切回 LOD0。
    world.GetCamera()->SetPosition(
        Eigen::Vector3f(0.0f, 0.0f, 950.0f));
    const WorldSnapshot near = BuildSnapshot(builder, world, 3);
    ASSERT_EQ(near.meshDraws.size(), 1u);
    EXPECT_EQ(near.meshDraws[0].lodLevel, 0u);
    EXPECT_EQ(builder.GetTerrainLodStats().switchedLastFrame, 1u);
}

TEST(TerrainLodSnapshot, GroupsWithoutLodKeepLegacyDraws)
{
    World world = MakeLodWorld(5000.0f);
    WorldMeshObject legacy;
    legacy.debugName = "Sphere_A";
    legacy.sceneObjectIdentity = "Sphere_A";
    legacy.meshKey = "SM_sphere|0|Body";
    legacy.materialKey = "M_default";
    legacy.materialInstanceKey = "Maps/SC_test/Materials/MI_default.json";
    legacy.localBoundsMin = Eigen::Vector3f(-1.0f, -1.0f, -1.0f);
    legacy.localBoundsMax = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    world.AddMeshObject("Sphere_A", legacy);

    WorldSnapshotBuilder builder;
    const WorldSnapshot snapshot = BuildSnapshot(builder, world, 1);
    ASSERT_EQ(snapshot.meshDraws.size(), 1u);
    EXPECT_EQ(snapshot.meshDraws[0].lodGroupId, std::string());
    EXPECT_EQ(snapshot.meshDraws[0].lodLevel, 0u);
    // 无 LOD 组的对象 RuntimeId 仍由 objectName 派生，旧资产选择不变。
    EXPECT_EQ(
        snapshot.meshDraws[0].objectId,
        WorldObjectRuntimeId(world, "Sphere_A"));
    EXPECT_EQ(builder.GetTerrainLodStats().groupCount, 0u);
    EXPECT_EQ(builder.GetTerrainLodStats().switchedLastFrame, 0u);
}

TEST(TerrainLodSnapshot, TwoGroupsSelectIndependently)
{
    World world = MakeLodWorld(5000.0f);
    const std::vector<float> distances = {1000.0f};

    const std::string nearGroup = "terrain|TerrainA|0|0";
    const std::string farGroup = "terrain|TerrainA|0|1";
    AddLodGroup(
        world,
        nearGroup,
        Eigen::Vector3f(0.0f, 0.0f, 4900.0f),
        distances);
    AddLodGroup(
        world,
        farGroup,
        Eigen::Vector3f(0.0f, 0.0f, 0.0f),
        distances);

    WorldSnapshotBuilder builder;
    const WorldSnapshot snapshot = BuildSnapshot(builder, world, 1);
    ASSERT_EQ(snapshot.meshDraws.size(), 2u);
    ASSERT_NE(FindDraw(snapshot, nearGroup), nullptr);
    ASSERT_NE(FindDraw(snapshot, farGroup), nullptr);
    EXPECT_EQ(FindDraw(snapshot, nearGroup)->lodLevel, 0u);
    EXPECT_EQ(FindDraw(snapshot, farGroup)->lodLevel, 1u);
    EXPECT_EQ(builder.GetTerrainLodStats().groupCount, 2u);
}
