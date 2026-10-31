#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "particles/particleBurst.h"

using namespace VL;

TEST(ParticleBurst, ZeroIntervalNeverRetriggers)
{
    EXPECT_FALSE(ParticleBurstCycleCrossed(0.0f, 100.0f, 0.0f));
    EXPECT_FALSE(ParticleBurstCycleCrossed(0.0f, 100.0f, -1.0f));
}

TEST(ParticleBurst, CrossingIntoNextCycleRetriggers)
{
    // 2.5 秒周期：2.4 -> 2.6 跨过 cycle 1。
    EXPECT_FALSE(ParticleBurstCycleCrossed(2.0f, 2.4f, 2.5f));
    EXPECT_TRUE(ParticleBurstCycleCrossed(2.4f, 2.6f, 2.5f));
    // 恰好落在周期边界也算跨越。
    EXPECT_TRUE(ParticleBurstCycleCrossed(0.0f, 2.5f, 2.5f));
    // 大 dt 跨过多个周期只触发一次整池重生。
    EXPECT_TRUE(ParticleBurstCycleCrossed(0.1f, 12.1f, 2.5f));
}

TEST(ParticleBurst, StalledOrBackwardTimeDoesNotRetrigger)
{
    EXPECT_FALSE(ParticleBurstCycleCrossed(3.0f, 3.0f, 1.0f));
    EXPECT_FALSE(ParticleBurstCycleCrossed(4.0f, 3.9f, 1.0f));
}

TEST(ParticleBurst, NonFiniteInputsAreIgnored)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(ParticleBurstCycleCrossed(0.0f, 1.0f, nan));
    EXPECT_FALSE(ParticleBurstCycleCrossed(0.0f, 1.0f, inf));
    EXPECT_FALSE(ParticleBurstCycleCrossed(nan, 1.0f, 1.0f));
    EXPECT_FALSE(ParticleBurstCycleCrossed(0.0f, inf, 1.0f));
}

TEST(ParticleBurst, LongRunningTimeStillCrossesOnBoundary)
{
    // 用 double 做除法，3600 秒附近仍能识别出 1 秒周期的跨帧。
    EXPECT_TRUE(ParticleBurstCycleCrossed(3599.9f, 3600.1f, 1.0f));
    EXPECT_FALSE(ParticleBurstCycleCrossed(3600.1f, 3600.9f, 1.0f));
}
