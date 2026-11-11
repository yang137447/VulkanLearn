#include <gtest/gtest.h>

#include "world/worldSnapshot.h"

TEST(WorldSnapshotQueue, ReplacedPendingSnapshotMergesHairDelta)
{
    VL::WorldSnapshotQueue queue;

    VL::WorldSnapshot first;
    first.frameIndex = 1;
    first.hairSimulationDeltaSeconds = 0.25f;
    queue.Publish(std::move(first));

    VL::WorldSnapshot second;
    second.frameIndex = 2;
    second.hairSimulationDeltaSeconds = 0.5f;
    queue.Publish(std::move(second));

    const auto latest = queue.ConsumeLatest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ((*latest)->frameIndex, 2u);
    EXPECT_FLOAT_EQ((*latest)->hairSimulationDeltaSeconds, 0.75f);
}

TEST(WorldSnapshotQueue, ConsumedSnapshotDoesNotMergeIntoNextFrame)
{
    VL::WorldSnapshotQueue queue;

    VL::WorldSnapshot first;
    first.hairSimulationDeltaSeconds = 0.25f;
    queue.Publish(std::move(first));
    ASSERT_TRUE(queue.ConsumeLatest().has_value());

    VL::WorldSnapshot second;
    second.hairSimulationDeltaSeconds = 0.5f;
    queue.Publish(std::move(second));

    const auto latest = queue.ConsumeLatest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_FLOAT_EQ((*latest)->hairSimulationDeltaSeconds, 0.5f);
}

