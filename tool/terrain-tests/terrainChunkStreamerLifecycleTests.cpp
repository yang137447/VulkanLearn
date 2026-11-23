#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "engine/terrainChunkStreamer.h"

namespace VL
{
struct TerrainChunkStreamerTestAccess
{
    using Placement = TerrainChunkStreamer::Placement;
    using BuildResult = TerrainChunkStreamer::BuildResult;

    static bool Finish(Placement& placement, const BuildResult& result)
    {
        return TerrainChunkStreamer::FinishBuild(placement, result);
    }

    static bool Dynamic(const TerrainBuildPlan& plan, bool authoring)
    {
        return TerrainChunkStreamer::UsesDynamicVertices(plan, authoring);
    }

    static void CheckWorkerHeightReplacement()
    {
        TerrainChunkStreamer streamer;
        TerrainBuildPlan plan;
        plan.placementId = "terrain|Test";
        plan.desc.gridResolution = 4;
        plan.desc.chunkQuads = 2;
        plan.desc.lodLevels = 2;
        plan.desc.size = Eigen::Vector2f(100.0f, 100.0f);
        auto height = std::make_shared<HostImage>();
        height->width = 4;
        height->height = 4;
        height->channels = 1;
        height->format = HostImage::PixelFormat::R32_FLOAT;
        height->data.resize(16 * sizeof(float));
        plan.heightmap = height;
        streamer.placements.emplace_back();
        auto& placement = streamer.placements.front();
        placement.plan = plan;
        placement.placementId = plan.placementId;
        placement.heightRevision = 1;
        streamer.epoch = 5;
        const TerrainChunkId id{0, 0, 0};
        streamer.RequestBuild(0, id);
        auto nextHeight = std::make_shared<const HostImage>(*height);
        plan.heightmap = nextHeight;
        streamer.RefreshHeightSnapshot(placement, plan);
        EXPECT_EQ(placement.heightRevision, 2u);
        EXPECT_EQ(placement.plan.heightmap, nextHeight);
        streamer.RefreshHeightSnapshot(placement, plan);
        EXPECT_EQ(placement.heightRevision, 2u);

        const auto waitForResult = [&]() -> bool
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                streamer.PollCompletedBuilds();
                if (!streamer.readyUploads.empty())
                    return true;
                std::this_thread::yield();
            }
            return false;
        };
        ASSERT_TRUE(waitForResult());
        auto old = std::move(streamer.readyUploads.front());
        streamer.readyUploads.pop_front();
        EXPECT_EQ(old.epoch, 5u);
        EXPECT_EQ(old.heightmap, height);
        EXPECT_EQ(old.heightRevision, 1u);
        ASSERT_TRUE(old.error.empty()) << old.error;
        EXPECT_FALSE(Finish(placement, old));
        EXPECT_EQ(placement.building.count(id), 0u);

        streamer.RequestBuild(0, id);
        ASSERT_TRUE(waitForResult());
        auto current = std::move(streamer.readyUploads.front());
        streamer.readyUploads.pop_front();
        EXPECT_EQ(current.heightmap, nextHeight);
        EXPECT_EQ(current.heightRevision, 2u);
        ASSERT_TRUE(current.error.empty()) << current.error;
        EXPECT_EQ(current.chunks.size(), 1u);
        EXPECT_TRUE(Finish(placement, current));
        EXPECT_TRUE(placement.building.empty());
    }
};
}

namespace
{
using Access = VL::TerrainChunkStreamerTestAccess;

TEST(TerrainChunkStreamerLifecycle, DynamicVerticesRequireCompositedAuthoring)
{
    VL::TerrainBuildPlan plan;
    EXPECT_FALSE(Access::Dynamic(plan, false));
    EXPECT_FALSE(Access::Dynamic(plan, true));
    plan.desc.compositedVT.enabled = true;
    EXPECT_FALSE(Access::Dynamic(plan, false));
    EXPECT_TRUE(Access::Dynamic(plan, true));
}

TEST(TerrainChunkStreamerLifecycle, OldHeightResultReleasesItsRequestForRetry)
{
    Access::Placement placement;
    placement.plan.heightmap = std::make_shared<const HostImage>();
    placement.heightRevision = 2;
    const VL::TerrainChunkId id{0, 0, 0};
    placement.building[id] = 1;
    Access::BuildResult result;
    result.id = id;
    result.heightRevision = 1;
    result.heightmap = std::make_shared<const HostImage>();
    std::weak_ptr<const HostImage> old = result.heightmap;
    EXPECT_FALSE(Access::Finish(placement, result));
    EXPECT_FALSE(old.expired());
    EXPECT_TRUE(placement.building.empty());
    placement.building[id] = 2;
    result.heightRevision = 2;
    result.heightmap = placement.plan.heightmap;
    EXPECT_TRUE(old.expired());
    EXPECT_TRUE(Access::Finish(placement, result));
    EXPECT_TRUE(placement.building.empty());
}

TEST(TerrainChunkStreamerLifecycle, OldResultCannotEraseNewerRequestOrPassPointerOnlyCheck)
{
    Access::Placement placement;
    placement.plan.heightmap = std::make_shared<const HostImage>();
    placement.heightRevision = 3;
    const VL::TerrainChunkId id{0, 0, 0};
    placement.building[id] = 3;
    Access::BuildResult result;
    result.id = id;
    result.heightRevision = 1;
    result.heightmap = placement.plan.heightmap;
    EXPECT_FALSE(Access::Finish(placement, result));
    EXPECT_EQ(placement.building.at(id), 3u);
    result.heightRevision = 3;
    EXPECT_TRUE(Access::Finish(placement, result));
    EXPECT_FALSE(Access::Finish(placement, result));
}

TEST(TerrainChunkStreamerLifecycle, ActualWorkerRejectsChangedHeightThenBuildsCurrentSnapshot)
{
    Access::CheckWorkerHeightReplacement();
}
}
