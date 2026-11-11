#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "render/backend/gpuPassTimings.h"

namespace
{

std::vector<std::optional<double>> Frame(std::initializer_list<std::optional<double>> values)
{
    return std::vector<std::optional<double>>(values);
}

} // namespace

TEST(GpuPassTimings, TicksConvertWithTimestampPeriod)
{
    // 1,000,000 ticks × 1 ns / 1e6 = 1 ms.
    const auto milliseconds = VL::ComputeGpuPassDurationMilliseconds(1000, 1001000, 1.0);
    ASSERT_TRUE(milliseconds.has_value());
    EXPECT_DOUBLE_EQ(*milliseconds, 1.0);

    // 非整数 period 也按同一线性换算。
    const auto halfTick = VL::ComputeGpuPassDurationMilliseconds(0, 4000, 0.5);
    ASSERT_TRUE(halfTick.has_value());
    EXPECT_DOUBLE_EQ(*halfTick, 0.002);
}

TEST(GpuPassTimings, WrappedOrReversedQueryIsNotANegativeDuration)
{
    EXPECT_FALSE(VL::ComputeGpuPassDurationMilliseconds(200, 100, 1.0).has_value());
    EXPECT_FALSE(VL::ComputeGpuPassDurationMilliseconds(100, 100, 0.0).has_value());
    EXPECT_FALSE(VL::ComputeGpuPassDurationMilliseconds(100, 100, -1.0).has_value());
}

TEST(GpuPassTimings, RingAveragesPartialWindow)
{
    VL::GpuPassTimingRing ring(4);
    ring.Configure({"a", "b"});
    ring.AddFrame(Frame({1.0, std::nullopt}));
    ring.AddFrame(Frame({3.0, 2.0}));

    const VL::GpuPassTimingTable table = ring.Snapshot();
    EXPECT_EQ(table.frameCount, 2u);
    ASSERT_EQ(table.rows.size(), 2u);
    EXPECT_EQ(table.rows[0].name, "a");
    ASSERT_TRUE(table.rows[0].averageMilliseconds.has_value());
    EXPECT_DOUBLE_EQ(*table.rows[0].averageMilliseconds, 2.0);
    EXPECT_EQ(table.rows[0].sampleCount, 2u);
    EXPECT_EQ(table.rows[1].name, "b");
    ASSERT_TRUE(table.rows[1].averageMilliseconds.has_value());
    EXPECT_DOUBLE_EQ(*table.rows[1].averageMilliseconds, 2.0);
    EXPECT_EQ(table.rows[1].sampleCount, 1u);
}

TEST(GpuPassTimings, RingKeepsOnlyWindowFrames)
{
    VL::GpuPassTimingRing ring(2);
    ring.Configure({"pass"});
    ring.AddFrame(Frame({1.0}));
    ring.AddFrame(Frame({3.0}));
    EXPECT_EQ(ring.WindowFrameCount(), 2u);

    ring.AddFrame(Frame({5.0}));
    const VL::GpuPassTimingTable table = ring.Snapshot();
    EXPECT_EQ(table.frameCount, 2u);
    ASSERT_EQ(table.rows.size(), 1u);
    ASSERT_TRUE(table.rows[0].averageMilliseconds.has_value());
    EXPECT_DOUBLE_EQ(*table.rows[0].averageMilliseconds, 4.0);
}

TEST(GpuPassTimings, NameOrderIsStableAndReconfigureResetsWindow)
{
    VL::GpuPassTimingRing ring(4);
    ring.Configure({"zulu", "alpha", "mike"});
    ring.AddFrame(Frame({1.0, 2.0, 3.0}));

    // 同名 Configure 不清空已有窗口。
    ring.Configure({"zulu", "alpha", "mike"});
    VL::GpuPassTimingTable table = ring.Snapshot();
    EXPECT_EQ(table.frameCount, 1u);
    ASSERT_EQ(table.rows.size(), 3u);
    EXPECT_EQ(table.rows[0].name, "zulu");
    EXPECT_EQ(table.rows[1].name, "alpha");
    EXPECT_EQ(table.rows[2].name, "mike");

    // pass 集合变化意味着旧样本不再属于任何一项，窗口必须从零开始。
    ring.Configure({"alpha", "zulu"});
    table = ring.Snapshot();
    EXPECT_EQ(table.frameCount, 0u);
    ASSERT_EQ(table.rows.size(), 2u);
    EXPECT_EQ(table.rows[0].name, "alpha");
    EXPECT_EQ(table.rows[1].name, "zulu");
}

TEST(GpuPassTimings, TableFormatUsesFixedOrderAndMilliseconds)
{
    VL::GpuPassTimingTable table;
    table.frameCount = 120;
    table.rows = {
        {"shadowCascade0", 0.42, 120},
        {"geometry", 1.5, 120},
        {"deferredLighting", 8.0, 120},
        {"hairSimulation", 0.18, 120}};

    EXPECT_EQ(
        VL::FormatGpuPassTimingTable(table),
        "[GPU] frame avg over 120: shadowCascade0 0.42 ms | geometry 1.50 ms | "
        "deferredLighting 8.00 ms | hairSimulation 0.18 ms | total 10.10 ms");
}

TEST(GpuPassTimings, TableFormatReportsUnavailableSamplesAsNa)
{
    VL::GpuPassTimingTable table;
    table.frameCount = 3;
    table.rows = {
        {"reversed", std::nullopt, 0},
        {"valid", 2.5, 2}};

    EXPECT_EQ(
        VL::FormatGpuPassTimingTable(table),
        "[GPU] frame avg over 3: reversed n/a | valid 2.50 ms | total 2.50 ms");

    VL::GpuPassTimingTable empty;
    EXPECT_EQ(VL::FormatGpuPassTimingTable(empty), "[GPU] frame avg over 0: total n/a");
}
