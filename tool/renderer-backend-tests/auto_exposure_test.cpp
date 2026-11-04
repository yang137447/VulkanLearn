#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
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
std::string LoadShaderSource(const std::string& relativePath)
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
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
            ASSERT_EQ(pass.barriersBeforePass.size(), 2u);
            EXPECT_EQ(pass.barriersBeforePass[0].type, VL::CompiledRenderGraphBarrierType::StorageToShaderRead);
            EXPECT_EQ(pass.barriersBeforePass[1].type, VL::CompiledRenderGraphBarrierType::AttachmentToShaderRead);
        }
    }
}

TEST(ParticleDistortionGraph, BloomPrefilterAndToneMappingShareDistortionInput)
{
    const auto result = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess());

    const VL::CompiledRenderGraphPass* distortion = nullptr;
    const VL::CompiledRenderGraphPass* bloom = nullptr;
    const VL::CompiledRenderGraphPass* toneMapping = nullptr;
    const VL::CompiledRenderGraphPass* autoExposure = nullptr;
    for (const auto& pass : result.Value().passes)
    {
        if (pass.name == "particleDistortion") distortion = &pass;
        if (pass.name == "bloomPrefilter") bloom = &pass;
        if (pass.name == "toneMapping") toneMapping = &pass;
        if (pass.name == "autoExposure") autoExposure = &pass;
    }
    ASSERT_NE(distortion, nullptr);
    ASSERT_NE(bloom, nullptr);
    ASSERT_NE(toneMapping, nullptr);
    ASSERT_NE(autoExposure, nullptr);
    EXPECT_EQ(distortion->type, VL::RenderGraphPassType::Distortion);

    const auto bloomBinding = std::find_if(
        bloom->inputDescriptors.begin(),
        bloom->inputDescriptors.end(),
        [](const VL::CompiledRenderGraphPassInputDescriptor& descriptor)
        {
            return descriptor.resource == "distortion";
        });
    ASSERT_NE(bloomBinding, bloom->inputDescriptors.end());
    EXPECT_EQ(bloomBinding->binding, 2u);

    // toneMapping 的既有绑定保持不变；Bloom 与最终合成共用同一扭曲资源。
    const auto toneMappingBinding = std::find_if(
        toneMapping->inputDescriptors.begin(),
        toneMapping->inputDescriptors.end(),
        [](const VL::CompiledRenderGraphPassInputDescriptor& descriptor)
        {
            return descriptor.resource == "distortion";
        });
    ASSERT_NE(toneMappingBinding, toneMapping->inputDescriptors.end());
    EXPECT_EQ(toneMappingBinding->binding, 5u);

    // autoExposure 只能读取未扭曲 sceneColor，扭曲资源不得进入测光输入。
    ASSERT_EQ(autoExposure->inputDescriptors.size(), 1u);
    EXPECT_EQ(autoExposure->inputDescriptors[0].resource, "sceneColor");
    EXPECT_EQ(autoExposure->inputDescriptors[0].binding, 0u);
}

TEST(ParticleDistortionShader, BloomAndToneMappingUseSameAspectCorrectedUv)
{
    const std::string bloom = LoadShaderSource("shader/glsl/pass/bloomPrefilter.frag");
    const std::string toneMapping = LoadShaderSource("shader/glsl/pass/toneMapping.frag");
    const std::string aspect = "float aspect = sceneSize.x / max(sceneSize.y, 1.0);";
    const std::string mapping =
        "vec2(distortionOffset.x / aspect, distortionOffset.y)";
    ASSERT_FALSE(bloom.empty());
    ASSERT_FALSE(toneMapping.empty());
    EXPECT_NE(bloom.find(aspect), std::string::npos);
    EXPECT_NE(toneMapping.find(aspect), std::string::npos);
    EXPECT_NE(bloom.find(mapping), std::string::npos);
    EXPECT_NE(toneMapping.find(mapping), std::string::npos);
    EXPECT_NE(bloom.find("texture(inputColor, sceneUV + texel"), std::string::npos);
    EXPECT_NE(toneMapping.find("texture(sceneColor, sceneUV)"), std::string::npos);
}

TEST(ForwardDecalReceiverGraph, OnlyForwardOpaqueBindsSurfaceDBufferInputs)
{
    const auto result = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess());
    for (const auto& pass : result.Value().passes)
    {
        if (pass.name != "forwardOpaque")
        {
            continue;
        }
        const std::array<std::string, 5> expected{
            "dbufferA", "dbufferB", "dbufferC",
            "dbufferD", "dbufferMask"};
        for (size_t index = 0; index < expected.size(); ++index)
        {
            const auto found = std::find_if(
                pass.inputDescriptors.begin(),
                pass.inputDescriptors.end(),
                [&expected, index](
                    const VL::CompiledRenderGraphPassInputDescriptor&
                        descriptor)
                {
                    return descriptor.resource == expected[index];
                });
            ASSERT_NE(found, pass.inputDescriptors.end()) << pass.name;
            EXPECT_EQ(found->binding, index + 1u) << pass.name;
        }
    }
    for (const auto& pass : result.Value().passes)
    {
        if (pass.name != "forwardTransparent") continue;
        for (const auto& descriptor : pass.inputDescriptors)
        {
            // 透明 pass 不读 A..D/Mask；只允许纯附加的 Emissive。
            if (descriptor.resource.rfind("dbuffer", 0) == 0)
                EXPECT_EQ(descriptor.resource, "dbufferEmissive");
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
