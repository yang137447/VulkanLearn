#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "terrain/terrainPageManager.h"

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

VL::TerrainBuildPlan MakePlan(
    const std::string& placementId,
    uint32_t tilesPerSide,
    int originX = 0)
{
    VL::TerrainBuildPlan plan;
    plan.placementId = placementId;
    plan.desc.assetPath =
        "Generated/Validation/TerrainPageManagerP31/TR_" + placementId + ".json";
    plan.desc.name = "TR_" + placementId;
    plan.desc.heightmapPath =
        "Generated/Validation/TerrainPageManagerP31/heightmap.png";
    plan.desc.size = Eigen::Vector2f(1000.0f, 1000.0f);
    plan.desc.heightScale = 100.0f;
    plan.desc.heightOffset = 0.0f;
    plan.desc.gridResolution = 4;
    plan.desc.chunkQuads = 2;
    plan.desc.materialInstancePath =
        "Generated/Validation/TerrainPageManagerP31/MI_terrain.json";
    plan.desc.layers = {
        {"a", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f},
        {"b", 0.0f, 1.0f, 0.0f, 90.0f, 0.0f}};
    plan.desc.pageOrigin = Eigen::Vector2i(originX, 0);
    plan.desc.pageTilesPerSide = tilesPerSide;
    plan.heightmap = std::make_shared<const HostImage>(
        MakeHeightmap(9, 9, [](uint32_t, uint32_t) { return 0.5f; }));
    return plan;
}

VL::TerrainPageManager::PageKey MakeKey(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ)
{
    VL::TerrainPageManager::PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    return key;
}

} // namespace

TEST(TerrainPageManagerP31, FeedbackHitChangesEpochAwareEvictionOrder)
{
    VL::TerrainPageManager manager;
    // framesInFlight=0 → 安全窗为 2。两页都能在 completedEpoch=7 淘汰。
    manager.SetGpuEpochContext(0, 0, 0);
    const VL::TerrainBuildPlan plan = MakePlan("P31Recency", 2);
    const std::string path = plan.placementId;
    manager.RegisterTerrain(plan, false);

    // 旧帧号 LRU 认为 (1,0) 更老；但 (1,0) 在上一个 epoch 刚被 feedback 命中，
    // epoch-aware LRU 必须保护它，转而淘汰帧号更新但反馈更旧的 (0,0)。
    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 100));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 1));
    ASSERT_TRUE(manager.MarkPageUsedAtEpoch(path, 0, 0, 3));
    ASSERT_TRUE(manager.MarkPageUsedAtEpoch(path, 1, 0, 5));

    EXPECT_EQ(manager.FindPage(MakeKey(path, 0, 0))->lastTouchedEpoch, 3u);
    EXPECT_EQ(manager.FindPage(MakeKey(path, 1, 0))->lastTouchedEpoch, 5u);

    ASSERT_TRUE(manager.EvictLeastRecentlyUsed(7));
    EXPECT_FALSE(manager.IsPageResident(MakeKey(path, 0, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakeKey(path, 1, 0)));
}

TEST(TerrainPageManagerP31, LoweringBudgetWaitsForFeedbackWindow)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(20, 20, 2);
    const VL::TerrainBuildPlan plan = MakePlan("P31Budget", 2);
    const std::string path = plan.placementId;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 1));
    ASSERT_EQ(manager.ResidentCount(), 2u);

    // 窗口 = max(2, 2+1) = 3。epoch 22 时两页都还在安全窗内：
    // 预算可以降，但驻留数允许暂时超预算，不能淘汰当前可见页。
    manager.SetResidentCapacityAtEpoch(1, 22);
    EXPECT_EQ(manager.ResidentCapacity(), 1u);
    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_EQ(manager.EvictionCount(), 0u);
    EXPECT_TRUE(manager.IsPageResident(MakeKey(path, 0, 0)));
    EXPECT_TRUE(manager.IsPageResident(MakeKey(path, 1, 0)));

    // epoch 23 时 lastSeen(20) + 3 <= 23，安全候选出现，回到预算。
    manager.SetResidentCapacityAtEpoch(1, 23);
    EXPECT_EQ(manager.ResidentCount(), 1u);
    EXPECT_EQ(manager.EvictionCount(), 1u);
}

TEST(TerrainPageManagerP31, EvictionWindowTracksFramesInFlight)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(0, 0, 2);
    EXPECT_EQ(manager.EvictionWindowFrames(), 3u);
    manager.SetGpuEpochContext(0, 0, 0);
    EXPECT_EQ(manager.EvictionWindowFrames(), 2u);
    manager.SetGpuEpochContext(0, 0, 1);
    EXPECT_EQ(manager.EvictionWindowFrames(), 2u);
}

TEST(TerrainPageManagerP31, ReclaimExpiredQuarantinesUpdatesCountsIdempotently)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(10, 10, 2);
    const VL::TerrainBuildPlan plan = MakePlan("P31Quarantine", 2);
    const std::string path = plan.placementId;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    ASSERT_TRUE(manager.RequestPage(path, 1, 0, 1));
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    // release = max(lastSeen=10, submitted=10) + framesInFlight(2) + 1 = 13。
    ASSERT_EQ(manager.QuarantinedPhysicalSlotCount(), 1u);
    const uint32_t freeBefore =
        manager.ImmediatelyAllocatablePhysicalSlotCount();

    // 未到期不回收。
    EXPECT_EQ(manager.ReclaimExpiredQuarantines(12), 0u);
    EXPECT_EQ(manager.QuarantinedPhysicalSlotCount(), 1u);
    EXPECT_EQ(manager.ImmediatelyAllocatablePhysicalSlotCount(), freeBefore);

    // 到期后即使没有新分配也要回收 free/隔离计数。
    EXPECT_EQ(manager.ReclaimExpiredQuarantines(13), 1u);
    EXPECT_EQ(manager.QuarantinedPhysicalSlotCount(), 0u);
    EXPECT_EQ(manager.ImmediatelyAllocatablePhysicalSlotCount(), freeBefore + 1);
    EXPECT_EQ(manager.PhysicalSlotReleaseEpoch(0, 0), 0u);
    EXPECT_TRUE(manager.IsPhysicalSlotAvailable(0, 0, 13));

    // 重复调用不重复计数。
    EXPECT_EQ(manager.ReclaimExpiredQuarantines(13), 0u);
    EXPECT_EQ(manager.ReclaimExpiredQuarantines(
                  std::numeric_limits<uint64_t>::max()), 0u);
    // 不再保留 fallback 物理槽位：最后一个槽位同样是可分配的普通槽位。
    EXPECT_TRUE(manager.IsPhysicalSlotAvailable(
        VL::TerrainPageManager::PageTableSize - 1,
        VL::TerrainPageManager::PageTableSize - 1,
        std::numeric_limits<uint64_t>::max()));
}

TEST(TerrainPageManagerP31, AllocateReclaimsExpiredQuarantineBeforeChoosingSlot)
{
    VL::TerrainPageManager manager;
    manager.SetGpuEpochContext(10, 10, 2);
    const VL::TerrainBuildPlan plan = MakePlan("P31QuarantineAllocate", 2);
    const std::string path = plan.placementId;
    manager.RegisterTerrain(plan, false);

    ASSERT_TRUE(manager.RequestPage(path, 0, 0, 1));
    const uint32_t freeAfterFirstAllocation =
        manager.ImmediatelyAllocatablePhysicalSlotCount();
    ASSERT_TRUE(manager.EvictPage(path, 0, 0));
    ASSERT_EQ(manager.QuarantinedPhysicalSlotCount(), 1u);

    // releaseEpoch=13；AllocatePhysicalSlot 在完成窗后先回收该槽位，再把它
    // 分配给新虚拟页。free 不能因“回收 + 分配”被净增加或双重计数。
    ASSERT_TRUE(manager.RequestPageAtEpoch(path, 1, 0, 2, 0, 13));
    const VL::TerrainPageManager::PageState* reused =
        manager.FindPage(MakeKey(path, 1, 0));
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->physicalPageX, 0u);
    EXPECT_EQ(reused->physicalPageY, 0u);
    EXPECT_EQ(manager.QuarantinedPhysicalSlotCount(), 0u);
    EXPECT_EQ(
        manager.ImmediatelyAllocatablePhysicalSlotCount(),
        freeAfterFirstAllocation);
}

TEST(TerrainPageManagerP31, CapacityStressTerminatesAtOneAndEight)
{
    // 容量 1：16 个请求逐个腾位，必须正常结束且驻留数始终不超过预算。
    {
        VL::TerrainPageManager manager;
        manager.SetGpuEpochContext(0, 0, 0);
        const VL::TerrainBuildPlan plan = MakePlan("P31StressOne", 4);
        const std::string path = plan.placementId;
        manager.RegisterTerrain(plan, false);
        manager.SetResidentCapacityAtEpoch(1, 100);

        uint32_t requestIndex = 0;
        for (uint32_t tileZ = 0; tileZ < 4; ++tileZ)
        {
            for (uint32_t tileX = 0; tileX < 4; ++tileX)
            {
                ASSERT_TRUE(manager.RequestPageAtEpoch(
                    path, tileX, tileZ, requestIndex, 0, 100 + requestIndex));
                ++requestIndex;
                EXPECT_LE(manager.ResidentCount(), 1u);
            }
        }
        EXPECT_EQ(manager.ResidentCount(), 1u);
    }

    // 容量 8：先驻留 8 页并降到 8，再继续请求其余页，必须持续淘汰且不死循环。
    {
        VL::TerrainPageManager manager;
        manager.SetGpuEpochContext(0, 0, 0);
        const VL::TerrainBuildPlan plan = MakePlan("P31StressEight", 4);
        const std::string path = plan.placementId;
        manager.RegisterTerrain(plan, false);

        for (uint32_t tileZ = 0; tileZ < 2; ++tileZ)
        {
            for (uint32_t tileX = 0; tileX < 4; ++tileX)
            {
                ASSERT_TRUE(manager.RequestPage(path, tileX, tileZ, tileX + tileZ));
            }
        }
        ASSERT_EQ(manager.ResidentCount(), 8u);
        manager.SetResidentCapacityAtEpoch(8, 1000);
        EXPECT_EQ(manager.ResidentCapacity(), 8u);
        EXPECT_EQ(manager.ResidentCount(), 8u);

        uint32_t requestIndex = 0;
        for (uint32_t tileZ = 2; tileZ < 4; ++tileZ)
        {
            for (uint32_t tileX = 0; tileX < 4; ++tileX)
            {
                ASSERT_TRUE(manager.RequestPageAtEpoch(
                    path, tileX, tileZ, requestIndex, 0, 1000 + requestIndex));
                ++requestIndex;
                EXPECT_LE(manager.ResidentCount(), 8u);
            }
        }
        EXPECT_EQ(manager.ResidentCount(), 8u);
    }
}
