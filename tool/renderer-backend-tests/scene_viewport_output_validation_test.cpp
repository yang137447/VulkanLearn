#include <gtest/gtest.h>

#include <stdexcept>

#include "render/rendergraph/renderGraphCompiler.h"
#include "render/rendergraph/sceneViewportOutputValidation.h"

namespace VL
{
namespace
{

nlohmann::json MakePass(
    const std::string& name,
    const std::string& type,
    const std::string& resource,
    const std::string& loadOp = "clear",
    const std::string& storeOp = "store")
{
    return {
        {"name", name},
        {"type", type},
        {"input", nlohmann::json::array()},
        {"output", nlohmann::json::array({
            {{"resource", resource}, {"loadOp", loadOp}, {"storeOp", storeOp}}
        })}
    };
}

nlohmann::json MakeGraph()
{
    return {
        {"resources", nlohmann::json::array({
            {
                {"name", "swapChain"},
                {"format", "R8G8B8A8_SRGB"},
                {"widthScale", 1},
                {"heightScale", 1},
                {"usage", {"colorAttachment", "present"}}
            },
            {
                {"name", "sceneColor"},
                {"format", "R16G16B16A16_SFLOAT"},
                {"widthScale", 1},
                {"heightScale", 1},
                {"usage", {"colorAttachment", "sampled"}}
            }
        })},
        {"passes", nlohmann::json::array({
            MakePass("geometry", "geometry", "sceneColor"),
            MakePass("opaque", "forwardOpaque", "sceneColor"),
            MakePass("transparent", "forwardTransparent", "sceneColor")
        })}
    };
}

} // namespace

TEST(SceneViewportOutputValidation, RejectsFirstWriterLoad)
{
    auto graph = MakeGraph();
    graph["passes"].push_back(MakePass("first", "postProcess", "swapChain", "load"));
    graph["passes"].push_back(MakePass("final", "postProcess", "swapChain"));
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    try
    {
        ValidateSceneViewportOutputs(compiled.Value());
        FAIL() << "First swapChain writer must not LOAD a new viewport image";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_STREQ(error.what(),
            "First scene viewport output pass cannot LOAD an uninitialized image: first");
    }
}

TEST(SceneViewportOutputValidation, RejectsFinalWriterDiscard)
{
    auto graph = MakeGraph();
    graph["passes"].push_back(MakePass("first", "postProcess", "swapChain"));
    graph["passes"].push_back(MakePass("final", "postProcess", "swapChain", "clear", "dontCare"));
    // 最后一个图 pass 不一定是 swapChain writer，必须找到最后一个显示输出。
    graph["passes"].push_back(MakePass("tail", "postProcess", "sceneColor"));
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    try
    {
        ValidateSceneViewportOutputs(compiled.Value());
        FAIL() << "Final swapChain writer must store the image for UI sampling";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_STREQ(error.what(),
            "Final scene viewport output pass requires storeOp=store: final");
    }
}

TEST(SceneViewportOutputValidation, PermitsIntermediateWriterDiscard)
{
    auto graph = MakeGraph();
    graph["passes"].push_back(MakePass("first", "postProcess", "swapChain"));
    graph["passes"].push_back(MakePass("intermediate", "postProcess", "swapChain", "clear", "dontCare"));
    graph["passes"].push_back(MakePass("final", "postProcess", "swapChain"));
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    EXPECT_TRUE(ValidateSceneViewportOutputs(compiled.Value()));
}

TEST(SceneViewportOutputValidation, ReturnsFalseWithoutSwapchainOutput)
{
    auto graph = MakeGraph();
    graph["passes"][0]["output"][0]["loadOp"] = "load";
    graph["passes"][2]["output"][0]["storeOp"] = "dontCare";
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    EXPECT_FALSE(ValidateSceneViewportOutputs(compiled.Value()));
}

TEST(SceneViewportOutputValidation, AcceptsValidSingleWriterGraph)
{
    auto graph = MakeGraph();
    graph["passes"].push_back(MakePass("present", "postProcess", "swapChain"));
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    EXPECT_TRUE(ValidateSceneViewportOutputs(compiled.Value()));
}

TEST(SceneViewportOutputValidation, PermitsLaterWriterLoad)
{
    auto graph = MakeGraph();
    graph["passes"].push_back(MakePass("first", "postProcess", "swapChain"));
    graph["passes"].push_back(MakePass("final", "postProcess", "swapChain", "load"));
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    EXPECT_TRUE(ValidateSceneViewportOutputs(compiled.Value()));
}

TEST(SceneViewportOutputValidation, IgnoresOtherAttachmentsInSwapchainWriter)
{
    auto graph = MakeGraph();
    auto pass = MakePass("present", "postProcess", "swapChain");
    pass["output"].push_back({
        {"resource", "sceneColor"}, {"loadOp", "load"}, {"storeOp", "dontCare"}
    });
    graph["passes"].push_back(pass);
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    EXPECT_TRUE(ValidateSceneViewportOutputs(compiled.Value()));
}

TEST(SceneViewportOutputValidation, PermitsFirstWriterDontCareLoad)
{
    auto graph = MakeGraph();
    graph["passes"].push_back(MakePass("present", "postProcess", "swapChain", "dontCare"));
    const auto compiled = RenderGraphCompiler().Compile(graph);
    ASSERT_TRUE(compiled.IsSuccess()) << compiled.Error().code << ": " << compiled.Error().message;

    EXPECT_TRUE(ValidateSceneViewportOutputs(compiled.Value()));
}

} // namespace VL
