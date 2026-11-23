#include "render/frame/terrainCompositedQualityValidation.h"
#include "render/frame/terrainCompositedFrameLifetime.h"
#include "render/rendergraph/renderGraphDepthOwner.h"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace
{
VL::TerrainQualityAbi::Counts PassingCounts()
{
    VL::TerrainQualityAbi::Counts counts;
    counts.visibleSamples = counts.validAtTargetPlusBias = 100;
    counts.complete = true;
    return counts;
}

class QualityReadiness : public testing::Test
{
protected:
    VL::TerrainQualityValidation state;
    Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    VL::TerrainQualityAbi::Surface surface{};
    VL::TerrainQualityValidationStamp stamp;

    void SetUp() override
    {
        surface.width = surface.height = 8192;
        surface.maxMipAndRepeat = 13;
        surface.generationLow = 7;
        surface.revisionLow = 3;
        state.Begin(7, 9, camera, {}, 100);
        stamp = state.Resources(&surface, 1, 110);
    }

    void Accept()
    {
        ASSERT_TRUE(state.Observe(stamp, camera, 20, 20, 200, PassingCounts(), true));
    }
};

TEST_F(QualityReadiness, NoEvidenceFailsAndRepeatedQueriesDoNotConsume)
{
    EXPECT_FALSE(state.Ready(7, camera, {}));
    EXPECT_EQ(state.PoseArrivedMonotonicMs(), 100);
    Accept();
    for (size_t query = 0; query < 100; ++query)
        EXPECT_TRUE(state.Ready(7, camera, {}));
    EXPECT_FALSE(state.Ready(8, camera, {}));
}

TEST_F(QualityReadiness, ExplicitTeleportRevokesBeforeFrameAndRejectsLateFence)
{
    Accept();
    const VL::TerrainQualityInvalidation teleport{7, 1, 250};
    EXPECT_FALSE(state.Ready(7, camera, teleport));
    const auto arrived = state.Begin(7, 9, camera, teleport, 900);
    EXPECT_EQ(state.PoseArrivedMonotonicMs(), 250);
    EXPECT_NE(stamp.poseId, arrived.poseId);
    EXPECT_FALSE(state.Observe(stamp, camera, 21, 21, 901, PassingCounts(), true));
    EXPECT_FALSE(state.Ready(7, camera, teleport));
    EXPECT_TRUE(state.Observe(arrived, camera, 22, 22, 902, PassingCounts(), true));
    EXPECT_TRUE(state.Ready(7, camera, teleport));
}

TEST_F(QualityReadiness, SmallContinuousMotionDoesNotRevokeEveryFrame)
{
    Accept();
    Eigen::Matrix4f moved = camera;
    for (size_t frame = 0; frame < 20; ++frame)
    {
        moved(0, 3) += 0.001f;
        EXPECT_EQ(state.Begin(7, 9, moved, {}, 210 + frame).poseId, stamp.poseId);
        EXPECT_TRUE(state.Ready(7, moved, {}));
    }
    moved(0, 3) = 0.5f;
    EXPECT_FALSE(state.Ready(7, moved, {}));
    EXPECT_NE(state.Begin(7, 9, moved, {}, 300).poseId, stamp.poseId);
    EXPECT_FALSE(state.Ready(7, moved, {}));
}

TEST_F(QualityReadiness, RevisionAndDigestChangeRejectHistoricalSuccess)
{
    Accept();
    ++surface.revisionLow;
    surface.identity[0] = 1234;
    const auto changed = state.Resources(&surface, 1, 300);
    EXPECT_NE(changed.resourceVersion, stamp.resourceVersion);
    EXPECT_FALSE(state.Ready(7, camera, {}));
    EXPECT_FALSE(state.Observe(stamp, camera, 21, 21, 301, PassingCounts(), true));
    EXPECT_TRUE(state.Observe(changed, camera, 22, 22, 302, PassingCounts(), true));
}

TEST_F(QualityReadiness, ReplacedPageTableRejectsOldCaptureEvenWithIdenticalDigest)
{
    Accept();
    const auto changed = state.BindingsChanged(camera, 250);
    EXPECT_EQ(state.PoseArrivedMonotonicMs(), 250);
    EXPECT_FALSE(state.Ready(7, camera, {}));
    EXPECT_FALSE(state.Observe(stamp, camera, 21, 21, 301, PassingCounts(), true));
    EXPECT_TRUE(state.Observe(changed, camera, 22, 22, 302, PassingCounts(), true));
}

TEST_F(QualityReadiness, FailureAndUncompletedSubmissionCannotReleaseLoading)
{
    EXPECT_FALSE(state.Observe(stamp, camera, 20, 19, 200, PassingCounts(), true));
    EXPECT_FALSE(state.Observe(stamp, camera, 20, 20, 200, PassingCounts(), false));
    EXPECT_FALSE(state.Ready(7, camera, {}));
    auto failed = PassingCounts();
    failed.unresolved = 1;
    EXPECT_FALSE(state.Observe(stamp, camera, 21, 21, 201, failed, true));
    EXPECT_FALSE(state.Ready(7, camera, {}));
}

TEST_F(QualityReadiness, WorldAndGraphReplacementDiscardPriorEvidence)
{
    Accept();
    state.Begin(7, 10, camera, {}, 300);
    EXPECT_FALSE(state.Ready(7, camera, {}));
    EXPECT_FALSE(state.Observe(stamp, camera, 21, 21, 301, PassingCounts(), true));
    state.Begin(8, 11, camera, {}, 302);
    EXPECT_FALSE(state.Ready(7, camera, {}));
    EXPECT_FALSE(state.Ready(8, camera, {}));
}

TEST_F(QualityReadiness, ConcurrentReadersRetainPersistentState)
{
    Accept();
    std::atomic<bool> success{true};
    std::thread reader([&] {
        for (size_t query = 0; query < 1000; ++query)
            if (!state.Ready(7, camera, {}))
                success.store(false);
    });
    for (uint64_t epoch = 21; epoch < 100; ++epoch)
        EXPECT_TRUE(state.Observe(stamp, camera, epoch, epoch, 200 + epoch, PassingCounts(), true));
    reader.join();
    EXPECT_TRUE(success.load());
}

TEST(QualityFrameLifetime, LoadingAndCaptureOwnersWaitForExactFence)
{
    VL::TerrainCompositedFrameLifetime frames;
    auto quality = std::make_shared<int>(1);
    auto loading = std::make_shared<int>(2);
    std::weak_ptr<int> qualityWeak = quality;
    std::weak_ptr<int> loadingWeak = loading;
    frames.Submitted(0, 20, quality, loading);
    quality.reset();
    loading.reset();
    frames.Complete(19);
    EXPECT_FALSE(qualityWeak.expired());
    EXPECT_FALSE(loadingWeak.expired());
    EXPECT_THROW(frames.Prepare(0), std::runtime_error);
    frames.Complete(20);
    EXPECT_TRUE(qualityWeak.expired());
    EXPECT_TRUE(loadingWeak.expired());
    EXPECT_NO_THROW(frames.Prepare(0));
}

TEST(QualityFrameLifetime, CompletingAndDroppingReadbackAllowsNextCaptureSlot)
{
    VL::TerrainCompositedFrameLifetime frames;
    frames.Submitted(0, 20, std::make_shared<int>(1), {});
    frames.Submitted(1, 21, std::make_shared<int>(2), {});
    frames.Complete(20);
    EXPECT_NO_THROW(frames.Submitted(0, 22, std::make_shared<int>(3), {}));
    EXPECT_THROW(frames.Prepare(1), std::runtime_error);
    frames.Complete(22);
    EXPECT_NO_THROW(frames.Prepare(0));
    EXPECT_NO_THROW(frames.Prepare(1));
}

TEST_F(QualityReadiness, PerspectiveWorldSpaceMicroMotionDoesNotUseNearPlanePixels)
{
    Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
    projection(0, 0) = projection(1, 1) = 1.7f;
    projection(2, 2) = -1.00001f;
    projection(2, 3) = -0.100001f;
    projection(3, 2) = -1.f;
    Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
    view(0, 3) = -150000;
    const Eigen::Matrix4f reference = projection * view;
    view(0, 3) -= 2;
    EXPECT_TRUE(VL::TerrainQualityViewWithinValidatedDomain(reference, projection * view));
    view(0, 3) -= 10000;
    EXPECT_FALSE(VL::TerrainQualityViewWithinValidatedDomain(reference, projection * view));
}

TEST(QualityDepthOwner, LeafSurvivesGraphReleaseUntilCaptureDrops)
{
    const vk::Image image(reinterpret_cast<VkImage>(uintptr_t(0x1234)));
    auto leaf = std::make_shared<int>(42);
    std::weak_ptr<int> weak = leaf;
    VL::RegisterRenderGraphDepthOwner(image, leaf);
    leaf.reset();
    auto capture = VL::CaptureRenderGraphDepthOwner(image);
    EXPECT_TRUE(capture);
    auto retired = VL::ReleaseRenderGraphDepthOwner(image);
    EXPECT_FALSE(VL::CaptureRenderGraphDepthOwner(image));
    retired.reset();
    EXPECT_FALSE(weak.expired());
    capture.reset();
    EXPECT_TRUE(weak.expired());
}
}
