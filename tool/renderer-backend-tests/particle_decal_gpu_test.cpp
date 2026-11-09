#include <gtest/gtest.h>

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

TEST(ParticleDecalGpu, GatherShaderWritesCompactBoundedRecords)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleDecalGather.comp");
    EXPECT_TRUE(Contains(source, "binding = 1"));
    EXPECT_TRUE(Contains(source, "binding = 2"));
    EXPECT_TRUE(Contains(source, "binding = 3"));
    EXPECT_TRUE(Contains(source, "binding = 4"));
    EXPECT_TRUE(Contains(source, "binding = 5"));
    EXPECT_TRUE(Contains(source, "DecalGpuInstance instances[64]"));
    EXPECT_TRUE(Contains(source, "atomicAdd(decalOutput.header.x, 1u)"));
    EXPECT_TRUE(Contains(source, "IsParticleAlive(particle)"));
    EXPECT_TRUE(Contains(source, "min(decalCounters.alive, 64u)"));
    EXPECT_TRUE(Contains(source, "positionLifeT"));
    EXPECT_TRUE(Contains(source, "rotationStable"));
    EXPECT_TRUE(Contains(source, "colorOpacity"));
    EXPECT_TRUE(Contains(source, "lifeInfo"));
}

TEST(ParticleDecalGpu, RendererDispatchesGatherAndPublishesReadback)
{
    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(
        renderer, "particle/particleDecalGather"));
    EXPECT_TRUE(Contains(
        renderer, "ReadbackDecalGather"));
    EXPECT_TRUE(Contains(
        renderer, "RecordDecalGather"));
    EXPECT_TRUE(Contains(
        renderer, "kParticleDecalGatherBytes"));
    EXPECT_TRUE(Contains(
        renderer, "vk::BufferMemoryBarrier"));
    EXPECT_TRUE(Contains(
        renderer, "decalGpuInstances"));
    EXPECT_TRUE(Contains(
        renderer,
        "emitter.asset.renderMode == \"decal\""));
}

TEST(ParticleDecalGpu, CpuRuntimeConsumesGpuInstancesAndKeepsFallback)
{
    const std::string runtime =
        ReadSource("source/particles/particleDecalRuntime.cpp");
    EXPECT_TRUE(Contains(runtime, "gpuInstances"));
    EXPECT_TRUE(Contains(runtime, "ParticleDecalStableId"));
    EXPECT_TRUE(Contains(runtime, "EmitDecalSnapshotFromGpu"));
    EXPECT_TRUE(Contains(runtime, "gpuKeys"));
    EXPECT_TRUE(Contains(runtime, "orderedEmitters"));
}
