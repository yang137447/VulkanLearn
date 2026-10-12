#include <gtest/gtest.h>
#include <fstream>
#include "render/camera/autoExposureContract.h"
#include "render/rendergraph/renderGraphCompiler.h"

namespace
{
nlohmann::json LoadGraph()
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) + "/config/renderGraphConfig.json");
    return nlohmann::json::parse(input);
}
nlohmann::json& ExposurePass(nlohmann::json& graph)
{
    for (auto& pass : graph.at("passes"))
        if (pass.at("type") == "autoExposure") return pass;
    throw std::runtime_error("Missing AutoExposure fixture pass");
}
nlohmann::json& ExposureResource(nlohmann::json& graph)
{
    const std::string name = ExposurePass(graph).at("output").at(0).at("resource");
    for (auto& resource : graph.at("resources"))
        if (resource.at("name") == name) return resource;
    throw std::runtime_error("Missing AutoExposure fixture output");
}
}

TEST(AutoExposureContract, HistogramRangeHas64Bins)
{
    EXPECT_EQ(VL::AutoExposureBinCount, 64u);
    EXPECT_FLOAT_EQ(VL::AutoExposureMinLogLuminance, -12.0f);
    EXPECT_FLOAT_EQ(VL::AutoExposureMaxLogLuminance, 16.0f);
}

TEST(AutoExposureContract, DispatchCoversIncompleteEdgeGroups)
{
    EXPECT_EQ(VL::AutoExposureDispatchGroups(1), 1u);
    EXPECT_EQ(VL::AutoExposureDispatchGroups(16), 1u);
    EXPECT_EQ(VL::AutoExposureDispatchGroups(17), 2u);
    EXPECT_EQ(VL::AutoExposureDispatchGroups(1920), 120u);
    EXPECT_EQ(VL::AutoExposureDispatchGroups(1080), 68u);
}

TEST(AutoExposureContract, FrozenParametersRetainFullAuthoredRates)
{
    VL::CameraExposure camera;
    camera.mode = VL::ExposureMode::Auto;
    camera.autoMinEv100 = -1;
    camera.autoMaxEv100 = 16;
    camera.compensation = 2;
    camera.autoSpeedUp = 3;
    camera.autoSpeedDown = 1;
    const auto params = VL::AutoExposureHistorySequence{}.Prepare(camera, 0.016f);
    EXPECT_FLOAT_EQ(params.range[1], static_cast<float>(std::exp2(-16.0) / 1.2));
    EXPECT_FLOAT_EQ(params.range[2], static_cast<float>(std::exp2(1.0) / 1.2));
    EXPECT_FLOAT_EQ(params.range[3], 2);
    EXPECT_FLOAT_EQ(params.adaptation[0], 3);
    EXPECT_FLOAT_EQ(params.adaptation[1], 1);
    EXPECT_FLOAT_EQ(params.adaptation[2], 0.016f);
}

TEST(AutoExposureContract, FirstFrameCannotReadUninitializedHistory)
{
    VL::CameraExposure camera;
    camera.mode = VL::ExposureMode::Auto;
    const auto params = VL::AutoExposureHistorySequence{}.Prepare(camera, 0.016f);
    EXPECT_EQ(params.control[0], 1u);
    EXPECT_EQ(params.control[1], 0u);
    EXPECT_EQ(params.control[2], 1u);
    EXPECT_EQ(params.control[3], 0u);
}

TEST(AutoExposureContract, HistoryAdvancesOnlyOnSubmittedSequence)
{
    VL::CameraExposure camera;
    camera.mode = VL::ExposureMode::Auto;
    VL::AutoExposureHistorySequence history;
    const auto first = history.Prepare(camera, 0.016f);
    EXPECT_EQ(history.Prepare(camera, 0.016f).control, first.control);
    history.Advance(camera.mode);
    const auto second = history.Prepare(camera, 0.016f);
    EXPECT_EQ(second.control[1], 1u);
    EXPECT_EQ(second.control[2], first.control[3]);
    EXPECT_EQ(second.control[3], first.control[2]);
    history.Advance(camera.mode);
    const auto third = history.Prepare(camera, 0.016f);
    EXPECT_EQ(third.control[2], second.control[3]);
    EXPECT_EQ(third.control[3], first.control[3]);
}

TEST(AutoExposureContract, ManualAndPhysicalInvalidateAutomaticHistory)
{
    for (const auto mode : {VL::ExposureMode::Ev100, VL::ExposureMode::PhysicalCamera})
    {
        VL::CameraExposure camera;
        camera.mode = VL::ExposureMode::Auto;
        VL::AutoExposureHistorySequence history;
        history.Advance(camera.mode);
        camera.mode = mode;
        const auto manual = history.Prepare(camera, 0.016f);
        EXPECT_EQ(manual.control[0], 0u);
        EXPECT_EQ(manual.control[1], 0u);
        EXPECT_FLOAT_EQ(manual.manual[0], VL::ExposureScale(camera));
        history.Advance(camera.mode);
        camera.mode = VL::ExposureMode::Auto;
        EXPECT_EQ(history.Prepare(camera, 0.016f).control[1], 0u);
    }
}

TEST(AutoExposureGraph, SingleComputePassAndSharedOutputAreCompiled)
{
    const auto result = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess());
    size_t updates = 0;
    std::string output;
    for (const auto& pass : result.Value().passes)
    {
        if (pass.type != VL::RenderGraphPassType::AutoExposure) continue;
        ++updates;
        output = pass.outputResources.at(0).resource;
        EXPECT_FALSE(pass.needCreateMaterial);
        ASSERT_EQ(pass.barriersBeforePass.size(), 2u);
        EXPECT_EQ(pass.barriersBeforePass[0].type, VL::CompiledRenderGraphBarrierType::AttachmentToShaderRead);
        EXPECT_EQ(pass.barriersBeforePass[1].type, VL::CompiledRenderGraphBarrierType::ShaderReadToStorage);
    }
    EXPECT_EQ(updates, 1u);
    for (const auto& pass : result.Value().passes)
    {
        if (pass.name != "bloomPrefilter" && pass.name != "toneMapping") continue;
        bool samplesExposure = false;
        for (const auto& descriptor : pass.inputDescriptors)
            samplesExposure = samplesExposure || descriptor.resource == output;
        EXPECT_TRUE(samplesExposure) << pass.name;
        if (pass.name == "bloomPrefilter")
        {
            ASSERT_EQ(pass.barriersBeforePass.size(), 1u);
            EXPECT_EQ(pass.barriersBeforePass[0].type, VL::CompiledRenderGraphBarrierType::StorageToShaderRead);
        }
    }
}

TEST(AutoExposureGraph, RejectsGraphicsLoadSemantics)
{
    auto graph = LoadGraph();
    ExposurePass(graph)["output"][0]["loadOp"] = "load";
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsDiscardingExposureResult)
{
    auto graph = LoadGraph();
    ExposurePass(graph)["output"][0]["storeOp"] = "dontCare";
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsSecondAdaptationInSameFrame)
{
    auto graph = LoadGraph();
    auto duplicate = ExposurePass(graph);
    duplicate["name"] = "secondExposure";
    graph["passes"].push_back(duplicate);
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsHalfPrecisionOutput)
{
    auto graph = LoadGraph();
    ExposureResource(graph)["format"] = "R16G16B16A16_SFLOAT";
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsUninitializedOutputLayout)
{
    auto graph = LoadGraph();
    ExposureResource(graph).erase("initialLayout");
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsMultisizedOutput)
{
    auto graph = LoadGraph();
    ExposureResource(graph)["widthSize"] = 2;
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsUnsampledOutput)
{
    auto graph = LoadGraph();
    ExposureResource(graph)["usage"] = {"storage"};
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}

TEST(AutoExposureGraph, RejectsMeteringBeforeSceneColorProducer)
{
    auto graph = LoadGraph();
    auto exposure = ExposurePass(graph);
    for (auto iterator = graph["passes"].begin(); iterator != graph["passes"].end(); ++iterator)
    {
        if ((*iterator)["type"] != "autoExposure") continue;
        graph["passes"].erase(iterator);
        break;
    }
    graph["passes"].insert(graph["passes"].begin(), exposure);
    EXPECT_TRUE(VL::RenderGraphCompiler{}.Compile(graph).IsFailure());
}
