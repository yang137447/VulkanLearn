#include <algorithm>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "render/decal/decalEmissiveComposite.h"
#include "render/decal/decalRenderer.h"
#include "render/rendergraph/renderGraphCompiler.h"

namespace
{

nlohmann::json LoadGraph()
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) +
        "/config/renderGraphConfig.json");
    return nlohmann::json::parse(input);
}

const VL::CompiledRenderGraphPass& FindPass(
    const VL::CompiledRenderGraph& graph,
    const std::string& name)
{
    for (const VL::CompiledRenderGraphPass& pass : graph.passes)
    {
        if (pass.name == name)
            return pass;
    }
    throw std::runtime_error("Missing compiled pass: " + name);
}

const VL::CompiledRenderGraphResource& FindResource(
    const VL::CompiledRenderGraph& graph,
    const std::string& name)
{
    for (const VL::CompiledRenderGraphResource& resource : graph.resources)
    {
        if (resource.name == name)
            return resource;
    }
    throw std::runtime_error("Missing compiled resource: " + name);
}

uint32_t FindInputBinding(
    const VL::CompiledRenderGraphPass& pass,
    const std::string& resource)
{
    for (const VL::CompiledRenderGraphPassInputDescriptor& input :
         pass.inputDescriptors)
    {
        if (input.resource == resource)
            return input.binding;
    }
    throw std::runtime_error(
        "Missing pass input: " + pass.name + ":" + resource);
}

} // namespace

TEST(DecalEmissiveGraph, ResourceAndPassOutputsAreWired)
{
    const auto compiled = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(compiled.IsSuccess());
    const VL::CompiledRenderGraph& graph = compiled.Value();

    const VL::CompiledRenderGraphResource& resource =
        FindResource(graph, "dbufferEmissive");
    EXPECT_EQ(resource.format, "R16G16B16A16_SFLOAT");
    EXPECT_NE(
        std::find(resource.usage.begin(), resource.usage.end(),
            "colorAttachment"),
        resource.usage.end());
    EXPECT_NE(
        std::find(resource.usage.begin(), resource.usage.end(),
            "sampled"),
        resource.usage.end());

    const VL::CompiledRenderGraphPass& decals =
        FindPass(graph, "decals");
    EXPECT_EQ(decals.colorOutputCount, 6u);
    std::vector<std::string> colorOutputs;
    for (const VL::CompiledRenderGraphPassOutput& output :
         decals.outputResources)
    {
        if (output.resource != "decalStencil")
            colorOutputs.push_back(output.resource);
    }
    const std::vector<std::string> expected = {
        "dbufferA", "dbufferB", "dbufferC",
        "dbufferD", "dbufferMask", "dbufferEmissive"};
    EXPECT_EQ(colorOutputs, expected);
}

TEST(DecalEmissiveGraph, ConsumersBindTheEmissiveBuffer)
{
    const auto compiled = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(compiled.IsSuccess());
    const VL::CompiledRenderGraph& graph = compiled.Value();

    EXPECT_EQ(
        FindInputBinding(FindPass(graph, "deferredLighting"),
            "dbufferEmissive"),
        16u);
    EXPECT_EQ(
        FindInputBinding(FindPass(graph, "forwardOpaque"),
            "dbufferEmissive"),
        7u);
    EXPECT_EQ(
        FindInputBinding(FindPass(graph, "forwardTransparent"),
            "dbufferEmissive"),
        7u);
}

TEST(DecalEmissiveGpu, InstanceLayoutMatchesStd430Stride)
{
    EXPECT_EQ(sizeof(VL::DecalGpuInstance), 208u);
    EXPECT_EQ(offsetof(VL::DecalGpuInstance, decalToWorld), 0u);
    EXPECT_EQ(offsetof(VL::DecalGpuInstance, color), 128u);
    EXPECT_EQ(offsetof(VL::DecalGpuInstance, surface), 144u);
    EXPECT_EQ(offsetof(VL::DecalGpuInstance, params), 160u);
    EXPECT_EQ(offsetof(VL::DecalGpuInstance, normal), 176u);
    EXPECT_EQ(offsetof(VL::DecalGpuInstance, emissive), 192u);
}

TEST(DecalEmissiveComposite, CoverageZeroWritesNothing)
{
    const VL::DecalEmissiveColor written = VL::DecalWriteEmissive(
        VL::DecalEmissiveColor{4.0f, 2.0f, 1.0f}, 8.0f, 0.0f);
    EXPECT_FLOAT_EQ(written.r, 0.0f);
    EXPECT_FLOAT_EQ(written.g, 0.0f);
    EXPECT_FLOAT_EQ(written.b, 0.0f);
}

TEST(DecalEmissiveComposite, FullCoverageWritesEmissiveTimesStrength)
{
    const VL::DecalEmissiveColor written = VL::DecalWriteEmissive(
        VL::DecalEmissiveColor{0.5f, 2.0f, 4.0f}, 3.0f, 1.0f);
    EXPECT_FLOAT_EQ(written.r, 1.5f);
    EXPECT_FLOAT_EQ(written.g, 6.0f);
    EXPECT_FLOAT_EQ(written.b, 12.0f);
}

TEST(DecalEmissiveComposite, PremultipliedOverComposition)
{
    const VL::DecalEmissiveColor first = VL::DecalWriteEmissive(
        VL::DecalEmissiveColor{1.0f, 1.0f, 1.0f}, 2.0f, 0.5f);
    const VL::DecalEmissiveColor second = VL::DecalWriteEmissive(
        VL::DecalEmissiveColor{2.0f, 0.0f, 0.0f}, 1.0f, 0.25f);
    const VL::DecalEmissiveColor composited =
        VL::DecalCompositeEmissive(second, 0.25f, first);

    // 第二层覆盖 .25，先画的 1.0 红只剩下 75%：
    // 0.5 + 1.0 * (1 - 0.25) = 1.25
    EXPECT_FLOAT_EQ(composited.r, 1.25f);
    EXPECT_FLOAT_EQ(composited.g, 0.75f);
    EXPECT_FLOAT_EQ(composited.b, 0.75f);

    const VL::DecalEmissiveColor resolved =
        VL::ResolveDecalEmissive(composited);
    EXPECT_FLOAT_EQ(resolved.r, composited.r);
    EXPECT_FLOAT_EQ(resolved.g, composited.g);
    EXPECT_FLOAT_EQ(resolved.b, composited.b);
}
