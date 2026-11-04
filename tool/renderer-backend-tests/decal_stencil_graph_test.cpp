#include <fstream>
#include <string>

#include <gtest/gtest.h>

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

nlohmann::json& FindPass(nlohmann::json& graph, const std::string& name)
{
    for (nlohmann::json& pass : graph.at("passes"))
    {
        if (pass.at("name") == name)
            return pass;
    }
    throw std::runtime_error("Missing graph pass: " + name);
}

nlohmann::json& FindResource(
    nlohmann::json& graph,
    const std::string& name)
{
    for (nlohmann::json& resource : graph.at("resources"))
    {
        if (resource.at("name") == name)
            return resource;
    }
    throw std::runtime_error("Missing graph resource: " + name);
}

const VL::CompiledRenderGraphPass& RequireCompiledPass(
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

size_t RequirePassIndex(
    const VL::CompiledRenderGraph& graph,
    const std::string& name)
{
    for (size_t index = 0; index < graph.passOrder.size(); ++index)
    {
        if (graph.passOrder[index] == name)
            return index;
    }
    throw std::runtime_error("Missing compiled pass order entry: " + name);
}
} // namespace

TEST(DecalStencilGraph, UsesD24S8AttachmentAndOrdersMaskBeforeDecals)
{
    nlohmann::json source = LoadGraph();
    const nlohmann::json& stencilResource =
        FindResource(source, "decalStencil");
    ASSERT_EQ(stencilResource.at("format"), "D24_UNORM_S8_UINT");
    ASSERT_EQ(stencilResource.at("usage").at(0),
        "depthStencilAttachment");

    const auto result = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(result.IsSuccess());
    const VL::CompiledRenderGraph& graph = result.Value();

    const VL::CompiledRenderGraphPass& maskPass =
        RequireCompiledPass(graph, "decalStencil");
    EXPECT_EQ(maskPass.type, VL::RenderGraphPassType::DecalStencil);
    ASSERT_EQ(maskPass.outputResources.size(), 1u);
    EXPECT_EQ(maskPass.outputResources[0].resource, "decalStencil");
    EXPECT_EQ(maskPass.outputResources[0].loadOp, "clear");
    EXPECT_EQ(maskPass.outputResources[0].stencilLoadOp, "clear");
    EXPECT_EQ(maskPass.outputResources[0].stencilStoreOp, "store");
    EXPECT_EQ(maskPass.colorOutputCount, 0u);
    EXPECT_TRUE(maskPass.outputsDepth);

    const VL::CompiledRenderGraphPass& decalPass =
        RequireCompiledPass(graph, "decals");
    EXPECT_EQ(decalPass.type, VL::RenderGraphPassType::Decal);
    EXPECT_EQ(decalPass.colorOutputCount, 6u);
    EXPECT_TRUE(decalPass.outputsDepth);
    const std::string* lastColor = nullptr;
    for (const auto& output : decalPass.outputResources)
    {
        if (output.resource == "decalStencil")
            continue;
        lastColor = &output.resource;
    }
    ASSERT_NE(lastColor, nullptr);
    EXPECT_EQ(*lastColor, "dbufferEmissive");
    ASSERT_FALSE(decalPass.inputResources.empty());
    EXPECT_EQ(decalPass.inputResources[0], "sceneDepth");
    bool attachesStencil = false;
    bool samplesStencil = false;
    for (const auto& output : decalPass.outputResources)
    {
        if (output.resource == "decalStencil")
        {
            attachesStencil = true;
            EXPECT_EQ(output.loadOp, "load");
            EXPECT_EQ(output.stencilLoadOp, "load");
            EXPECT_EQ(output.stencilStoreOp, "dontCare");
        }
    }
    for (const std::string& input : decalPass.inputResources)
        samplesStencil = samplesStencil || input == "decalStencil";
    EXPECT_TRUE(attachesStencil);
    EXPECT_FALSE(samplesStencil);

    const size_t maskIndex = RequirePassIndex(graph, "decalStencil");
    const size_t decalIndex = RequirePassIndex(graph, "decals");
    const size_t lightingIndex =
        RequirePassIndex(graph, "deferredLighting");
    EXPECT_LT(maskIndex, decalIndex);
    EXPECT_LT(decalIndex, lightingIndex);
}

TEST(DecalStencilGraph, PassTypeNameIsRegistered)
{
    const auto parsed =
        VL::ParseRenderGraphPassType("decalStencil");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, VL::RenderGraphPassType::DecalStencil);
    EXPECT_EQ(
        VL::RenderGraphPassTypeToString(
            VL::RenderGraphPassType::DecalStencil),
        "decalStencil");
}

TEST(DecalStencilGraph, PipelineStateCarriesBit0MaskAndNotEqualGate)
{
    const auto result = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess());

    const VL::CompiledRenderGraphPass& maskPass =
        RequireCompiledPass(result.Value(), "decalStencil");
    const VL::CompiledGraphicsPipelineState& maskState =
        maskPass.pipelineState;
    EXPECT_TRUE(maskState.stencilTestEnable);
    EXPECT_EQ(maskState.stencilWriteMask, 0x01u);
    EXPECT_EQ(maskState.stencilCompareMask, 0x01u);
    EXPECT_EQ(maskState.stencilReference, 0x01u);
    EXPECT_EQ(maskState.stencilCompareOp,
        VL::CompiledStencilCompareOp::Always);
    EXPECT_EQ(maskState.stencilPassOp,
        VL::CompiledStencilOp::Replace);
    EXPECT_EQ(maskState.stencilFailOp,
        VL::CompiledStencilOp::Keep);
    EXPECT_EQ(maskState.stencilDepthFailOp,
        VL::CompiledStencilOp::Keep);

    const VL::CompiledRenderGraphPass& decalPass =
        RequireCompiledPass(result.Value(), "decals");
    const VL::CompiledGraphicsPipelineState& decalState =
        decalPass.pipelineState;
    EXPECT_TRUE(decalState.stencilTestEnable);
    EXPECT_EQ(decalState.stencilWriteMask, 0u);
    EXPECT_EQ(decalState.stencilCompareMask, 0x01u);
    EXPECT_EQ(decalState.stencilReference, 0u);
    EXPECT_EQ(decalState.stencilCompareOp,
        VL::CompiledStencilCompareOp::NotEqual);
    EXPECT_EQ(decalState.stencilPassOp,
        VL::CompiledStencilOp::Keep);
}

TEST(DecalStencilGraph, NonDecalPassesKeepStencilDisabledByDefault)
{
    nlohmann::json source = LoadGraph();
    nlohmann::json& geometry = FindPass(source, "geometry");
    geometry["pipelineState"].erase("stencil");

    const auto result = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(result.IsSuccess());
    const VL::CompiledGraphicsPipelineState& state =
        RequireCompiledPass(result.Value(), "geometry").pipelineState;
    EXPECT_FALSE(state.stencilTestEnable);
    EXPECT_EQ(state.stencilWriteMask, 0xFFu);
    EXPECT_EQ(state.stencilCompareMask, 0xFFu);
    EXPECT_EQ(state.stencilReference, 0u);
    EXPECT_EQ(state.stencilCompareOp,
        VL::CompiledStencilCompareOp::Always);
}

TEST(DecalStencilGraph, RejectsPassThatSamplesAndAttachesSameResource)
{
    nlohmann::json source = LoadGraph();
    nlohmann::json& geometry = FindPass(source, "geometry");
    geometry["input"].push_back({
        {"resource", "sceneColorBase"},
        {"binding", 0u}});

    const auto result = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code,
        "RenderGraphCompiler.PassFeedbackLoop");
}

TEST(DecalStencilGraph, RejectsStencilTestWithoutStencilAttachment)
{
    nlohmann::json source = LoadGraph();
    nlohmann::json& geometry = FindPass(source, "geometry");
    geometry["pipelineState"]["stencil"] = {
        {"testEnable", true},
        {"writeMask", 1u},
        {"compareMask", 1u},
        {"reference", 1u},
        {"compareOp", "always"},
        {"passOp", "replace"}};

    const auto result = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code,
        "RenderGraphCompiler.MissingStencilAttachment");
}

TEST(DecalStencilGraph, RejectsUnsupportedStencilOperationsAndMasks)
{
    nlohmann::json source = LoadGraph();
    FindPass(source, "decalStencil")
        ["pipelineState"]["stencil"]["passOp"] = "bogus";
    auto invalidOp = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(invalidOp.IsFailure());
    EXPECT_EQ(invalidOp.Error().code,
        "RenderGraphCompiler.InvalidPipelineState");

    source = LoadGraph();
    FindPass(source, "decalStencil")
        ["pipelineState"]["stencil"]["writeMask"] = 256;
    auto invalidMask = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(invalidMask.IsFailure());
    EXPECT_EQ(invalidMask.Error().code,
        "RenderGraphCompiler.InvalidPipelineState");

    source = LoadGraph();
    FindPass(source, "decalStencil")
        ["output"].at(0)["stencilLoadOp"] = "bogus";
    auto invalidLoadOp = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(invalidLoadOp.IsFailure());
    EXPECT_EQ(invalidLoadOp.Error().code,
        "RenderGraphCompiler.UnsupportedStencilLoadOp");
}
