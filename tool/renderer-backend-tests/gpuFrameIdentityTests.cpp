#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <sstream>
#include <nlohmann/json.hpp>
#include "render/backend/gpuFrameIdentity.h"

namespace
{
struct FrameContextFixture
{
    uint32_t currentFrame;
    struct Scene { uint64_t worldGeneration; } renderScene;
};

VL::RenderFrameIdentity ContextIdentity(uint32_t frame, uint64_t world)
{
    const FrameContextFixture context{frame, {world}};
    return VL::RenderFrameIdentity::FromContext(context);
}
}

TEST(GpuFrameIdentity, ActualContextMappingCannotSwapWorldFrameOrEpoch)
{
    const auto identity = ContextIdentity(701, 91003);
    EXPECT_EQ(identity.renderFrame, 701u);
    EXPECT_EQ(identity.worldGeneration, 91003u);
    EXPECT_EQ(identity.submitEpoch, 702u);
}

TEST(GpuFrameIdentity, EpochWideningPrecedesAdditionAndJsonRetainsLargeWorldIntegers)
{
    const auto identity = ContextIdentity(UINT32_MAX, (uint64_t{1} << 60) + 13);
    std::ostringstream raw;
    raw << '{';
    VL::WriteRawFrameIdentity(raw, identity);
    raw << '}';
    const auto document = nlohmann::json::parse(raw.str());
    EXPECT_EQ(document.at("frame").get<uint64_t>(), 4294967295ull);
    EXPECT_EQ(document.at("submitEpoch").get<uint64_t>(), 4294967296ull);
    EXPECT_EQ(document.at("worldGeneration").get<uint64_t>(), (uint64_t{1} << 60) + 13);
}

TEST(GpuFrameIdentity, EndWithoutActualSubmitNeverPublishesReadbackIdentity)
{
    VL::GpuQueryFrameIdentity state;
    state.Begin(ContextIdentity(17, 900));
    state.End(true);
    EXPECT_FALSE(state.Pending());
    state.Abort();
    EXPECT_FALSE(state.Submitted());
    EXPECT_FALSE(state.Pending());
}

TEST(GpuFrameIdentity, SlotReuseDoesNotRelabelPreviousBatchWithNewWorldOrFrame)
{
    VL::GpuQueryFrameIdentity slot;
    slot.Begin(ContextIdentity(17, 900));
    slot.End(true);
    ASSERT_TRUE(slot.Submitted());
    slot.Begin(ContextIdentity(251, 12345));
    ASSERT_TRUE(slot.Pending());
    EXPECT_EQ(slot.Pending()->renderFrame, 17u);
    EXPECT_EQ(slot.Pending()->worldGeneration, 900u);
    EXPECT_EQ(slot.Pending()->submitEpoch, 18u);
    slot.Consumed();
    slot.End(true);
    EXPECT_FALSE(slot.Pending());
    ASSERT_TRUE(slot.Submitted());
    EXPECT_EQ(slot.Pending()->renderFrame, 251u);
    EXPECT_EQ(slot.Pending()->worldGeneration, 12345u);
}

TEST(GpuFrameIdentity, DelayedSlotsAndSkippedFramesNeedNoShiftOrOrdinalGuess)
{
    std::array<VL::GpuQueryFrameIdentity, 2> slots;
    slots[0].Begin(ContextIdentity(100, 3));
    slots[0].End(true);
    ASSERT_TRUE(slots[0].Submitted());
    slots[1].Begin(ContextIdentity(107, 8));
    slots[1].End(true);
    ASSERT_TRUE(slots[1].Submitted());
    ASSERT_TRUE(slots[1].Pending());
    EXPECT_EQ(slots[1].Pending()->submitEpoch, 108u);
    EXPECT_EQ(slots[1].Pending()->worldGeneration, 8u);
    ASSERT_TRUE(slots[0].Pending());
    EXPECT_EQ(slots[0].Pending()->renderFrame, 100u);
    EXPECT_EQ(slots[0].Pending()->worldGeneration, 3u);
}

TEST(GpuFrameIdentity, AbortOfCurrentBatchPreservesAnUnconsumedOlderBatch)
{
    VL::GpuQueryFrameIdentity slot;
    slot.Begin(ContextIdentity(20, 5));
    slot.End(true);
    ASSERT_TRUE(slot.Submitted());
    slot.Begin(ContextIdentity(22, 6));
    slot.End(true);
    slot.Abort();
    EXPECT_FALSE(slot.Submitted());
    ASSERT_TRUE(slot.Pending());
    EXPECT_EQ(slot.Pending()->renderFrame, 20u);
    EXPECT_EQ(slot.Pending()->worldGeneration, 5u);
}

TEST(GpuFrameIdentity, EmptyQueryBatchAndDisabledBeginCannotInventGpuRows)
{
    VL::GpuQueryFrameIdentity slot;
    EXPECT_FALSE(slot.Submitted());
    slot.Begin(ContextIdentity(40, 21));
    slot.End(false);
    EXPECT_FALSE(slot.Submitted());
    EXPECT_FALSE(slot.Pending());
    slot.Begin(ContextIdentity(41, 21));
    slot.Abort();
    EXPECT_FALSE(slot.Submitted());
}

TEST(GpuFrameIdentity, CurrentIdentityIsCopiedAndSubmissionCannotBeReplayed)
{
    VL::GpuQueryFrameIdentity slot;
    auto original = ContextIdentity(55, 81);
    slot.Begin(original);
    original.renderFrame = 1000;
    original.worldGeneration = 2000;
    slot.End(true);
    ASSERT_TRUE(slot.Submitted());
    EXPECT_FALSE(slot.Submitted());
    EXPECT_EQ(slot.Pending()->renderFrame, 55u);
    EXPECT_EQ(slot.Pending()->worldGeneration, 81u);
    slot.Consumed();
    EXPECT_FALSE(slot.Pending());
}

TEST(GpuFrameIdentity, CpuAndGpuSerializationUseTheSameImmutableIdentity)
{
    const auto cpuIdentity = ContextIdentity(315, 8888);
    VL::GpuQueryFrameIdentity slot;
    slot.Begin(cpuIdentity);
    slot.End(true);
    ASSERT_TRUE(slot.Submitted());
    std::ostringstream cpu, gpu;
    cpu << '{';
    VL::WriteRawFrameIdentity(cpu, cpuIdentity);
    cpu << '}';
    gpu << '{';
    VL::WriteRawFrameIdentity(gpu, *slot.Pending());
    gpu << ",\"readbackSequence\":2}";
    const auto cpuDocument = nlohmann::json::parse(cpu.str());
    const auto gpuDocument = nlohmann::json::parse(gpu.str());
    EXPECT_EQ(gpuDocument.at("frame"), 315);
    EXPECT_EQ(gpuDocument.at("renderFrame"), 315);
    EXPECT_EQ(gpuDocument.at("worldGeneration"), 8888);
    EXPECT_EQ(gpuDocument.at("submitEpoch"), 316);
    EXPECT_EQ(cpuDocument.at("renderFrame"), gpuDocument.at("renderFrame"));
    EXPECT_EQ(cpuDocument.at("submitEpoch"), gpuDocument.at("submitEpoch"));
    EXPECT_NE(gpuDocument.at("frame"), gpuDocument.at("readbackSequence"));
}

TEST(GpuFrameIdentity, ReadbackRequiresTheActualCompletedSubmissionEpoch)
{
    VL::GpuQueryFrameIdentity slot;
    slot.Begin(ContextIdentity(801, 39));
    slot.End(true);
    ASSERT_TRUE(slot.Submitted());
    EXPECT_EQ(slot.Completed(801), nullptr);
    const auto* actual = slot.Completed(802);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual->renderFrame, 801u);
    EXPECT_EQ(actual->submitEpoch, 802u);
    EXPECT_EQ(actual->worldGeneration, 39u);
    slot.Consumed();
    EXPECT_EQ(slot.Completed(900), nullptr);
}
