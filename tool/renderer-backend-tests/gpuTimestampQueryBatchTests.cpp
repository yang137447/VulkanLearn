#include <gtest/gtest.h>

#include "render/backend/gpuTimestampQueryBatch.h"

// 这些测试固定“timestamp query 批次只有随成功提交才可读回”的状态机。
// 被废弃帧的 query 从未在 GPU 上执行；若仍按已提交批次读回，会得到
// VK_NOT_READY 并中断渲染线程（R16 submitFail 恢复帧的实际故障）。

TEST(GpuTimestampQueryBatch, CompleteRecordedBatchIsReadableOnlyAfterSubmit)
{
    VL::GpuTimestampQueryBatch batch(4);
    EXPECT_FALSE(batch.HasSubmitted());

    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(0));
    batch.End(0);
    ASSERT_TRUE(batch.Begin(2));
    batch.End(2);
    EXPECT_TRUE(batch.WasRecorded(0));
    EXPECT_TRUE(batch.WasRecorded(2));
    EXPECT_FALSE(batch.WasRecorded(1));

    batch.Submitted();
    EXPECT_TRUE(batch.HasSubmitted());
    batch.Consumed();
    EXPECT_FALSE(batch.HasSubmitted());
}

TEST(GpuTimestampQueryBatch, DiscardedRecordingIsNeverReadBack)
{
    VL::GpuTimestampQueryBatch batch(4);
    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(0));
    batch.End(0);

    batch.Aborted();
    EXPECT_FALSE(batch.HasSubmitted());
    EXPECT_FALSE(batch.WasRecorded(0));

    batch.Submitted();
    EXPECT_FALSE(batch.HasSubmitted());
}

TEST(GpuTimestampQueryBatch, IncompleteBeginEndPairCannotBeRead)
{
    VL::GpuTimestampQueryBatch batch(4);
    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(1));
    // end 未写：半区间不允许成为可读回批次。
    batch.Submitted();
    EXPECT_FALSE(batch.HasSubmitted());
}

TEST(GpuTimestampQueryBatch, DuplicateBeginIsRejectedAndKeepsFirstPair)
{
    VL::GpuTimestampQueryBatch batch(4);
    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(0));
    EXPECT_FALSE(batch.Begin(0));
    batch.End(0);
    EXPECT_FALSE(batch.Begin(0));
    batch.Submitted();
    EXPECT_TRUE(batch.HasSubmitted());
}

TEST(GpuTimestampQueryBatch, AbortOfCurrentRecordingPreservesEarlierSubmittedBatch)
{
    VL::GpuTimestampQueryBatch batch(4);
    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(0));
    batch.End(0);
    batch.Submitted();
    ASSERT_TRUE(batch.HasSubmitted());

    // 下一帧录制被丢弃，但尚未读回的旧批次仍必须保留。
    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(0));
    batch.Aborted();
    EXPECT_TRUE(batch.HasSubmitted());
    EXPECT_FALSE(batch.WasRecorded(0));
}

TEST(GpuTimestampQueryBatch, NextRecordingStartsWithoutPreviousRecordedFlags)
{
    VL::GpuTimestampQueryBatch batch(4);
    batch.BeginRecording();
    ASSERT_TRUE(batch.Begin(0));
    batch.End(0);
    batch.Submitted();
    batch.Consumed();

    batch.BeginRecording();
    EXPECT_FALSE(batch.WasRecorded(0));
    EXPECT_FALSE(batch.IsOpen(0));
    EXPECT_FALSE(batch.HasSubmitted());
}
