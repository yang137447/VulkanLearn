#include <gtest/gtest.h>

#include <vector>

#include "render/backend/frameEpochRing.h"

TEST(FrameEpochRing, EmptySlotReportsNoCompletedEpoch)
{
    std::vector<uint64_t> epochs = {0, 0};
    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 0), 0u);
    EXPECT_EQ(epochs[0], 0u);
}

TEST(FrameEpochRing, CompletedEpochIsConsumedExactlyOnce)
{
    std::vector<uint64_t> epochs = {0, 0};
    VL::RecordSubmittedFrameEpoch(epochs, 1, 7);
    EXPECT_EQ(epochs[1], 7u);

    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 1), 7u);
    EXPECT_EQ(epochs[1], 0u);
    // 同一槽再次 BeginFrame 时没有旧 epoch，不能重复消费。
    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 1), 0u);
}

TEST(FrameEpochRing, SlotsAreIndependentAndOutOfRangeIsSafe)
{
    std::vector<uint64_t> epochs = {0, 0};
    VL::RecordSubmittedFrameEpoch(epochs, 0, 3);
    VL::RecordSubmittedFrameEpoch(epochs, 1, 4);

    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 0), 3u);
    EXPECT_EQ(epochs[1], 4u);
    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 2), 0u);
    EXPECT_EQ(VL::ConsumeCompletedFrameEpoch(epochs, 99), 0u);
}
