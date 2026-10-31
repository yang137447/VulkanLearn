#include <gtest/gtest.h>

#include "world/world.h"

namespace
{

VL::WorldParticleSystem MakeParticleSystemInstance(
    const std::string& name,
    const std::string& assetPath)
{
    VL::WorldParticleSystem system;
    system.name = name;
    system.assetPath = assetPath;
    return system;
}

} // namespace

TEST(ParticleWorldRuntime, RemoveParticleSystemRemovesMatchingName)
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    world.AddParticleSystem(
        "Spark_B",
        MakeParticleSystemInstance(
            "Spark_B", "Maps/PS_sparks.json"));

    EXPECT_TRUE(world.RemoveParticleSystem("Spark_A"));
    EXPECT_EQ(world.FindParticleSystem("Spark_A"), nullptr);
    EXPECT_NE(world.FindParticleSystem("Spark_B"), nullptr);
    EXPECT_EQ(world.GetParticleSystems().size(), 1u);
}

TEST(ParticleWorldRuntime, RemoveMissingParticleSystemReturnsFalse)
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));

    EXPECT_FALSE(world.RemoveParticleSystem("Spark_Missing"));
    EXPECT_NE(world.FindParticleSystem("Spark_A"), nullptr);
}

TEST(ParticleWorldRuntime, AddParticleSystemWithSameNameReplaces)
{
    VL::World world(3, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_sparks.json"));
    world.AddParticleSystem(
        "Spark_A",
        MakeParticleSystemInstance(
            "Spark_A", "Maps/PS_fire.json"));

    ASSERT_EQ(world.GetParticleSystems().size(), 1u);
    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Spark_A");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->assetPath, "Maps/PS_fire.json");
}

TEST(ParticleWorldRuntime, ParticleSystemMapKeepsInsertionIdentity)
{
    VL::World world(4, "Maps/SC_test/SC_test.json");
    world.AddParticleSystem(
        "Campfire",
        MakeParticleSystemInstance(
            "Campfire", "Maps/PS_campfire.json"));

    const VL::WorldParticleSystem* system =
        world.FindParticleSystem("Campfire");
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->name, "Campfire");
    EXPECT_EQ(system->assetPath, "Maps/PS_campfire.json");
}
