#include <gtest/gtest.h>

#include "render/backend/swapchainFrameResult.h"

namespace VL
{

TEST(SwapchainFrameResult, SuccessContinuesWithoutRebuild)
{
    const auto result = ClassifySwapchainFrameResult(vk::Result::eSuccess);
    EXPECT_EQ(result, SwapchainFrameResult::Ready);
    EXPECT_FALSE(RequiresSwapchainRecreate(result));
    EXPECT_TRUE(HasAcquiredSwapchainImage(result));
}

TEST(SwapchainFrameResult, SuboptimalUsesAcquiredImageThenRequestsRebuild)
{
    const auto result = ClassifySwapchainFrameResult(vk::Result::eSuboptimalKHR);
    EXPECT_EQ(result, SwapchainFrameResult::Suboptimal);
    EXPECT_TRUE(RequiresSwapchainRecreate(result));
    EXPECT_TRUE(HasAcquiredSwapchainImage(result));
}

TEST(SwapchainFrameResult, OutOfDateSkipsAcquireOrEndsPresentAndRequestsRebuild)
{
    const auto result = ClassifySwapchainFrameResult(vk::Result::eErrorOutOfDateKHR);
    EXPECT_EQ(result, SwapchainFrameResult::OutOfDate);
    EXPECT_TRUE(RequiresSwapchainRecreate(result));
    EXPECT_FALSE(HasAcquiredSwapchainImage(result));
}

TEST(SwapchainFrameResult, OtherErrorsRemainFatal)
{
    EXPECT_THROW(
        ClassifySwapchainFrameResult(vk::Result::eErrorDeviceLost),
        std::runtime_error);
}

} // namespace VL
