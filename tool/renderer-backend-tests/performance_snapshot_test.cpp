#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/performanceSnapshot.h"
#include "render/backend/gpuPassTimings.h"

namespace
{
using VL::BuildPerformanceSnapshot;
using VL::FormatPerformanceJson;
using VL::FormatPerformanceNumber;
using VL::PerformanceCpuStagePercent;
using VL::PerformancePassPercent;
using VL::PerformancePassSample;
using VL::PerformanceSnapshot;

TEST(PerformanceSnapshot, JsonUsesStableKeyOrderRoundingAndNullsForMissingSamples)
{
    PerformanceSnapshot snapshot;
    snapshot.gpuTimingSupported = true;
    snapshot.collecting = true;
    snapshot.fps = 59.94;
    snapshot.cpuFrameMs = 16.6834;
    snapshot.gpuTotalMs = 12.3456;
    snapshot.passes = {
        {"hairSimulation", 0.18, 120},
        {"shadowCascade0", std::nullopt, 0},
        {"geometry", 12.1656, 120}};
    snapshot.cpuStages = {
        {"input", 0.5, 120},
        {"other", 16.0, 0}};

    EXPECT_TRUE(snapshot.IsValid());
    EXPECT_EQ(
        FormatPerformanceJson(snapshot),
        "{\"fps\":59.940,\"cpuMs\":16.683,\"gpuTotalMs\":12.346,\"valid\":true,"
        "\"passes\":[{\"name\":\"hairSimulation\",\"ms\":0.180},"
        "{\"name\":\"shadowCascade0\",\"ms\":null},"
        "{\"name\":\"geometry\",\"ms\":12.166}],"
        "\"cpuStages\":[{\"name\":\"input\",\"ms\":0.500},"
        "{\"name\":\"other\",\"ms\":16.000}]}");
}

TEST(PerformanceSnapshot, JsonNeverEmitsNaNOrInfinity)
{
    PerformanceSnapshot snapshot;
    snapshot.gpuTimingSupported = false;
    snapshot.collecting = false;
    snapshot.fps = std::numeric_limits<double>::quiet_NaN();
    snapshot.cpuFrameMs = std::numeric_limits<double>::infinity();
    snapshot.gpuTotalMs = -std::numeric_limits<double>::infinity();
    snapshot.passes = {{"broken", std::numeric_limits<double>::quiet_NaN(), 3}};

    const std::string json = FormatPerformanceJson(snapshot);
    EXPECT_EQ(
        json,
        "{\"fps\":null,\"cpuMs\":null,\"gpuTotalMs\":null,\"valid\":false,"
        "\"passes\":[{\"name\":\"broken\",\"ms\":null}],\"cpuStages\":[]}");
    EXPECT_EQ(json.find("nan"), std::string::npos);
    EXPECT_EQ(json.find("inf"), std::string::npos);
}

TEST(PerformanceSnapshot, JsonQuotesAreEscapedEvenThoughPassNamesAreInternal)
{
    PerformanceSnapshot snapshot;
    snapshot.passes = {{"pass\"quoted\"", 1.0, 1}};
    EXPECT_EQ(
        FormatPerformanceJson(snapshot),
        "{\"fps\":null,\"cpuMs\":null,\"gpuTotalMs\":null,\"valid\":false,"
        "\"passes\":[{\"name\":\"pass\\\"quoted\\\"\",\"ms\":1.000}],"
        "\"cpuStages\":[]}");
}

TEST(PerformanceSnapshot, BuildMergesCpuDeltaWithPartialGpuWindowInStableOrder)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"hairSimulation", 0.2, 3}, {"shadowCascade0", std::nullopt, 0},
            {"deferredLighting", 8.0, 3}},
        3, true, true, 0.025);

    ASSERT_TRUE(snapshot.fps.has_value());
    ASSERT_TRUE(snapshot.cpuFrameMs.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.fps, 40.0);
    EXPECT_DOUBLE_EQ(*snapshot.cpuFrameMs, 25.0);
    EXPECT_EQ(snapshot.windowFrameCount, 3u);
    ASSERT_EQ(snapshot.passes.size(), 3u);
    EXPECT_EQ(snapshot.passes[0].name, "hairSimulation");
    EXPECT_EQ(snapshot.passes[1].name, "shadowCascade0");
    EXPECT_EQ(snapshot.passes[2].name, "deferredLighting");
    EXPECT_FALSE(snapshot.passes[1].milliseconds.has_value());
    ASSERT_TRUE(snapshot.gpuTotalMs.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.gpuTotalMs, 8.2);
    EXPECT_TRUE(snapshot.IsValid());
}

TEST(PerformanceSnapshot, BuildMergesCpuStagesAndDerivesOtherLast)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"geometry", 2.0, 1}}, 1, true, true, 0.020,
        {{"input", 0.5, 2}, {"scripts", 1.0, 2}, {"submitPresent", 2.5, 2}},
        2);

    ASSERT_EQ(snapshot.cpuStages.size(), 4u);
    EXPECT_EQ(snapshot.cpuStages[0].name, "input");
    EXPECT_EQ(snapshot.cpuStages[1].name, "scripts");
    EXPECT_EQ(snapshot.cpuStages[2].name, "submitPresent");
    EXPECT_EQ(snapshot.cpuStages[3].name, "other");
    ASSERT_TRUE(snapshot.cpuStages[3].milliseconds.has_value());
    // 20 - (0.5 + 1.0 + 2.5) = 16 ms；派生行没有独立样本数。
    EXPECT_DOUBLE_EQ(*snapshot.cpuStages[3].milliseconds, 16.0);
    EXPECT_EQ(snapshot.cpuStages[3].sampleCount, 0u);
    EXPECT_EQ(snapshot.cpuWindowFrameCount, 2u);

    const auto percent = PerformanceCpuStagePercent(snapshot, snapshot.cpuStages[0]);
    ASSERT_TRUE(percent.has_value());
    EXPECT_DOUBLE_EQ(*percent, 2.5);
    EXPECT_EQ(
        FormatPerformanceJson(snapshot),
        "{\"fps\":50.000,\"cpuMs\":20.000,\"gpuTotalMs\":2.000,\"valid\":true,"
        "\"passes\":[{\"name\":\"geometry\",\"ms\":2.000}],"
        "\"cpuStages\":[{\"name\":\"input\",\"ms\":0.500},"
        "{\"name\":\"scripts\",\"ms\":1.000},"
        "{\"name\":\"submitPresent\",\"ms\":2.500},"
        "{\"name\":\"other\",\"ms\":16.000}]}");
}

TEST(PerformanceSnapshot, MissingOrNegativeSamplesOnlyRemoveTheirOwnRowFromTheTotal)
{
    PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"a", 1.5, 2}, {"b", -0.5, 1}, {"c", std::nullopt, 0}, {"d", 2.0, 2}},
        4, true, true, 0.02);
    ASSERT_TRUE(snapshot.gpuTotalMs.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.gpuTotalMs, 3.5);
    EXPECT_FALSE(snapshot.passes[1].milliseconds.has_value());
}

TEST(PerformanceSnapshot, EmptyWindowIsInvalidWithoutInventingZeros)
{
    PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"shadowCascade0", std::nullopt, 0}}, 0, true, true, 0.02);
    EXPECT_FALSE(snapshot.gpuTotalMs.has_value());
    EXPECT_FALSE(snapshot.IsValid());
    EXPECT_EQ(
        FormatPerformanceJson(snapshot),
        "{\"fps\":50.000,\"cpuMs\":20.000,\"gpuTotalMs\":null,\"valid\":false,"
        "\"passes\":[{\"name\":\"shadowCascade0\",\"ms\":null}],"
        "\"cpuStages\":[]}");
}

TEST(PerformanceSnapshot, UnsupportedOrNotYetReadBackGpuChannelIsInvalid)
{
    // 有可读回的数字但设备不支持 timestamp：GPU 通道仍必须判无效。
    PerformanceSnapshot unsupported = BuildPerformanceSnapshot(
        {{"geometry", 1.0, 1}}, 1, false, true, 0.02);
    EXPECT_FALSE(unsupported.IsValid());
    EXPECT_TRUE(unsupported.gpuTotalMs.has_value());

    // 支持设备但采样尚未读回：valid=false，pass 数字可以是缺省。
    PerformanceSnapshot notSampled = BuildPerformanceSnapshot(
        {{"geometry", std::nullopt, 0}}, 0, true, false, 0.02);
    EXPECT_FALSE(notSampled.IsValid());
}

TEST(PerformanceSnapshot, InvalidCpuDeltaLeavesFpsAndCpuMsNull)
{
    PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {}, 0, true, false, std::numeric_limits<double>::quiet_NaN());
    EXPECT_FALSE(snapshot.fps.has_value());
    EXPECT_FALSE(snapshot.cpuFrameMs.has_value());

    snapshot = BuildPerformanceSnapshot({}, 0, true, false, 0.0);
    EXPECT_FALSE(snapshot.fps.has_value());
    EXPECT_FALSE(snapshot.cpuFrameMs.has_value());
}

TEST(PerformanceSnapshot, CopyIsIndependentAndKeepsGraphOrder)
{
    const PerformanceSnapshot original = BuildPerformanceSnapshot(
        {{"c", 0.1, 1}, {"a", 5.0, 1}, {"b", 1.0, 1}}, 1, true, true, 0.02);
    PerformanceSnapshot copy = original;
    ASSERT_EQ(copy.passes.size(), 3u);
    EXPECT_EQ(copy.passes[0].name, "c");
    EXPECT_EQ(copy.passes[1].name, "a");
    EXPECT_EQ(copy.passes[2].name, "b");

    copy.passes[0].milliseconds = 9.0;
    EXPECT_DOUBLE_EQ(*original.passes[0].milliseconds, 0.1);
}

TEST(PerformanceSnapshot, PercentagesComeFromTheValidTotal)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"a", 1.0, 1}, {"b", 3.0, 1}}, 1, true, true, 0.02);
    const auto first = PerformancePassPercent(snapshot, snapshot.passes[0]);
    const auto second = PerformancePassPercent(snapshot, snapshot.passes[1]);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_DOUBLE_EQ(*first, 25.0);
    EXPECT_DOUBLE_EQ(*second, 75.0);

    PerformanceSnapshot empty;
    EXPECT_FALSE(PerformancePassPercent(empty, PerformancePassSample{"a", 1.0, 1}).has_value());
}

TEST(PerformanceSnapshot, NumberFormattingIsFixedAndNormalizesNegativeZero)
{
    EXPECT_EQ(FormatPerformanceNumber(1.23456), "1.235");
    EXPECT_EQ(FormatPerformanceNumber(1.0), "1.000");
    EXPECT_EQ(FormatPerformanceNumber(-0.0), "0.000");
    EXPECT_EQ(FormatPerformanceNumber(59.94, 1), "59.9");
}

TEST(PerformanceSnapshot, TableConversionKeepsPassOrderAndMissingSamples)
{
    const PerformanceSnapshot snapshot = BuildPerformanceSnapshot(
        {{"first", 1.0, 4}, {"second", std::nullopt, 0}}, 4, true, true, 0.02);
    const VL::GpuPassTimingTable table = VL::ToGpuPassTimingTable(snapshot);
    EXPECT_EQ(table.frameCount, 4u);
    ASSERT_EQ(table.rows.size(), 2u);
    EXPECT_EQ(table.rows[0].name, "first");
    EXPECT_DOUBLE_EQ(*table.rows[0].averageMilliseconds, 1.0);
    EXPECT_EQ(table.rows[1].name, "second");
    EXPECT_FALSE(table.rows[1].averageMilliseconds.has_value());
}

} // namespace
