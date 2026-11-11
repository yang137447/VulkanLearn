#include <array>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "particles/particleEmitterKey.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleEmitterKey, JoinsPlainSegments)
{
    EXPECT_EQ(
        BuildParticleEmitterKey("Campfire", "Maps/PS_fire.json", "fire"),
        "Campfire|Maps/PS_fire.json|fire");
}

TEST(ParticleEmitterKey, EscapesPipesAndBackslashes)
{
    EXPECT_EQ(EscapeParticleEmitterKeySegment("a|b"), "a\\|b");
    EXPECT_EQ(EscapeParticleEmitterKeySegment("a\\b"), "a\\\\b");
    EXPECT_EQ(EscapeParticleEmitterKeySegment("a\\|b"), "a\\\\\\|b");
}

TEST(ParticleEmitterKey, KeepsEmptySegmentsDistinct)
{
    EXPECT_EQ(BuildParticleEmitterKey("", "", ""), "||");
    EXPECT_EQ(BuildParticleEmitterKey("|", "", ""), "\\|||");
    EXPECT_NE(
        BuildParticleEmitterKey("|", "", ""),
        BuildParticleEmitterKey("", "|", ""));
}

TEST(ParticleEmitterKey, SeparatorPlacementCannotCollide)
{
    const std::vector<std::array<std::string, 3>> segments = {
        {"a|b", "c", "d"},
        {"a", "b|c", "d"},
        {"a", "b", "c|d"},
        {"a\\b", "c", "d"},
        {"a", "b\\c", "d"},
        {"a", "b", "c\\d"},
        {"a\\|b", "c", "d"},
        {"a", "b\\|c", "d"},
        {"a", "b", "c\\|d"},
        {"a\\\\b", "c", "d"}};

    std::set<std::string> keys;
    for (const auto& segment : segments)
    {
        const std::string key = BuildParticleEmitterKey(
            segment[0], segment[1], segment[2]);
        EXPECT_TRUE(keys.insert(key).second) << key;
    }
}
