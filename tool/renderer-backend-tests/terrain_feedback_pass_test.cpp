#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

#include "render/rendergraph/renderGraphCompiler.h"

namespace
{

nlohmann::json LoadGraph()
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) +
        "/config/renderGraphConfig.json");
    if (!input)
        throw std::runtime_error("Cannot open renderGraphConfig.json");
    return nlohmann::json::parse(input);
}

nlohmann::json& FeedbackPass(nlohmann::json& graph)
{
    for (auto& pass : graph.at("passes"))
        if (pass.at("type") == "terrainFeedback")
            return pass;
    throw std::runtime_error("Missing terrainFeedback fixture pass");
}

nlohmann::json& FeedbackResource(nlohmann::json& graph)
{
    const std::string name =
        FeedbackPass(graph).at("output").at(0).at("resource")
            .get<std::string>();
    for (auto& resource : graph.at("resources"))
        if (resource.at("name") == name)
            return resource;
    throw std::runtime_error("Missing terrainFeedback resource");
}

nlohmann::json& FeedbackDepthResource(nlohmann::json& graph)
{
    // L4b 起 output 是 1..5 张颜色 + 最后一张深度，不能再用固定下标。
    for (auto& output : FeedbackPass(graph).at("output"))
    {
        const std::string name = output.at("resource").get<std::string>();
        for (auto& resource : graph.at("resources"))
        {
            if (resource.at("name") != name) continue;
            const std::string format =
                resource.value("format", std::string());
            if (format.rfind("D", 0) == 0)
                return resource;
        }
    }
    throw std::runtime_error("Missing terrainFeedback depth resource");
}

} // namespace

TEST(TerrainFeedbackGraph, CompilesDedicatedColorAndDepthPass)
{
    const auto result =
        VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess());

    const VL::CompiledRenderGraphPass* feedbackPass = nullptr;
    for (const auto& pass : result.Value().passes)
        if (pass.type == VL::RenderGraphPassType::TerrainFeedback)
            feedbackPass = &pass;
    ASSERT_NE(feedbackPass, nullptr);
    EXPECT_TRUE(feedbackPass->needCreateMaterial);
    EXPECT_FALSE(feedbackPass->needMsaa);
    EXPECT_TRUE(feedbackPass->outputsDepth);
    // 覆盖页 1 张 + L4b 层页 4 张；深度固定在最后。
    EXPECT_EQ(feedbackPass->colorOutputCount, 5u);
    ASSERT_EQ(feedbackPass->outputResources.size(), 6u);
    EXPECT_EQ(
        feedbackPass->outputResources[0].resource,
        "terrainFeedback");
    EXPECT_EQ(
        feedbackPass->outputResources[1].resource,
        "terrainLayerFeedback0");
    EXPECT_EQ(
        feedbackPass->outputResources[4].resource,
        "terrainLayerFeedback3");
    EXPECT_EQ(
        feedbackPass->outputResources[5].resource,
        "terrainFeedbackDepth");

    bool foundResource = false;
    for (const auto& resource : result.Value().resources)
    {
        if (resource.name != "terrainFeedback")
            continue;
        foundResource = true;
        EXPECT_EQ(resource.format, "R8G8B8A8_UNORM");
        EXPECT_TRUE(resource.hasFixedWidth);
        EXPECT_TRUE(resource.hasFixedHeight);
        EXPECT_EQ(resource.widthValue, 128.0f);
        EXPECT_EQ(resource.heightValue, 128.0f);
        EXPECT_NE(
            std::find(
                resource.usage.begin(),
                resource.usage.end(),
                "transferSrc"),
            resource.usage.end());
    }
    EXPECT_TRUE(foundResource);
}

TEST(TerrainFeedbackGraph, RejectsNonFixedOrWrongSizedOutput)
{
    {
        nlohmann::json graph = LoadGraph();
        nlohmann::json& resource = FeedbackResource(graph);
        resource.erase("widthSize");
        resource["widthScale"] = 0.125f;
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackResource(graph)["widthSize"] = 64;
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackResource(graph)["format"] = "R16G16B16A16_SFLOAT";
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
}

TEST(TerrainFeedbackGraph, RejectsMsaaMissingDepthAndExtraOutputs)
{
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["needMsaa"] = true;
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["pipelineState"]["depthTestEnable"] = false;
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        // 去掉最后一项（深度）后只剩颜色附件。
        nlohmann::json& outputs = FeedbackPass(graph)["output"];
        outputs.erase(outputs.size() - 1);
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["output"].push_back(
            { { "resource", "gbufferA" },
              { "loadOp", "clear" },
              { "storeOp", "store" } });
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["output"].push_back(
            { { "resource", "sceneDepth" },
              { "loadOp", "clear" },
              { "storeOp", "store" } });
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
}

TEST(TerrainFeedbackGraph, RejectsNonD32DepthFormat)
{
    nlohmann::json graph = LoadGraph();
    FeedbackDepthResource(graph)["format"] = "D24_UNORM_S8_UINT";
    const auto result = VL::RenderGraphCompiler{}.Compile(graph);
    ASSERT_TRUE(result.IsFailure());
    // 深度格式不是任意深度格式：必须是可与 shader/回读约定对齐的 D32_SFLOAT。
    EXPECT_NE(
        result.Error().message.find("D32_SFLOAT"), std::string::npos);
}

TEST(TerrainFeedbackGraph, RejectsSwapChainAndWrongUsage)
{
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["output"][0]["resource"] = "swapChain";
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        nlohmann::json& resource = FeedbackResource(graph);
        resource["usage"] = { "colorAttachment" };
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
}

TEST(TerrainFeedbackGraph, RequiresWorldTextureInputAndMaterial)
{
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["input"] = nlohmann::json::array();
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["input"][0]["source"] = "renderGraph";
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
    {
        nlohmann::json graph = LoadGraph();
        FeedbackPass(graph)["needCreateMaterial"] = false;
        EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
    }
}
