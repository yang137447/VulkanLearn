#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "particles/particleEmitterKey.h"
#include "particles/particleRuntimeCommand.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleRuntimeCommand, SpawnAcceptsNamePathAndFinitePosition)
{
    std::string error;
    EXPECT_TRUE(ValidateParticleSpawnRequest(
        "TestSpark",
        "Maps/SC_particle_demo/PS_sparks.json",
        Eigen::Vector3f(500.0f, 0.0f, 0.0f),
        error));
    EXPECT_TRUE(error.empty());
}

TEST(ParticleRuntimeCommand, SpawnRejectsEmptyName)
{
    std::string error;
    EXPECT_FALSE(ValidateParticleSpawnRequest(
        "",
        "Maps/SC_particle_demo/PS_sparks.json",
        Eigen::Vector3f::Zero(),
        error));
    EXPECT_FALSE(error.empty());
}

TEST(ParticleRuntimeCommand, SpawnRejectsEmptyAssetPath)
{
    std::string error;
    EXPECT_FALSE(ValidateParticleSpawnRequest(
        "TestSpark",
        "",
        Eigen::Vector3f::Zero(),
        error));
    EXPECT_FALSE(error.empty());
}

TEST(ParticleRuntimeCommand, SpawnRejectsNonFinitePosition)
{
    std::string error;
    EXPECT_FALSE(ValidateParticleSpawnRequest(
        "TestSpark",
        "Maps/SC_particle_demo/PS_sparks.json",
        Eigen::Vector3f(
            std::numeric_limits<float>::quiet_NaN(),
            0.0f,
            0.0f),
        error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(ValidateParticleSpawnRequest(
        "TestSpark",
        "Maps/SC_particle_demo/PS_sparks.json",
        Eigen::Vector3f(
            0.0f,
            std::numeric_limits<float>::infinity(),
            0.0f),
        error));
    EXPECT_FALSE(error.empty());
}

TEST(ParticleRuntimeCommand, SpawnModelIsPureTranslation)
{
    const Eigen::Vector3f position(120.0f, -40.0f, 7.5f);
    const Eigen::Matrix4f model = BuildParticleSpawnModel(position);

    EXPECT_TRUE((model.block<3, 3>(0, 0).isApprox(
        Eigen::Matrix3f::Identity())));
    EXPECT_TRUE((model.block<3, 1>(0, 3).isApprox(position)));
    EXPECT_FLOAT_EQ(model(3, 0), 0.0f);
    EXPECT_FLOAT_EQ(model(3, 1), 0.0f);
    EXPECT_FLOAT_EQ(model(3, 2), 0.0f);
    EXPECT_FLOAT_EQ(model(3, 3), 1.0f);
}

TEST(ParticleRuntimeCommand, SceneMatchUsesEscapedKeyPrefix)
{
    const std::string key = BuildParticleEmitterKey(
        "Campfire", "Maps/PS_campfire.json", "smoke");

    EXPECT_TRUE(ParticleEmitterKeyMatchesScene(key, "Campfire"));
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene(key, "Camp"));
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene(key, "Campfire2"));
}

TEST(ParticleRuntimeCommand, SceneMatchDoesNotCrossUnescapedPipeNames)
{
    const std::string innerPipeKey = BuildParticleEmitterKey(
        "A|B", "Maps/PS.json", "smoke");

    EXPECT_TRUE(ParticleEmitterKeyMatchesScene(innerPipeKey, "A|B"));
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene(innerPipeKey, "A"));

    const std::string prefixNameKey = BuildParticleEmitterKey(
        "A", "Maps/PS.json", "smoke");
    EXPECT_TRUE(ParticleEmitterKeyMatchesScene(prefixNameKey, "A"));
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene(prefixNameKey, "A|B"));
}

TEST(ParticleRuntimeCommand, SceneMatchRequiresEmitterSegment)
{
    const std::string sceneOnly = EscapeParticleEmitterKeySegment("Campfire") + "|";
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene(sceneOnly, "Campfire"));
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene("", "Campfire"));
}

TEST(ParticleRuntimeCommand, SceneMatchHandlesBackslashNames)
{
    const std::string key = BuildParticleEmitterKey(
        "Floor\\A", "Maps/PS.json", "smoke");

    EXPECT_TRUE(ParticleEmitterKeyMatchesScene(key, "Floor\\A"));
    EXPECT_FALSE(ParticleEmitterKeyMatchesScene(key, "Floor"));
}
