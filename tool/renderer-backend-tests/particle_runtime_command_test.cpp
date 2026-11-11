#include <gtest/gtest.h>

#include "particleSourceTestUtils.h"

#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadSource(const std::string& path)
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) + "/" + path);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

bool Contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST(ParticleRuntimeCommandShader, SimulateBindsRequestBufferAtBindingNine)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleSimulate.comp");
    EXPECT_TRUE(Contains(source, "binding = 9"));
    EXPECT_TRUE(Contains(source, "kParticleRequestCapacity = 16u"));
    EXPECT_TRUE(Contains(source, "struct ParticleRuntimeRequest"));
    EXPECT_TRUE(Contains(source, "positionLifetime"));
    EXPECT_TRUE(Contains(source, "uvec4 kindIndex"));
    EXPECT_TRUE(Contains(source, "SpawnParticleAtPosition"));
    EXPECT_TRUE(Contains(source, "particle.positionAge.w = particle.velocityLife.w"));
    EXPECT_TRUE(Contains(source, "request.kindIndex.y != index"));
    EXPECT_TRUE(Contains(source, "max(lifetime, 1.0e-4)"));
}

TEST(ParticleRuntimeCommandShader, RendererOwnsBoundedRequestQueue)
{
    const std::string renderer =
        ParticleSourceTest::ReadParticleRendererSources();
    EXPECT_TRUE(Contains(renderer, "RequestParticleKill"));
    EXPECT_TRUE(Contains(renderer, "RequestParticleSpawnAt"));
    EXPECT_TRUE(Contains(renderer, "pendingParticleRequests"));
    EXPECT_TRUE(Contains(renderer, "ParticleRuntimeRequestGpu"));
    EXPECT_TRUE(Contains(renderer, "kParticleRuntimeRequestCapacity = 16"));
    EXPECT_TRUE(Contains(renderer, "emitter.runtimeRequests.mapped"));
    EXPECT_TRUE(Contains(renderer, "setDstBinding(9)"));
    EXPECT_TRUE(Contains(renderer, "inverseModel"));
    EXPECT_TRUE(Contains(renderer, "nextRuntimeSpawnSlot"));
}

TEST(ParticleRuntimeCommandShader, ConsoleAndExecutorExposeBothCommands)
{
    const std::string console =
        ReadSource("source/debugConsole.cpp") +
        ReadSource("source/debugConsoleProcessCommand.cpp");
    EXPECT_TRUE(Contains(console, "particlekill"));
    EXPECT_TRUE(Contains(console, "particlespawnat"));
    EXPECT_TRUE(Contains(console, "ValidateParticleKillRequest"));
    EXPECT_TRUE(Contains(console, "ValidateParticleSpawnAtRequest"));

    const std::string executor =
        ReadSource("source/engine/runtimeCommandExecutor.cpp") +
        ReadSource("source/engine/runtimeCommandExecutorExecuteCommand.cpp");
    EXPECT_TRUE(Contains(executor, "RuntimeCommandType::KillParticle"));
    EXPECT_TRUE(Contains(executor, "RuntimeCommandType::SpawnParticleAt"));
    EXPECT_TRUE(Contains(executor, "FindParticleEmitterIndex"));
    EXPECT_TRUE(Contains(executor, "renderSystem.KillParticle"));
    EXPECT_TRUE(Contains(executor, "renderSystem.SpawnParticleAt"));
}

