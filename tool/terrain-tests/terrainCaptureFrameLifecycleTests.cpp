#include <gtest/gtest.h>
#include "render/frame/terrainCaptureFrameLifecycle.h"
#include "render/resource/resourceRetireQueue.h"
#include <vector>

namespace VL
{
namespace
{
class ObservedHooks final : public ITerrainCaptureHooks
{
public:
    std::vector<uint64_t> submitted;
    std::vector<uint64_t> consumed;
    unsigned after = 0;
    unsigned aborted = 0;
    unsigned closed = 0;
    bool throwSubmit = false;
    bool throwConsume = false;
    std::shared_ptr<const void> storage = std::make_shared<int>(99);
    bool RequiresTesVertexStores() const noexcept override { return true; }
    void BeforeGeometry(PassRuntimeContext&, const TerrainCaptureFrameInfo&) override {}
    void AfterGeometry(vk::CommandBuffer) override { ++after; }
    std::shared_ptr<PipelineBase> SelectActualDraw(const TerrainCaptureDrawInputs&) override { return {}; }
    void BindActualDraw(vk::CommandBuffer, const TerrainCaptureDrawInputs&, const PipelineBase&) override {}
    void ActualDrawRecorded(const TerrainCaptureDrawInputs&) override {}
    std::shared_ptr<const void> Submitted(uint64_t epoch) override
    {
        submitted.push_back(epoch);
        if (throwSubmit)
            throw std::runtime_error("native callback submission failure");
        return storage;
    }
    void AbortUnsubmitted() override { ++aborted; }
    void ConsumeCompleted(uint64_t epoch) override
    {
        consumed.push_back(epoch);
        if (throwConsume)
            throw std::runtime_error("native readback decode failed");
    }
    void CloseAfterDeviceIdle() override { ++closed; }
};

class FrameLifecycleTest : public ::testing::Test
{
protected:
    TerrainCaptureFrameLifecycle lifecycle;
    std::shared_ptr<ObservedHooks> hooks = std::make_shared<ObservedHooks>();
    std::shared_ptr<const void> frame = std::make_shared<int>(7);
    std::shared_ptr<const void> graph = std::make_shared<int>(8);
    std::shared_ptr<TerrainCaptureFrameLifecycle::Flight> flight;
    TerrainCaptureFrameInfo info;
    unsigned before = 0;

    void SetUp() override
    {
        flight = lifecycle.Prepare(1, 93, hooks, frame, graph);
        info.submissionEpoch = 93;
        info.immutableFrameOwner = frame;
        info.immutableGraphOwner = graph;
    }
    void Geometry()
    {
        lifecycle.BeginGeometry(flight, info, [&] { ++before; });
        lifecycle.AfterGeometry(flight, {});
    }
    void Accepted()
    {
        Geometry();
        lifecycle.SubmitAttempted(flight);
        lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 93);
    }
};

TEST_F(FrameLifecycleTest, BeforeAndAfterCallOrderPrecedesActualSubmitAndCompletedFence)
{
    Geometry();
    EXPECT_EQ(before, 1u);
    EXPECT_EQ(hooks->after, 1u);
    EXPECT_TRUE(hooks->submitted.empty());
    lifecycle.SubmitAttempted(flight);
    lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 93);
    EXPECT_EQ(hooks->submitted, std::vector<uint64_t>{93});
    lifecycle.Complete(92);
    EXPECT_TRUE(hooks->consumed.empty());
    EXPECT_TRUE(lifecycle.Pending(1));
    lifecycle.Complete(93);
    EXPECT_EQ(hooks->consumed, std::vector<uint64_t>{93});
    EXPECT_FALSE(lifecycle.Pending(1));
}

TEST_F(FrameLifecycleTest, ActualOwnerIdentityAndEpochCannotBeReplaced)
{
    ++info.submissionEpoch;
    EXPECT_THROW(lifecycle.BeginGeometry(flight, info, [] {}), std::logic_error);
    --info.submissionEpoch;
    info.immutableGraphOwner = std::make_shared<int>(9);
    EXPECT_THROW(lifecycle.BeginGeometry(flight, info, [] {}), std::logic_error);
    EXPECT_FALSE(flight->geometryOpen);
}

TEST_F(FrameLifecycleTest, GeometryCopyAndSubmitCannotBeReorderedOrDuplicated)
{
    EXPECT_THROW(lifecycle.AfterGeometry(flight, {}), std::logic_error);
    EXPECT_THROW(lifecycle.SubmitAttempted(flight), std::logic_error);
    Geometry();
    EXPECT_THROW(lifecycle.BeginGeometry(flight, info, [] {}), std::logic_error);
    EXPECT_THROW(lifecycle.AfterGeometry(flight, {}), std::logic_error);
    lifecycle.SubmitAttempted(flight);
    EXPECT_THROW(lifecycle.SubmitAttempted(flight), std::logic_error);
}

TEST_F(FrameLifecycleTest, KnownDiscardAbortsOnceAndDoesNotConsumeOrSubmit)
{
    Geometry();
    EXPECT_TRUE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Discarded));
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Discarded));
    EXPECT_EQ(hooks->aborted, 1u);
    EXPECT_TRUE(hooks->submitted.empty());
    lifecycle.Complete(100);
    EXPECT_TRUE(hooks->consumed.empty());
    EXPECT_FALSE(lifecycle.Pending(1));
}

TEST_F(FrameLifecycleTest, RecordingIsNotProofOfUnsubmittedAbort)
{
    Geometry();
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Recording));
    EXPECT_EQ(hooks->aborted, 0u);
    EXPECT_TRUE(lifecycle.Pending(1));
}

TEST_F(FrameLifecycleTest, FailedSubmitCallIsAmbiguousAndNeverIssuesAbortOrFakeSubmit)
{
    Geometry();
    lifecycle.SubmitAttempted(flight);
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Recording));
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Discarded));
    lifecycle.Complete(1000);
    EXPECT_TRUE(lifecycle.Pending(1));
    EXPECT_TRUE(hooks->submitted.empty());
    EXPECT_TRUE(hooks->consumed.empty());
    EXPECT_EQ(hooks->aborted, 0u);
}

TEST_F(FrameLifecycleTest, AcceptedSubmitThenPresentThrowStillReceivesActualEpoch)
{
    Geometry();
    lifecycle.SubmitAttempted(flight);
    lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 93);
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Submitted));
    lifecycle.Complete(97);
    EXPECT_EQ(hooks->submitted, std::vector<uint64_t>{93});
    EXPECT_EQ(hooks->consumed, std::vector<uint64_t>{97});
}

TEST_F(FrameLifecycleTest, WrongBackendDispositionOrEpochNeverMarksAccepted)
{
    Geometry();
    lifecycle.SubmitAttempted(flight);
    EXPECT_THROW(lifecycle.Submitted(flight, AcquiredFramePhase::Recording, 93), std::logic_error);
    EXPECT_THROW(lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 94), std::logic_error);
    EXPECT_FALSE(flight->accepted);
    EXPECT_TRUE(hooks->submitted.empty());
}

TEST_F(FrameLifecycleTest, AcceptedOwnerLivesUntilFenceAndCallbackIsOnlyOnce)
{
    Accepted();
    std::weak_ptr<const void> storage = hooks->storage;
    hooks->storage.reset();
    lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 93);
    EXPECT_EQ(hooks->submitted.size(), 1u);
    flight.reset();
    lifecycle.Complete(92);
    EXPECT_FALSE(storage.expired());
    lifecycle.Complete(93);
    EXPECT_TRUE(storage.expired());
}

TEST_F(FrameLifecycleTest, SlotCannotBeReusedUntilItsOwnActualFence)
{
    Accepted();
    EXPECT_THROW(lifecycle.Prepare(1, 94, hooks, frame, graph), std::logic_error);
    lifecycle.Complete(92);
    EXPECT_THROW(lifecycle.Prepare(1, 94, hooks, frame, graph), std::logic_error);
    lifecycle.Complete(93);
    EXPECT_NO_THROW(lifecycle.Prepare(1, 94, hooks, frame, graph));
}

TEST_F(FrameLifecycleTest, SubmittedCallbackThrowRetainsOwnersAndNeverFallsBackToAbort)
{
    Geometry();
    lifecycle.SubmitAttempted(flight);
    hooks->throwSubmit = true;
    EXPECT_THROW(lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 93), std::runtime_error);
    EXPECT_TRUE(flight->accepted);
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Submitted));
    lifecycle.Submitted(flight, AcquiredFramePhase::Submitted, 93);
    EXPECT_EQ(hooks->submitted.size(), 1u);
    EXPECT_TRUE(flight->frameOwner);
    EXPECT_TRUE(flight->graphOwner);
}

TEST_F(FrameLifecycleTest, ConsumeFailurePropagatesWithoutEarlyOwnerRelease)
{
    Accepted();
    hooks->throwConsume = true;
    EXPECT_THROW(lifecycle.Complete(93), std::runtime_error);
    EXPECT_TRUE(lifecycle.Pending(1));
    EXPECT_TRUE(flight->submittedOwner);
}

TEST_F(FrameLifecycleTest, IncompleteOrOutOfBoundRequestsAreRejected)
{
    EXPECT_THROW(lifecycle.Prepare(8, 94, hooks, frame, graph), std::logic_error);
    EXPECT_THROW(lifecycle.Prepare(2, 0, hooks, frame, graph), std::logic_error);
    EXPECT_THROW(lifecycle.Prepare(2, 94, {}, frame, graph), std::logic_error);
    EXPECT_THROW(lifecycle.Prepare(2, 94, hooks, {}, graph), std::logic_error);
    EXPECT_THROW(lifecycle.Prepare(2, 94, hooks, frame, {}), std::logic_error);
}

TEST_F(FrameLifecycleTest, GeometryThrowCanAbortOnlyAfterActualDiscard)
{
    EXPECT_THROW(lifecycle.BeginGeometry(flight, info, [] {
        throw std::runtime_error("actual recording failure");
    }), std::runtime_error);
    EXPECT_TRUE(flight->geometryOpen);
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Recording));
    EXPECT_TRUE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Discarded));
    EXPECT_EQ(hooks->aborted, 1u);
}

TEST_F(FrameLifecycleTest, AmbiguousStorageReleasesOnlyAtExplicitDeviceIdleTeardown)
{
    Geometry();
    lifecycle.SubmitAttempted(flight);
    EXPECT_FALSE(lifecycle.AbortProvenUnsubmitted(flight, AcquiredFramePhase::Recording));
    lifecycle.CloseAfterDeviceIdle();
    EXPECT_EQ(hooks->closed, 1u);
    EXPECT_FALSE(lifecycle.Pending(1));
    EXPECT_TRUE(hooks->submitted.empty());
    EXPECT_EQ(hooks->aborted, 0u);
}

TEST(TerrainCaptureFrameRetirement, ActualRetireQueueKeepsAmbiguousOwnerUntilRealEpochCollection)
{
    auto& queue = ResourceRetireQueue::GetInstance();
    auto hooks = std::make_shared<ObservedHooks>();
    std::weak_ptr<const void> weak;
    {
        TerrainCaptureFrameLifecycle state;
        auto owner = std::make_shared<int>(42);
        auto frame = state.Prepare(0, 501, hooks, owner, owner);
        weak = owner;
        queue.RetireShared("TES-CPU-test-actual-queue", 7, 501, frame);
        TerrainCaptureFrameInfo info;
        info.submissionEpoch = 501;
        info.immutableFrameOwner = owner;
        info.immutableGraphOwner = owner;
        state.BeginGeometry(frame, info, [] {});
        state.AfterGeometry(frame, {});
        state.SubmitAttempted(frame);
    }
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(hooks->aborted, 0u);
    queue.CollectCompletedEpoch(500);
    EXPECT_FALSE(weak.expired());
    queue.CollectCompletedEpoch(501);
    EXPECT_TRUE(weak.expired());
}

TEST(TerrainCaptureFrameRetirement, ConfirmedIdleClearsQueuedStorageBeforeDeferredQueueNodeRetires)
{
    auto& queue = ResourceRetireQueue::GetInstance();
    TerrainCaptureFrameLifecycle state;
    auto hooks = std::make_shared<ObservedHooks>();
    auto owner = std::make_shared<int>(42);
    auto flight = state.Prepare(0, 601, hooks, owner, owner);
    std::weak_ptr<const void> weak = owner;
    queue.RetireShared("TES-CPU-idle-actual-queue", 7, 601, flight);
    owner.reset();
    EXPECT_FALSE(weak.expired());
    state.CloseAfterDeviceIdle();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(hooks->closed, 1u);
    EXPECT_TRUE(queue.FindPendingResourceForTest("TES-CPU-idle-actual-queue", 7).lock());
    queue.CollectCompletedEpoch(601);
}
}
}
