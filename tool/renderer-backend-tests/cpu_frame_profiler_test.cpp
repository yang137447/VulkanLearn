#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "core/cpuFrameProfiler.h"
#include "core/performanceSnapshot.h"

namespace
{
using VL::BuildPerformanceSnapshot;
using VL::ComputePerformanceCpuOtherMilliseconds;
using VL::CpuFrameProfiler;
using VL::CpuFrameStage;
using VL::CpuFrameStageName;
using VL::CpuFrameTimingRing;
using VL::kCpuFrameStageCount;
using VL::kPerformanceCpuOtherStageName;
using VL::PerformancePassSample;
using VL::PerformanceSnapshot;
using VL::RecomputePerformanceSnapshotCpuOther;

std::array<std::optional<double>, kCpuFrameStageCount> FrameSamples()
{
    std::array<std::optional<double>, kCpuFrameStageCount> frame{};
    frame.fill(std::nullopt);
    return frame;
}

TEST(CpuFrameProfiler, PartialWindowAveragesPerStageSampleCounts)
{
    CpuFrameTimingRing ring(4);
    auto first = FrameSamples();
    first[static_cast<size_t>(CpuFrameStage::Input)] = 1.0;
    first[static_cast<size_t>(CpuFrameStage::Scripts)] = 2.0;
    ring.AddFrame(first);

    // 第二帧缺少 scripts 样本：该行只按 1 个样本平均，不影响 input 的部分窗口。
    auto second = FrameSamples();
    second[static_cast<size_t>(CpuFrameStage::Input)] = 3.0;
    ring.AddFrame(second);

    EXPECT_EQ(ring.WindowFrameCount(), 2u);
    ASSERT_TRUE(ring.Average(CpuFrameStage::Input).has_value());
    EXPECT_DOUBLE_EQ(*ring.Average(CpuFrameStage::Input), 2.0);
    ASSERT_TRUE(ring.Average(CpuFrameStage::Scripts).has_value());
    EXPECT_DOUBLE_EQ(*ring.Average(CpuFrameStage::Scripts), 2.0);
    EXPECT_EQ(ring.SampleCount(CpuFrameStage::Input), 2u);
    EXPECT_EQ(ring.SampleCount(CpuFrameStage::Scripts), 1u);
}

TEST(CpuFrameProfiler, UnsampledStageStaysNullInsteadOfZero)
{
    CpuFrameTimingRing ring(4);
    auto frame = FrameSamples();
    frame[static_cast<size_t>(CpuFrameStage::Input)] = 0.5;
    ring.AddFrame(frame);

    // 从未采样的阶段是 n/a；0.0 是真实样本，两者不能混为一谈。
    EXPECT_FALSE(ring.Average(CpuFrameStage::Animation).has_value());
    EXPECT_EQ(ring.SampleCount(CpuFrameStage::Animation), 0u);
    ASSERT_TRUE(ring.Average(CpuFrameStage::Input).has_value());
    EXPECT_DOUBLE_EQ(*ring.Average(CpuFrameStage::Input), 0.5);
}

TEST(CpuFrameProfiler, RingEvictsOldestSamplesAtCapacity)
{
    CpuFrameTimingRing ring(2);
    for (const double value : {1.0, 2.0, 3.0})
    {
        auto frame = FrameSamples();
        frame[static_cast<size_t>(CpuFrameStage::Input)] = value;
        ring.AddFrame(frame);
    }

    EXPECT_EQ(ring.WindowFrameCount(), 2u);
    EXPECT_EQ(ring.SampleCount(CpuFrameStage::Input), 2u);
    ASSERT_TRUE(ring.Average(CpuFrameStage::Input).has_value());
    EXPECT_DOUBLE_EQ(*ring.Average(CpuFrameStage::Input), 2.5);
}

TEST(CpuFrameProfiler, StageOrderAndNamesAreStable)
{
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::Input), "input");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::Scripts), "scripts");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::Animation), "animation");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::HairSimulation), "hairSimulation");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::WorldSnapshot), "worldSnapshot");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::UiViewModel), "uiViewModel");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::RenderRecord), "renderRecord");
    EXPECT_STREQ(CpuFrameStageName(CpuFrameStage::SubmitPresent), "submitPresent");
}

TEST(CpuFrameProfiler, CommitAccumulatesMultipleIntervalsIntoOneStage)
{
    CpuFrameProfiler profiler;
    profiler.BeginFrame();
    // 同一阶段一帧内允许两段（Tick 的 input/scripts 就是不连续的）。
    profiler.AddStageMilliseconds(CpuFrameStage::Scripts, 1.5);
    profiler.AddStageMilliseconds(CpuFrameStage::Scripts, 0.5);
    profiler.AddStageMilliseconds(CpuFrameStage::Animation, 2.0);

    const std::vector<PerformancePassSample>& samples = profiler.CommitFrame();
    ASSERT_EQ(samples.size(), kCpuFrameStageCount);
    EXPECT_EQ(samples[0].name, "input");
    EXPECT_FALSE(samples[0].milliseconds.has_value());
    ASSERT_TRUE(samples[1].milliseconds.has_value());
    EXPECT_DOUBLE_EQ(*samples[1].milliseconds, 2.0);
    ASSERT_TRUE(samples[2].milliseconds.has_value());
    EXPECT_DOUBLE_EQ(*samples[2].milliseconds, 2.0);

    // CommitFrame 之后本帧累加器已清零：第二次只提交一次 Animation。
    profiler.AddStageMilliseconds(CpuFrameStage::Animation, 4.0);
    const std::vector<PerformancePassSample>& second = profiler.CommitFrame();
    EXPECT_EQ(profiler.WindowFrameCount(), 2u);
    ASSERT_TRUE(second[2].milliseconds.has_value());
    EXPECT_DOUBLE_EQ(*second[2].milliseconds, 3.0);
    EXPECT_EQ(second[2].sampleCount, 2u);
}

TEST(CpuFrameProfiler, OtherRemainderIsFrameDeltaMinusMeasuredStages)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"geometry", 2.0, 1}},
        1, true, true, 0.020,
        {{"input", 1.0, 1}, {"scripts", 2.0, 1}, {"animation", 3.0, 1}},
        1);

    ASSERT_EQ(snapshot.cpuStages.size(), 4u);
    EXPECT_EQ(snapshot.cpuStages.back().name, kPerformanceCpuOtherStageName);
    ASSERT_TRUE(snapshot.cpuStages.back().milliseconds.has_value());
    // 20 - (1 + 2 + 3) = 14 ms
    EXPECT_DOUBLE_EQ(*snapshot.cpuStages.back().milliseconds, 14.0);
    EXPECT_EQ(snapshot.cpuStages.back().sampleCount, 0u);
    EXPECT_DOUBLE_EQ(*ComputePerformanceCpuOtherMilliseconds(snapshot), 14.0);
}

TEST(CpuFrameProfiler, OtherRemainderClampsAtZeroWhenStagesExceedFrameDelta)
{
    PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"geometry", 2.0, 1}},
        1, true, true, 0.010,
        {{"input", 8.0, 1}, {"scripts", 9.0, 1}},
        1);

    ASSERT_EQ(snapshot.cpuStages.size(), 3u);
    ASSERT_TRUE(snapshot.cpuStages.back().milliseconds.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.cpuStages.back().milliseconds, 0.0);

    // 再次重算仍是 0，不会出现负 remainder。
    RecomputePerformanceSnapshotCpuOther(snapshot);
    ASSERT_TRUE(snapshot.cpuStages.back().milliseconds.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.cpuStages.back().milliseconds, 0.0);
}

TEST(CpuFrameProfiler, OtherRemainderIsNullWithoutCpuFrameTime)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {}, 0, true, false, std::nullopt,
        {{"input", 1.0, 1}}, 0);
    ASSERT_EQ(snapshot.cpuStages.size(), 2u);
    EXPECT_EQ(snapshot.cpuStages.back().name, kPerformanceCpuOtherStageName);
    EXPECT_FALSE(snapshot.cpuStages.back().milliseconds.has_value());
}

TEST(CpuFrameProfiler, EmptyCpuStageChannelDoesNotInventOtherRow)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"geometry", 2.0, 1}}, 1, true, true, 0.020);
    EXPECT_TRUE(snapshot.cpuStages.empty());
    EXPECT_EQ(snapshot.cpuWindowFrameCount, 0u);
}

TEST(CpuFrameProfiler, OtherRowUpdatesInPlaceAndStaysLast)
{
    PerformanceSnapshot snapshot;
    snapshot.cpuFrameMs = 10.0;
    snapshot.cpuStages = {{"input", 2.0, 3}, {"scripts", 3.0, 3}};
    RecomputePerformanceSnapshotCpuOther(snapshot);
    ASSERT_EQ(snapshot.cpuStages.size(), 3u);
    EXPECT_EQ(snapshot.cpuStages.back().name, kPerformanceCpuOtherStageName);

    snapshot.cpuFrameMs = 20.0;
    RecomputePerformanceSnapshotCpuOther(snapshot);
    ASSERT_EQ(snapshot.cpuStages.size(), 3u);
    EXPECT_DOUBLE_EQ(*snapshot.cpuStages.back().milliseconds, 15.0);
}

} // namespace
