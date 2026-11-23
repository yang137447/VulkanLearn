#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "render/backend/frameAbandon.h"
#include "render/backend/frameEpochRing.h"
#include "render/renderThread.h"
#include "render/frame/frameSubmitFault.h"
#include "render/resource/terrainCompositedWorldBindings.h"

TEST(FrameAbandon, RecordingCannotBeRecoveredOrDisposedTwice)
{
    VL::AcquiredFrameDisposition state;
    EXPECT_THROW(state.RecoverySubmitted(), std::logic_error);
    EXPECT_THROW(state.Recovered(), std::logic_error);
    state.Discarded();
    EXPECT_THROW(state.Submitted(), std::logic_error);
    EXPECT_THROW(state.Discarded(), std::logic_error);
}

TEST(FrameAbandon, RecoveryOrdersRecordingFenceResetSubmitAndPresent)
{
    VL::AcquiredFrameDisposition state;
    state.Discarded();
    std::vector<int> steps;
    const auto result = VL::CompleteDiscardedFrame(state,
        [&] { steps.push_back(1); }, [&] { steps.push_back(2); },
        [&] { steps.push_back(3); }, [&] { steps.push_back(4); return 7; });
    EXPECT_EQ(result, 7);
    EXPECT_EQ(steps, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_EQ(state.Phase(), VL::AcquiredFramePhase::Recovered);
    EXPECT_THROW(state.Submitted(), std::logic_error);
    EXPECT_THROW(VL::CompleteDiscardedFrame(state, [] {}, [] {}, [] {}, [] { return 0; }),
        std::logic_error);
}

TEST(FrameAbandon, RealDeviceLostIsNeverConvertedToRecoverableAbort)
{
    VL::AcquiredFrameDisposition state;
    state.Discarded();
    bool presented = false;
    EXPECT_THROW(VL::CompleteDiscardedFrame(state, [] {}, [] {},
        [] { throw vk::DeviceLostError("CPU exception propagation test"); },
        [&] { presented = true; return 0; }), vk::DeviceLostError);
    EXPECT_FALSE(presented);
    EXPECT_EQ(state.Phase(), VL::AcquiredFramePhase::Discarded);
}

TEST(FrameAbandon, PresentFailureCannotDeclareRecovery)
{
    VL::AcquiredFrameDisposition state;
    state.Discarded();
    EXPECT_THROW(VL::CompleteDiscardedFrame(state, [] {}, [] {}, [] {},
        []() -> int { throw std::runtime_error("present failed"); }), std::runtime_error);
    EXPECT_EQ(state.Phase(), VL::AcquiredFramePhase::RecoverySubmitted);
}

TEST(FrameAbandon, RecoveryEpochAndNextSceneEpochRemainDistinct)
{
    std::vector<uint64_t> epochs(2);
    VL::RecordSubmittedFrameEpoch(epochs, 0, 31);
    VL::RecordSubmittedFrameEpoch(epochs, 1, 32);
    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 0), 31u);
    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 1), 32u);
}

TEST(FrameAbandon, ExplicitRecoveredAbortReleasesBudgetAndNextMailboxSlot)
{
    VL::RenderFrameBudget budget;
    budget.Publish();
    ASSERT_TRUE(budget.BeginRender());
    try
    {
        throw VL::TaskFrameAborted({31, 0, 1, false}, "cpu-explicit-abort");
    }
    catch (const VL::TaskFrameAborted&)
    {
        budget.EndRender();
    }
    EXPECT_TRUE(budget.IsIdle());
    budget.Publish();
    ASSERT_TRUE(budget.BeginRender());
    budget.EndRender();
    EXPECT_EQ(budget.GetCompletedFrameSerial(), 2u);
}

TEST(FrameAbandon, WorkerExceptionPolicyIsThreadLocalAndRestored)
{
    EXPECT_FALSE(VL::taskFrameWorkerActive);
    std::thread worker([] {
        EXPECT_FALSE(VL::taskFrameWorkerActive);
        {
            VL::TaskFrameWorkerScope scope;
            EXPECT_TRUE(VL::taskFrameWorkerActive);
        }
        EXPECT_FALSE(VL::taskFrameWorkerActive);
    });
    worker.join();
    EXPECT_FALSE(VL::taskFrameWorkerActive);
}

TEST(FrameAbandon, NormalSubmissionCannotBeAbandoned)
{
    VL::AcquiredFrameDisposition state;
    state.Submitted();
    EXPECT_THROW(state.Discarded(), std::logic_error);
    EXPECT_THROW(state.Submitted(), std::logic_error);
}

TEST(FrameAbandon, RealFrameHelpersLinkWithControllerAndDefaultMissingProviderDoesNotArm)
{
    VL::SetFrameSubmitFaultProvider({});
    EXPECT_FALSE(VL::TakeFrameSubmitFault({}, 42, 73));
    VL::ObservePostAbortSubmittedFrame(42, 74);
    VL::ObservePostAbortCompletedFrame(74);
    const VL::FrameSubmitFault noOwner;
    EXPECT_THROW(VL::ObserveRecoveredFrameAbort(noOwner, {73, 0, 0, false}), std::bad_function_call);
}

TEST(DescriptorFrameRecovery, OnlyAllocationFailuresBecomeBoundedImageRetry)
{
    try
    {
        VL::RunWorldDescriptorOperation(51, 27, 2, "descriptor-write", [] { throw std::bad_alloc(); });
        FAIL();
    }
    catch (const VL::WorldBindingDescriptorRetry& error)
    {
        EXPECT_EQ(error.worldGeneration, 51u);
        EXPECT_EQ(error.bindingRevision, 27u);
        EXPECT_EQ(error.imageIndex, 2u);
        EXPECT_STREQ(error.operation, "descriptor-write");
        EXPECT_EQ(error.cause, VL::WorldDescriptorRetryCause::HostAllocation);
    }
}

TEST(DescriptorFrameRecovery, DeviceLostAndStructuralRuntimeErrorsRemainFatalTypes)
{
    EXPECT_THROW(VL::RunWorldDescriptorOperation(1, 2, 0, "descriptor-write",
        [] { throw vk::DeviceLostError("CPU type propagation"); }), vk::DeviceLostError);
    EXPECT_THROW(VL::RunWorldDescriptorOperation(1, 2, 0, "descriptor-write",
        [] { throw std::runtime_error("Structural descriptor mismatch"); }), std::runtime_error);
    EXPECT_THROW(VL::RunWorldDescriptorOperation(1, 2, 0, "descriptor-write",
        [] { throw std::logic_error("Invalid handle"); }), std::logic_error);
    EXPECT_THROW(VL::RunWorldDescriptorOperation(1, 2, 0, "descriptor-write",
        [] { throw 7; }), int);
}

TEST(DescriptorFrameRecovery, VulkanAllocationKeepsOriginalResultAndExactBindingIdentity)
{
    try
    {
        VL::RunWorldDescriptorOperation(19, 888, 1, "descriptor-write",
            [] { throw vk::OutOfHostMemoryError("CPU classification only"); });
        FAIL();
    }
    catch (const VL::WorldBindingDescriptorRetry& error)
    {
        EXPECT_EQ(error.originalResult, static_cast<int32_t>(vk::Result::eErrorOutOfHostMemory));
        EXPECT_EQ(error.cause, VL::WorldDescriptorRetryCause::VulkanAllocation);
        EXPECT_EQ(error.worldGeneration, 19u);
        EXPECT_EQ(error.bindingRevision, 888u);
        EXPECT_EQ(error.imageIndex, 1u);
    }
}

TEST(DescriptorFrameRecovery, AdmissionRetryIsExplicitAndDoesNotChangeAnyCap)
{
    EXPECT_THROW(VL::RunWorldDescriptorOperation(1, 3, 2, "registry-prepare", [] {
        throw VL::WorldBindingDescriptorRetry(VL::WorldDescriptorRetryCause::CpuAdmission);
    }), VL::WorldBindingDescriptorRetry);
    VL::RenderFrameBudget budget;
    budget.Publish();
    ASSERT_TRUE(budget.BeginRender());
    try { throw VL::DescriptorFrameSkipped({9, 0, 1, false}); }
    catch (const VL::DescriptorFrameSkipped&) { budget.EndRender(); }
    EXPECT_TRUE(budget.CanPublish());
}
