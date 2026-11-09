#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

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

TEST(ParticleSoftDepth, GraphDeclaresHalfResPassBeforeParticle)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) +
        "/config/renderGraphConfig.json");
    nlohmann::json graph;
    input >> graph;

    bool hasResource = false;
    for (const auto& resource : graph.at("resources"))
    {
        if (resource.value("name", std::string()) !=
            "particleSoftDepth")
            continue;
        hasResource = true;
        EXPECT_EQ(resource.value("format", std::string()), "R32_SFLOAT");
        EXPECT_FLOAT_EQ(resource.value("widthScale", 0.0f), 0.5f);
        EXPECT_FLOAT_EQ(resource.value("heightScale", 0.0f), 0.5f);
    }
    EXPECT_TRUE(hasResource);

    size_t softDepthPass = std::string::npos;
    size_t particlePass = std::string::npos;
    const auto& passes = graph.at("passes");
    for (size_t index = 0; index < passes.size(); ++index)
    {
        const std::string name =
            passes.at(index).value("name", std::string());
        if (name == "particleSoftDepth") softDepthPass = index;
        if (name == "particle") particlePass = index;
    }
    ASSERT_NE(softDepthPass, std::string::npos);
    ASSERT_NE(particlePass, std::string::npos);
    EXPECT_LT(softDepthPass, particlePass);

    bool particleHasSoftDepth = false;
    for (const auto& inputDesc :
         passes.at(particlePass).at("input"))
    {
        if (inputDesc.value("resource", std::string()) ==
            "particleSoftDepth")
        {
            particleHasSoftDepth =
                inputDesc.value("binding", -1) == 3;
        }
    }
    EXPECT_TRUE(particleHasSoftDepth);
}

TEST(ParticleSoftDepth, ComputeShaderWritesLinearR32Depth)
{
    const std::string compute =
        ReadSource("shader/glsl/particle/particleSoftDepth.comp");
    EXPECT_TRUE(Contains(compute, "binding = 0"));
    EXPECT_TRUE(Contains(compute, "binding = 1, r32f"));
    EXPECT_TRUE(Contains(compute, "imageStore(softDepthOut"));
    EXPECT_TRUE(Contains(compute, "ParticleViewDepthFromDeviceDepth"));
    EXPECT_TRUE(Contains(compute, "for (int y = -1; y <= 1; ++y)"));
}

TEST(ParticleSoftDepth, ParticleShadersBindSoftDepthAndKeepFallback)
{
    for (const char* path : {
        "shader/glsl/particle/particle.frag",
        "shader/glsl/particle/particleMesh.frag",
        "shader/glsl/particle/particleRibbon.frag",
        "shader/glsl/particle/particleBeam.frag",
        "shader/glsl/particle/particleDistortion.frag",
        "shader/glsl/particle/particleSkeletalFragmentBody.glsl",
        "shader/glsl/particle/particleGlobal.frag"})
    {
        const std::string source = ReadSource(path);
        EXPECT_TRUE(Contains(source, "binding = 3")) << path;
        EXPECT_TRUE(Contains(source, "SampleParticleSoftViewDepth"))
            << path;
        EXPECT_TRUE(Contains(source, "ParticleSoftFade")) << path;
    }

    const std::string helper =
        ReadSource("shader/glsl/particle/particleSoftDepth.glsl");
    EXPECT_TRUE(Contains(helper, "SampleParticleSceneViewDepth"));
    EXPECT_TRUE(Contains(helper, "softSize.x <= 1"));
    EXPECT_TRUE(Contains(helper, "thicknessCm > 0.0"));
}

TEST(ParticleSoftDepth, DescriptorLayoutsExposeBindingThree)
{
    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(
        renderer, "MakeParticlePassSetBinding(3, \"softDepth\")"));
    EXPECT_TRUE(Contains(
        renderer,
        "softDepth.stageFlags = vk::ShaderStageFlagBits::eFragment;"));
    EXPECT_TRUE(Contains(renderer, "\"particle/particleSoftDepth\""));
}
