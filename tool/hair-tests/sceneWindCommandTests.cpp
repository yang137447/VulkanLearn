#include <gtest/gtest.h>

#include <limits>
#include "world/sceneWindCommand.h"
#include "world/sceneWindRuntime.h"

namespace
{
TEST(SceneWindCommand, ParsesReadAndExplicitStrengthWithoutSchedule)
{
    VL::SceneWindCommandRequest request;
    std::string error;
    ASSERT_TRUE(VL::ParseSceneWindCommand(" get ", request, error));
    EXPECT_EQ(request.operation, "get");
    ASSERT_TRUE(VL::ParseSceneWindCommand("strength 2000", request, error));
    EXPECT_EQ(request.operation, "strength");
    EXPECT_FLOAT_EQ(request.strength, 2000.0f);
    ASSERT_TRUE(VL::ParseSceneWindCommand("strength 0", request, error));
    EXPECT_FLOAT_EQ(request.strength, 0.0f);
}

TEST(SceneWindCommand, RejectsMalformedValuesAtomically)
{
    VL::SceneWindCommandRequest request{"strength", 400.0f};
    std::string error;
    for (const char* arguments : {"", "reset", "get 2000", "strength",
             "strength -1", "strength nan", "strength inf", "strength 1e99",
             "strength 2000junk", "strength 2000 3", "strength 2000;quit"})
    {
        EXPECT_FALSE(VL::ParseSceneWindCommand(arguments, request, error)) << arguments;
        EXPECT_EQ(request.operation, "strength");
        EXPECT_FLOAT_EQ(request.strength, 400.0f);
        EXPECT_FALSE(error.empty());
    }
}

TEST(SceneWindCommand, RejectsMissingWindAndInvalidDirectRequests)
{
    VL::WorldWind wind;
    std::string error;
    EXPECT_FALSE(VL::ApplySceneWindCommand(wind, {"strength", 2000.0f}, error));
    EXPECT_NE(error.find("no authored wind"), std::string::npos);
    EXPECT_FLOAT_EQ(wind.strength, 0.0f);
    wind.authored = true;
    for (float strength : {-1.0f, std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()})
        EXPECT_FALSE(VL::ApplySceneWindCommand(wind, {"strength", strength}, error));
    EXPECT_FALSE(VL::ApplySceneWindCommand(wind, {"unknown", 20.0f}, error));
    EXPECT_FLOAT_EQ(wind.strength, 0.0f);
}

TEST(SceneWindCommand, PreservesDirectionGustAndAuthoredIdentity)
{
    VL::WorldWind wind;
    wind.authored = true;
    wind.name = "MainWind";
    wind.direction = Eigen::Vector3f(0, 0, -1);
    wind.gust = {true, 0.2f, 0.5f, 0.3f};
    std::string error;
    ASSERT_TRUE(VL::ApplySceneWindCommand(wind, {"strength", 2000}, error));
    EXPECT_EQ(wind.name, "MainWind");
    EXPECT_TRUE(wind.authored);
    EXPECT_EQ(wind.direction, Eigen::Vector3f(0, 0, -1));
    EXPECT_TRUE(wind.gust.enabled);
    EXPECT_FLOAT_EQ(wind.gust.amplitude, 0.2f);
    EXPECT_FLOAT_EQ(wind.gust.frequency, 0.5f);
    EXPECT_FLOAT_EQ(wind.gust.turbulence, 0.3f);
    ASSERT_TRUE(VL::ApplySceneWindCommand(wind, {"get", 0}, error));
    EXPECT_FLOAT_EQ(wind.strength, 2000);
}

TEST(SceneWindCommand, RetainsExistingEasingAcrossStrengthChanges)
{
    VL::WorldWind wind;
    wind.authored = true;
    VL::SceneWindRuntime runtime;
    EXPECT_FLOAT_EQ(runtime.Advance(wind, 0.0).strength, 0);
    std::string error;
    ASSERT_TRUE(VL::ApplySceneWindCommand(wind, {"strength", 2000}, error));
    const float rising = runtime.Advance(wind, 0.05).strength;
    EXPECT_GT(rising, 0);
    EXPECT_LT(rising, 2000);
    EXPECT_NEAR(rising, 2000.0 * (1.0 - std::exp(-0.05 / 0.5)), 0.001);
    ASSERT_TRUE(VL::ApplySceneWindCommand(wind, {"strength", 0}, error));
    const float falling = runtime.Advance(wind, 0.1).strength;
    EXPECT_GT(falling, 0);
    EXPECT_LT(falling, rising);
}

TEST(SceneWindCommand, ReceiptNamesWorldGenerationAndRuntimeOnlyOverride)
{
    VL::WorldWind wind;
    wind.authored = true;
    wind.name = "MainWind";
    wind.strength = 2000;
    const auto receipt = VL::MakeSceneWindCommandReceipt(
        wind, {"strength", 2000}, 0, 17, 123.5);
    EXPECT_EQ(receipt.at("command"), "wind");
    EXPECT_EQ(receipt.at("status"), "applied");
    EXPECT_EQ(receipt.at("worldGeneration"), 17);
    EXPECT_EQ(receipt.at("steadyTimeSeconds"), 123.5);
    EXPECT_EQ(receipt.at("previousTargetStrengthCmPerSecond"), 0);
    EXPECT_EQ(receipt.at("targetStrengthCmPerSecond"), 2000);
    EXPECT_EQ(receipt.at("strengthOverrideApplied"), true);
    EXPECT_EQ(receipt.at("runtimeOnly"), true);
}
}
