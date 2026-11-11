#include <gtest/gtest.h>

#include "script/scriptTypes.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleSimulationDelta, PauseFreezesParticles)
{
    EXPECT_FLOAT_EQ(
        SelectParticleSimulationDelta(
            ScriptPlayState::Paused, true, 0.25f, 1.0f / 60.0f),
        0.0f);
}

TEST(ParticleSimulationDelta, PlayingUsesScaledScriptDelta)
{
    EXPECT_FLOAT_EQ(
        SelectParticleSimulationDelta(
            ScriptPlayState::Playing, true, 0.04f, 1.0f / 60.0f),
        0.04f);
}

TEST(ParticleSimulationDelta, PlayingWithoutHostFallsBack)
{
    EXPECT_FLOAT_EQ(
        SelectParticleSimulationDelta(
            ScriptPlayState::Playing, false, 0.04f, 1.0f / 60.0f),
        1.0f / 60.0f);
}

TEST(ParticleSimulationDelta, EditingUsesEditorTime)
{
    EXPECT_FLOAT_EQ(
        SelectParticleSimulationDelta(
            ScriptPlayState::Editing, true, 0.04f, 1.0f / 30.0f),
        1.0f / 30.0f);
}
