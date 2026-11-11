#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleFrameStats.h"
#include "render/particles/particleGpuTiming.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleFrameStats, ZeroValuesFormatAndExposeEveryJsonField)
{
    const ParticleFrameStats stats;
    EXPECT_EQ(
        stats.FormatLine(),
        "particles frame=0 elapsed=0.00s emitters=0 visible=0 "
        "culled=0 sorted=0 drawCalls=0 vertices=0 particles=0 "
        "sortDispatches=0 sortCapacity=0 gpuTimingSupported=0 "
        "gpuMs=0.000 gpuAvgMs=0.000 gpuMaxMs=0.000 gpuSamples=0");

    const nlohmann::ordered_json json = stats.ToJson();
    ASSERT_EQ(json.size(), 17u);
    EXPECT_EQ(json.at("totalEmitters").get<uint32_t>(), 0u);
    EXPECT_EQ(json.at("visibleEmitters").get<uint32_t>(), 0u);
    EXPECT_EQ(json.at("culledEmitters").get<uint32_t>(), 0u);
    EXPECT_EQ(json.at("sortedEmitters").get<uint32_t>(), 0u);
    EXPECT_EQ(json.at("estimatedDrawCalls").get<uint32_t>(), 0u);
    EXPECT_EQ(json.at("estimatedDrawnVertices").get<uint64_t>(), 0u);
    EXPECT_EQ(json.at("totalParticles").get<uint64_t>(), 0u);
    EXPECT_EQ(json.at("sortDispatchCount").get<uint64_t>(), 0u);
    EXPECT_EQ(json.at("sortCapacityTotal").get<uint64_t>(), 0u);
    EXPECT_EQ(json.at("frameIndex").get<uint64_t>(), 0u);
    EXPECT_DOUBLE_EQ(json.at("elapsedSeconds").get<double>(), 0.0);
    EXPECT_FALSE(json.at("gpuTimingSupported").get<bool>());
    EXPECT_DOUBLE_EQ(json.at("gpuMilliseconds").get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(
        json.at("gpuAverageMilliseconds").get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(json.at("gpuMaxMilliseconds").get<double>(), 0.0);
    EXPECT_EQ(json.at("gpuSampleCount").get<uint64_t>(), 0u);
    ASSERT_TRUE(json.at("emitterGpu").is_array());
    EXPECT_TRUE(json.at("emitterGpu").empty());
}

TEST(ParticleFrameStats, FullValuesSurviveJsonRoundTrip)
{
    ParticleFrameStats stats;
    stats.totalEmitters = 7;
    stats.visibleEmitters = 6;
    stats.culledEmitters = 1;
    stats.sortedEmitters = 3;
    stats.estimatedDrawCalls = 6;
    stats.estimatedDrawnVertices = 89856;
    stats.totalParticles = 14976;
    stats.sortDispatchCount = 108;
    stats.sortCapacityTotal = 6144;
    stats.frameIndex = 123;
    stats.elapsedSeconds = 2.05;
    stats.gpuTimingSupported = true;
    stats.gpuMilliseconds = 0.42;
    stats.gpuAverageMilliseconds = 0.37;
    stats.gpuMaxMilliseconds = 0.91;
    stats.gpuSampleCount = 64;

    const nlohmann::json roundTripped = nlohmann::json::parse(
        stats.ToJson().dump());
    EXPECT_EQ(roundTripped.at("totalEmitters").get<uint32_t>(), 7u);
    EXPECT_EQ(roundTripped.at("visibleEmitters").get<uint32_t>(), 6u);
    EXPECT_EQ(roundTripped.at("culledEmitters").get<uint32_t>(), 1u);
    EXPECT_EQ(roundTripped.at("sortedEmitters").get<uint32_t>(), 3u);
    EXPECT_EQ(roundTripped.at("estimatedDrawCalls").get<uint32_t>(), 6u);
    EXPECT_EQ(
        roundTripped.at("estimatedDrawnVertices").get<uint64_t>(),
        89856u);
    EXPECT_EQ(roundTripped.at("totalParticles").get<uint64_t>(), 14976u);
    EXPECT_EQ(
        roundTripped.at("sortDispatchCount").get<uint64_t>(), 108u);
    EXPECT_EQ(
        roundTripped.at("sortCapacityTotal").get<uint64_t>(), 6144u);
    EXPECT_EQ(roundTripped.at("frameIndex").get<uint64_t>(), 123u);
    EXPECT_DOUBLE_EQ(
        roundTripped.at("elapsedSeconds").get<double>(), 2.05);
    EXPECT_TRUE(roundTripped.at("gpuTimingSupported").get<bool>());
    EXPECT_DOUBLE_EQ(
        roundTripped.at("gpuMilliseconds").get<double>(), 0.42);
    EXPECT_DOUBLE_EQ(
        roundTripped.at("gpuAverageMilliseconds").get<double>(), 0.37);
    EXPECT_DOUBLE_EQ(
        roundTripped.at("gpuMaxMilliseconds").get<double>(), 0.91);
    EXPECT_EQ(roundTripped.at("gpuSampleCount").get<uint64_t>(), 64u);
    EXPECT_TRUE(roundTripped.at("emitterGpu").is_array());
}

TEST(ParticleFrameStats, EmitterGpuRowsRoundTrip)
{
    ParticleFrameStats stats;
    stats.emitterGpu.push_back(ParticleFrameStats::EmitterGpu{
        "__global__", true, 0.25, 0.20, 0.40, 8});
    stats.emitterGpu.push_back(ParticleFrameStats::EmitterGpu{
        "scene|asset|sparks", false, 0.0, 0.0, 0.0, 0});

    const nlohmann::json json =
        nlohmann::json::parse(stats.ToJson().dump());
    ASSERT_EQ(json.at("emitterGpu").size(), 2u);
    EXPECT_EQ(
        json.at("emitterGpu").at(0).at("key").get<std::string>(),
        "__global__");
    EXPECT_TRUE(
        json.at("emitterGpu").at(0).at("supported").get<bool>());
    EXPECT_DOUBLE_EQ(
        json.at("emitterGpu").at(0).at("milliseconds").get<double>(),
        0.25);
    EXPECT_EQ(
        json.at("emitterGpu").at(0).at("sampleCount").get<uint64_t>(),
        8u);
    EXPECT_FALSE(
        json.at("emitterGpu").at(1).at("supported").get<bool>());
}

TEST(ParticleFrameStats, BitonicDispatchCountMatchesPowersOfTwo)
{
    EXPECT_EQ(ParticleSortDispatchCount(1u), 0u);
    EXPECT_EQ(ParticleSortDispatchCount(2u), 1u);
    EXPECT_EQ(ParticleSortDispatchCount(4u), 3u);
    EXPECT_EQ(ParticleSortDispatchCount(256u), 36u);
    EXPECT_EQ(ParticleSortDispatchCount(2048u), 66u);
    EXPECT_EQ(ParticleSortDispatchCount(4096u), 78u);
}

TEST(ParticleFrameStats, FormatLineKeepsGreppableKeywords)
{
    ParticleFrameStats stats;
    stats.totalEmitters = 1;

    const std::string line = stats.FormatLine();
    EXPECT_NE(line.find("emitters="), std::string::npos);
    EXPECT_NE(line.find("visible="), std::string::npos);
    EXPECT_NE(line.find("sorted="), std::string::npos);
    EXPECT_NE(line.find("drawCalls="), std::string::npos);
    EXPECT_NE(line.find("vertices="), std::string::npos);
    EXPECT_NE(line.find("sortDispatches="), std::string::npos);
    EXPECT_NE(line.find("gpuTimingSupported="), std::string::npos);
    EXPECT_NE(line.find("gpuMs="), std::string::npos);
    EXPECT_NE(line.find("gpuAvgMs="), std::string::npos);
    EXPECT_NE(line.find("gpuMaxMs="), std::string::npos);
    EXPECT_NE(line.find("gpuSamples="), std::string::npos);
}

TEST(ParticleFrameStats, GpuFieldsFormatMillisecondsWithThreeDecimals)
{
    ParticleFrameStats stats;
    stats.gpuTimingSupported = true;
    stats.gpuMilliseconds = 1.23456;
    stats.gpuAverageMilliseconds = 0.98765;
    stats.gpuMaxMilliseconds = 2.5;
    stats.gpuSampleCount = 12;

    const std::string line = stats.FormatLine();
    EXPECT_NE(
        line.find("gpuTimingSupported=1 gpuMs=1.235 gpuAvgMs=0.988 "
                  "gpuMaxMs=2.500 gpuSamples=12"),
        std::string::npos) << line;
}

TEST(ParticleGpuTiming, TimestampMaskHandlesPartialAndFullWidth)
{
    EXPECT_EQ(ParticleGpuTimestampMask(0u), 0u);
    EXPECT_EQ(ParticleGpuTimestampMask(1u), 1u);
    EXPECT_EQ(ParticleGpuTimestampMask(8u), 255u);
    EXPECT_EQ(ParticleGpuTimestampMask(32u), 0xFFFFFFFFull);
    EXPECT_EQ(
        ParticleGpuTimestampMask(64u),
        std::numeric_limits<uint64_t>::max());
}

TEST(ParticleGpuTiming, TimestampDeltaConvertsTicksWithWraparound)
{
    // 8 位有效位、1ns/ticks：0xFE -> 0x05 回绕后是 7 ticks。
    EXPECT_NEAR(
        ParticleGpuTimestampDeltaMilliseconds(
            0xFEull, 0x05ull, 8u, 1.0f),
        0.000007,
        1e-12);
    // 64 位有效位、2ns/tick：差值 500 ticks = 1000ns = 0.001ms。
    EXPECT_NEAR(
        ParticleGpuTimestampDeltaMilliseconds(
            10ull, 510ull, 64u, 2.0f),
        0.001,
        1e-12);
    // 不支持计时时始终返回 0。
    EXPECT_DOUBLE_EQ(
        ParticleGpuTimestampDeltaMilliseconds(0ull, 100ull, 0u, 1.0f),
        0.0);
    EXPECT_DOUBLE_EQ(
        ParticleGpuTimestampDeltaMilliseconds(0ull, 100ull, 32u, 0.0f),
        0.0);
}

TEST(ParticleGpuTiming, AccumulatorTracksLastAverageMaxAndSamples)
{
    ParticleGpuTimingAccumulator accumulator;
    accumulator.Reset(true);
    accumulator.Record(1.0);
    accumulator.Record(3.0);
    accumulator.Record(2.0);

    const ParticleGpuTimingSnapshot& snapshot = accumulator.GetSnapshot();
    EXPECT_TRUE(snapshot.supported);
    EXPECT_EQ(snapshot.sampleCount, 3u);
    EXPECT_DOUBLE_EQ(snapshot.lastMilliseconds, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.averageMilliseconds, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.maxMilliseconds, 3.0);
}

TEST(ParticleGpuTiming, DisabledAccumulatorIgnoresSamplesAndZeroesSnapshot)
{
    ParticleGpuTimingAccumulator accumulator;
    accumulator.Reset(false);
    accumulator.Record(5.0);

    ParticleGpuTimingSnapshot snapshot = accumulator.GetSnapshot();
    EXPECT_FALSE(snapshot.supported);
    EXPECT_EQ(snapshot.sampleCount, 0u);
    EXPECT_DOUBLE_EQ(snapshot.lastMilliseconds, 0.0);

    accumulator.Reset(true);
    accumulator.Record(4.0);
    accumulator.Record(-1.0);
    accumulator.Record(std::numeric_limits<double>::quiet_NaN());
    snapshot = accumulator.GetSnapshot();
    EXPECT_TRUE(snapshot.supported);
    EXPECT_EQ(snapshot.sampleCount, 1u);
    EXPECT_DOUBLE_EQ(snapshot.lastMilliseconds, 4.0);

    accumulator.Disable();
    snapshot = accumulator.GetSnapshot();
    EXPECT_FALSE(snapshot.supported);
    EXPECT_EQ(snapshot.sampleCount, 0u);
    EXPECT_DOUBLE_EQ(snapshot.averageMilliseconds, 0.0);
}
