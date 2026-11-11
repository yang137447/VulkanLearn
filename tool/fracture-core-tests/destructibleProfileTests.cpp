#include <gtest/gtest.h>

#include "physics/fracture/destructibleProfile.h"

namespace
{
using namespace VL::Physics;
using namespace VL::Physics::Fracture;
} // namespace

TEST(DestructibleProfile, DefaultProfilesKeepExistingMaterialAndBudgetKeys)
{
    EXPECT_EQ(DefaultGlassProfile().materialProfile,
        FractureMaterialProfile::Glass);
    EXPECT_EQ(DefaultGlassProfile().dynamicBudgetClass, "glass.dynamic");
    EXPECT_EQ(DefaultGlassProfile().attachedBudgetClass, "glass.attached");
    EXPECT_EQ(DefaultGlassProfile().audioProfile, "glass");

    EXPECT_EQ(DefaultBalloonProfile().materialProfile,
        FractureMaterialProfile::Balloon);
    EXPECT_EQ(DefaultBalloonProfile().shardBudgetClass, "balloon.shard");
    EXPECT_EQ(DefaultBalloonProfile().audioProfile, "balloon");
}

TEST(DestructibleProfile, RegistryReturnsProfilesAndNullForUnknownKeys)
{
    const auto* glass = FindDestructibleProfile("glass.default");
    ASSERT_NE(glass, nullptr);
    EXPECT_EQ(glass->materialProfile, FractureMaterialProfile::Glass);
    const auto* balloon = FindDestructibleProfile("balloon.default");
    ASSERT_NE(balloon, nullptr);
    EXPECT_EQ(balloon->materialProfile, FractureMaterialProfile::Balloon);
    EXPECT_EQ(FindDestructibleProfile("missing"), nullptr);
    EXPECT_EQ(FindDestructibleProfile(""), nullptr);
}

TEST(DestructibleProfile, TemperedGlassIsAProfileVariantNotANewRuntime)
{
    const auto* tempered = FindDestructibleProfile("glass.tempered");
    ASSERT_NE(tempered, nullptr);
    EXPECT_EQ(tempered->materialProfile, FractureMaterialProfile::Glass);
    EXPECT_GT(tempered->maxSubfractureDepth,
        DefaultGlassProfile().maxSubfractureDepth);
    EXPECT_LT(tempered->minProjectileSubfractureArea,
        DefaultGlassProfile().minProjectileSubfractureArea);
}
