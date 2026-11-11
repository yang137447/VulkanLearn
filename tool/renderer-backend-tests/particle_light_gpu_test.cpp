#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "particles/particleLightGpu.h"

namespace
{

std::string ReadSource(const std::string& relativePath)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

} // namespace

TEST(ParticleLightGpu, LayoutMatchesStd430)
{
    EXPECT_EQ(sizeof(VL::ParticleLightSSBOHeader), 16u);
    EXPECT_EQ(sizeof(VL::ParticleLightGPU), 64u);
    EXPECT_EQ(offsetof(VL::ParticleLightGPU, colorIntensity), 16u);
    EXPECT_EQ(
        VL::ParticleLightSSBOSize(),
        sizeof(VL::ParticleLightSSBOHeader) +
            sizeof(VL::ParticleLightGPU) * 64u);
}

TEST(ParticleLightGpu, SlotAllocationIsSortedAndDeterministic)
{
    std::vector<std::pair<std::string, uint32_t>> requests{
        {"b", 8u},
        {"a", 4u},
        {"c", 2u}};
    const std::vector<VL::ParticleLightSlot> forward =
        VL::AllocateParticleLightSlots(requests);
    std::reverse(requests.begin(), requests.end());
    const std::vector<VL::ParticleLightSlot> reversed =
        VL::AllocateParticleLightSlots(requests);

    ASSERT_EQ(forward.size(), 3u);
    EXPECT_EQ(forward[0].key, "a");
    EXPECT_EQ(forward[0].base, 0u);
    EXPECT_EQ(forward[0].capacity, 4u);
    EXPECT_EQ(forward[1].key, "b");
    EXPECT_EQ(forward[1].base, 4u);
    EXPECT_EQ(forward[1].capacity, 8u);
    EXPECT_EQ(forward[2].key, "c");
    EXPECT_EQ(forward[2].base, 12u);
    EXPECT_EQ(forward[2].capacity, 2u);
    for (size_t index = 0; index < forward.size(); ++index)
    {
        EXPECT_EQ(forward[index].key, reversed[index].key);
        EXPECT_EQ(forward[index].base, reversed[index].base);
        EXPECT_EQ(forward[index].capacity, reversed[index].capacity);
    }
}

TEST(ParticleLightGpu, CapacityTruncatesInStableKeyOrder)
{
    const std::vector<VL::ParticleLightSlot> slots =
        VL::AllocateParticleLightSlots(
            {{"b", 40u}, {"a", 40u}, {"c", 4u}},
            64u);
    ASSERT_EQ(slots.size(), 3u);
    EXPECT_EQ(slots[0].key, "a");
    EXPECT_EQ(slots[0].capacity, 40u);
    EXPECT_EQ(slots[1].key, "b");
    EXPECT_EQ(slots[1].base, 40u);
    EXPECT_EQ(slots[1].capacity, 24u);
    EXPECT_EQ(slots[2].key, "c");
    EXPECT_EQ(slots[2].base, 64u);
    EXPECT_EQ(slots[2].capacity, 0u);
}

TEST(ParticleLightGpu, LightingShaderReadsSeparateBinding)
{
    const std::string source =
        ReadSource("shader/glsl/common/lighting.glsl");
    EXPECT_NE(
        source.find(
            "layout(std430, set = 0, binding = 4) "
            "readonly buffer ParticleLightSSBO"),
        std::string::npos);
    EXPECT_NE(
        source.find("CalculateParticleLightLobes"),
        std::string::npos);
    EXPECT_NE(
        source.find("particleLightSSBO.header.x"),
        std::string::npos);
}

TEST(ParticleLightGpu, EmitComputeWritesHeaderAndCentimeterIntensity)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleLightEmit.comp");
    EXPECT_NE(source.find("atomicMax"), std::string::npos);
    EXPECT_NE(
        source.find("intensity * 10000.0"),
        std::string::npos);
    EXPECT_NE(
        source.find("frame.particleLightParams"),
        std::string::npos);
}

TEST(ParticleLightGpu, RenderSystemUsesGpuPathInsteadOfCpuMirror)
{
    const std::string source = ReadSource("source/renderSystem.cpp");
    EXPECT_EQ(
        source.find("particleLightRuntime.Update"),
        std::string::npos);
    EXPECT_EQ(
        source.find("particleLightRuntime.Synchronize"),
        std::string::npos);
    EXPECT_NE(
        source.find("SetParticleLightBuffers"),
        std::string::npos);
    EXPECT_NE(
        source.find("ClearParticleLightBuffer"),
        std::string::npos);
}

TEST(ParticleLightGpu, DescriptorPlanRoutesGlobalBindingFour)
{
    const std::string plan =
        ReadSource("source/render/backend/rendererDescriptorPlan.cpp");
    EXPECT_NE(
        plan.find("binding.binding == 4"),
        std::string::npos);
    EXPECT_NE(
        plan.find("ParticleLightStorage"),
        std::string::npos);
    const std::string writer =
        ReadSource("source/render/backend/rendererDescriptorWriter.cpp");
    EXPECT_NE(
        writer.find("particleLightBufferInfos"),
        std::string::npos);
}

TEST(ParticleLightGpu, FrameResourcesOwnPerImageFixedBuffer)
{
    const std::string source =
        ReadSource("source/render/backend/rendererFrameResources.cpp");
    EXPECT_NE(
        source.find("ParticleLightSSBOSize()"),
        std::string::npos);
    EXPECT_NE(
        source.find("SSBO_ParticleLight"),
        std::string::npos);
    EXPECT_NE(
        source.find("ClearParticleLightBuffer"),
        std::string::npos);
    EXPECT_NE(
        source.find("HostWrite"),
        std::string::npos);

    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_NE(
        renderer.find("particle/particleLightEmit"),
        std::string::npos);
    EXPECT_NE(
        renderer.find("ShaderWrite"),
        std::string::npos);
    EXPECT_NE(
        renderer.find("dumpEmitter.verticesPerParticle = 0u"),
        std::string::npos);
}

TEST(ParticleLightGpu, FrameUniformCarriesSlotAndPhotometry)
{
    const std::string rendererHeader =
        ReadSource("source/particles/particleRenderer.h");
    const std::string particleCommon =
        ReadSource("shader/glsl/particle/particleCommon.glsl");
    EXPECT_NE(
        rendererHeader.find("particleLightParams"),
        std::string::npos);
    EXPECT_NE(
        particleCommon.find("particleLightParams"),
        std::string::npos);
    const std::string emitterHeader =
        ReadSource("shader/glsl/particle/particleLightEmit.comp");
    EXPECT_NE(
        emitterHeader.find("frame.particleLightParams"),
        std::string::npos);
}
