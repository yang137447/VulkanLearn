#include <gtest/gtest.h>
#include "terrain/terrainCompositedQualityClock.h"
#include <limits>
#include <thread>

namespace VL
{
TEST(TerrainCompositedQualityClock, InitialIncludesPreparationBeforeFirstPose)
{
    TerrainCompositedQualityClock clock;
    clock.Start(true, 1000, 86748, 10000);
    clock.SetPose(1, 80000);
    EXPECT_DOUBLE_EQ(clock.DeadlineMs(), 87748);
    EXPECT_FALSE(clock.Failed(87748));
    EXPECT_TRUE(clock.Failed(87749));
}

TEST(TerrainCompositedQualityClock, InitialMissingReadbackStillExpires)
{
    TerrainCompositedQualityClock clock;
    clock.Start(true, 1000, 86748, 10000);
    EXPECT_TRUE(clock.Failed(87749));
    clock.SetPose(1, 90000);
    EXPECT_FALSE(clock.Observe(90001, true));
}

TEST(TerrainCompositedQualityClock, InitialResourceChurnCannotRestartDeadline)
{
    TerrainCompositedQualityClock clock;
    clock.Start(true, 0, 86748, 10000);
    clock.SetPose(1, 60000);
    clock.SetPose(2, 75000);
    clock.SetPose(3, 85000);
    EXPECT_DOUBLE_EQ(clock.DeadlineMs(), 86748);
    EXPECT_FALSE(clock.Observe(86749, true));
}

TEST(TerrainCompositedQualityClock, LaterPoseUsesArrivalInsteadOfFirstCapture)
{
    TerrainCompositedQualityClock clock;
    clock.Start(true, 0, 86748, 10000);
    clock.SetPose(1, 1000);
    ASSERT_TRUE(clock.Observe(2000, true));
    clock.SetPose(2, 100000);
    EXPECT_DOUBLE_EQ(clock.DeadlineMs(), 110000);
    clock.SetPose(2, 109000);
    EXPECT_DOUBLE_EQ(clock.DeadlineMs(), 110000);
    EXPECT_FALSE(clock.Observe(110001, true));
}

TEST(TerrainCompositedQualityClock, ValidatedPoseDoesNotExpireWhileUnchanged)
{
    TerrainCompositedQualityClock clock;
    clock.Start(true, 0, 86748, 10000);
    clock.SetPose(1, 1000);
    ASSERT_TRUE(clock.Observe(2000, true));
    EXPECT_FALSE(clock.Failed(1000000));
    EXPECT_TRUE(clock.Observe(1000001, true));
    EXPECT_FALSE(clock.Failed(1000002));
}

TEST(TerrainCompositedQualityClock, LaterWorldStartsAtItsActualPoseArrival)
{
    TerrainCompositedQualityClock clock;
    clock.Start(false, 0, 86748, 10000);
    EXPECT_FALSE(clock.Failed(1000000));
    clock.SetPose(1, 1000010);
    EXPECT_DOUBLE_EQ(clock.DeadlineMs(), 1010010);
    EXPECT_TRUE(clock.Observe(1010009, true));
}

TEST(TerrainCompositedQualityClock, ExpiredPoseCannotHideFailureByChangingPose)
{
    TerrainCompositedQualityClock clock;
    clock.Start(false, 0, 86748, 10000);
    clock.SetPose(1, 100);
    clock.SetPose(2, 10101);
    EXPECT_TRUE(clock.Failed(10102));
    EXPECT_FALSE(clock.Observe(10103, true));
}

TEST(TerrainCompositedQualityClock, InvalidClockInputAndDuplicateStartRejected)
{
    TerrainCompositedQualityClock clock;
    EXPECT_THROW(clock.Start(true, 0, 0, 10000), std::invalid_argument);
    EXPECT_THROW(clock.SetPose(1, 1), std::logic_error);
    clock.Start(true, 0, 86748, 10000);
    EXPECT_THROW(clock.Start(true, 0, 86748, 10000), std::logic_error);
    EXPECT_THROW(clock.SetPose(0, 1), std::invalid_argument);
    EXPECT_THROW(clock.SetPose(1, std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(TerrainCompositedQualityClock, ConcurrentDeadlineReadsAndValidObservations)
{
    TerrainCompositedQualityClock clock;
    clock.Start(false, 0, 86748, 10000);
    clock.SetPose(1, 100);
    std::thread reader([&] {
        for (uint32_t count = 0; count < 10000; ++count)
            EXPECT_FALSE(clock.Failed(1000));
    });
    for (uint32_t count = 0; count < 10000; ++count)
        EXPECT_TRUE(clock.Observe(1001, true));
    reader.join();
}
}
