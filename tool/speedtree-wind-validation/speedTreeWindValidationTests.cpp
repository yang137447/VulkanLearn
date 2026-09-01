#include <gtest/gtest.h>

#include "speedTreeWindValidation.h"

TEST(SpeedTreeWind, NumericParity)
{
    char executableName[] = "speedtree_wind_validation";
    char* arguments[] = {executableName};
    EXPECT_EQ(RunSpeedTreeWindValidation(1, arguments), 0);
}
