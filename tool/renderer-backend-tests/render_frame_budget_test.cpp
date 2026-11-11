#include <gtest/gtest.h>

#include "render/renderThread.h"

TEST(RenderFrameBudget, AllowsExactlyOneOutstandingFrame)
{
    VL::RenderFrameBudget budget;
    budget.Reset();

    EXPECT_TRUE(budget.CanPublish());
    budget.Publish();
    EXPECT_FALSE(budget.CanPublish());
    EXPECT_EQ(budget.GetPendingFrameCount(), 1u);
    EXPECT_EQ(budget.GetRequestedFrameSerial(), 1u);
    EXPECT_TRUE(budget.HasPendingRequest());
    EXPECT_FALSE(budget.IsRenderBusy());
}

TEST(RenderFrameBudget, PublishWhileBusyCollapsesIntoLatestRequest)
{
    VL::RenderFrameBudget budget;
    budget.Reset();

    budget.Publish();
    ASSERT_TRUE(budget.BeginRender().has_value());
    EXPECT_TRUE(budget.IsRenderBusy());
    EXPECT_EQ(budget.GetPendingFrameCount(), 1u);

    // GT may publish one frame while RT records; it must not grow the budget.
    budget.Publish();
    budget.Publish();
    EXPECT_EQ(budget.GetPendingFrameCount(), 1u);
    EXPECT_EQ(budget.GetRequestedFrameSerial(), 3u);

    budget.EndRender();
    EXPECT_FALSE(budget.IsRenderBusy());
    EXPECT_EQ(budget.GetPendingFrameCount(), 0u);
    EXPECT_EQ(budget.GetCompletedFrameSerial(), 1u);

    // The two requests published during the frame collapse into one rerun that
    // consumes the newest snapshot serial.
    const auto next = budget.BeginRender();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 3u);
    budget.EndRender();
    EXPECT_EQ(budget.GetCompletedFrameSerial(), 3u);
}

TEST(RenderFrameBudget, FrameSlotIsReleasedOnlyAfterEndRender)
{
    VL::RenderFrameBudget budget;
    budget.Reset();

    budget.Publish();
    ASSERT_TRUE(budget.BeginRender().has_value());
    // A frame in flight still owns the slot even though no request is queued.
    EXPECT_FALSE(budget.CanPublish());
    EXPECT_EQ(budget.GetPendingFrameCount(), 1u);

    budget.EndRender();
    EXPECT_TRUE(budget.CanPublish());
    EXPECT_TRUE(budget.IsIdle());
}

TEST(RenderFrameBudget, AbortReleasesEveryWaiter)
{
    VL::RenderFrameBudget budget;
    budget.Reset();

    budget.Publish();
    ASSERT_TRUE(budget.BeginRender().has_value());
    budget.Publish();
    budget.Abort();

    EXPECT_TRUE(budget.IsIdle());
    EXPECT_TRUE(budget.CanPublish());
    EXPECT_EQ(budget.GetPendingFrameCount(), 0u);
    EXPECT_FALSE(budget.HasPendingRequest());
    EXPECT_FALSE(budget.IsRenderBusy());
}
