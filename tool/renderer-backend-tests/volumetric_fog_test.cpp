#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "render/rendergraph/renderGraphCompiler.h"
#include "render/volumetricFog/volumetricFogSettings.h"

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
        if (pass.at("name") == name) return pass;
    }
    throw std::runtime_error("Missing graph pass: " + name);
}

const VL::CompiledRenderGraphPass& RequireCompiledPass(
    const VL::CompiledRenderGraph& graph,
    const std::string& name)
{
    for (const VL::CompiledRenderGraphPass& pass : graph.passes)
    {
        if (pass.name == name) return pass;
    }
    throw std::runtime_error("Missing compiled pass: " + name);
}

size_t RequirePassIndex(
    const VL::CompiledRenderGraph& graph,
    const std::string& name)
{
    for (size_t index = 0; index < graph.passOrder.size(); ++index)
    {
        if (graph.passOrder[index] == name) return index;
    }
    throw std::runtime_error("Missing compiled pass order entry: " + name);
}

bool HasBarrier(
    const VL::CompiledRenderGraphPass& pass,
    const std::string& resource,
    VL::CompiledRenderGraphBarrierType type)
{
    for (const VL::CompiledRenderGraphBarrier& barrier :
         pass.barriersBeforePass)
    {
        if (barrier.resource == resource && barrier.type == type)
            return true;
    }
    return false;
}

std::string ReadSource(const std::string& relativePath)
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    if (!input)
    {
        throw std::runtime_error("Cannot read " + relativePath);
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}
} // namespace

TEST(VolumetricFogSettings, DefaultsAreDisabled)
{
    const VL::FogSettings fog;
    EXPECT_FALSE(fog.enabled);
    EXPECT_FALSE(VL::IsFogEnabled(fog));
    EXPECT_FLOAT_EQ(0.0f, fog.density);
}

TEST(VolumetricFogSettings, SliceDepthIsExponentialAndInvertible)
{
    constexpr float nearCm = 1.0f;
    constexpr float farCm = 8000.0f;
    constexpr uint32_t layerCount = 32;
    EXPECT_NEAR(VL::FogSliceViewDepthCm(nearCm, farCm, 0, layerCount), nearCm, 1.0e-3);
    EXPECT_NEAR(VL::FogSliceViewDepthCm(nearCm, farCm, layerCount - 1, layerCount), farCm, 1.0f);
    float previous = 0.0f;
    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        const float depth = VL::FogSliceViewDepthCm(
            nearCm, farCm, layer, layerCount);
        EXPECT_GT(depth, previous);
        previous = depth;
        const float coordinate = VL::FogSliceCoordinate(
            depth, nearCm, farCm, layerCount);
        EXPECT_NEAR(coordinate, static_cast<float>(layer), 1.0e-3);
    }
}

TEST(VolumetricFogSettings, SliceCoordinateClampsBeyondClipPlanes)
{
    EXPECT_FLOAT_EQ(
        0.0f,
        VL::FogSliceCoordinate(-100.0f, 1.0f, 100.0f, 32));
    EXPECT_FLOAT_EQ(
        31.0f,
        VL::FogSliceCoordinate(10000.0f, 1.0f, 100.0f, 32));
}

TEST(VolumetricFogSettings, HeightDensityFallsOffAboveBaseHeight)
{
    EXPECT_FLOAT_EQ(1.0f, VL::FogHeightDensityScale(100.0f, 100.0f, 0.1f));
    const float above = VL::FogHeightDensityScale(1100.0f, 100.0f, 0.1f);
    // 10 m above base、0.1/m 衰减 => exp(-1) ≈ 0.3679。
    EXPECT_NEAR(above, 0.3678794f, 1.0e-4f);
    // 低于 baseHeight 时上限 8，避免密度爆炸。
    EXPECT_FLOAT_EQ(8.0f, VL::FogHeightDensityScale(-10000.0f, 0.0f, 1.0f));
}

TEST(VolumetricFogSettings, HenyeyGreensteinIsNormalizedAndDirectional)
{
    EXPECT_NEAR(VL::HenyeyGreensteinPhase(0.3f, 0.0f), 1.0f, 1.0e-6f);
    EXPECT_NEAR(VL::HenyeyGreensteinPhase(-0.5f, 0.0f), 1.0f, 1.0e-6f);
    EXPECT_GT(
        VL::HenyeyGreensteinPhase(1.0f, 0.6f),
        VL::HenyeyGreensteinPhase(-1.0f, 0.6f));
    EXPECT_NEAR(
        VL::HenyeyGreensteinPhase(0.25f, 0.4f),
        VL::HenyeyGreensteinPhase(0.25f, 0.4f),
        0.0f);
}

TEST(VolumetricFogSettings, ParsesAuthoredValues)
{
    const nlohmann::json node = {
        {"enabled", true},
        {"density", 0.02},
        {"heightFalloff", 0.15},
        {"baseHeight", 100.0},
        {"albedo", {0.6, 0.7, 0.9}},
        {"anisotropy", 0.55},
        {"intensity", 2.0}};
    const VL::FogSettings fog = VL::ParseFogSettingsJson(node);
    EXPECT_TRUE(fog.enabled);
    EXPECT_FLOAT_EQ(0.02f, fog.density);
    EXPECT_FLOAT_EQ(0.15f, fog.heightFalloff);
    EXPECT_FLOAT_EQ(100.0f, fog.baseHeight);
    EXPECT_FLOAT_EQ(0.6f, fog.albedo.x());
    EXPECT_FLOAT_EQ(0.9f, fog.albedo.z());
    EXPECT_FLOAT_EQ(0.55f, fog.anisotropy);
    EXPECT_FLOAT_EQ(2.0f, fog.intensity);
    EXPECT_TRUE(VL::IsFogEnabled(fog));
}

TEST(VolumetricFogSettings, RejectsInvalidAuthoring)
{
    EXPECT_THROW(
        VL::ParseFogSettingsJson(nlohmann::json{{"density", -1.0}}),
        std::invalid_argument);
    EXPECT_THROW(
        VL::ParseFogSettingsJson(nlohmann::json{{"anisotropy", 1.5}}),
        std::invalid_argument);
    EXPECT_THROW(
        VL::ParseFogSettingsJson(nlohmann::json{{"albedo", {1.0, 2.0}}}),
        std::invalid_argument);
    EXPECT_THROW(
        VL::ParseFogSettingsJson(nlohmann::json{{"enabled", 1}}),
        std::invalid_argument);
    EXPECT_THROW(
        VL::ParseFogSettingsJson(nlohmann::json::array()),
        std::invalid_argument);
}

TEST(VolumetricFogGraph, ShipsStorageArrayPassBeforeComposite)
{
    const nlohmann::json source = LoadGraph();
    const auto compileResult = VL::RenderGraphCompiler{}.Compile(source);
    ASSERT_TRUE(compileResult.IsSuccess());
    const VL::CompiledRenderGraph& graph = compileResult.Value();

    const VL::CompiledRenderGraphPass& fogPass =
        RequireCompiledPass(graph, "volumetricFog");
    EXPECT_EQ(fogPass.type, VL::RenderGraphPassType::VolumetricFog);
    ASSERT_EQ(fogPass.outputResources.size(), 1u);
    EXPECT_EQ(fogPass.outputResources[0].resource, "volumetricFog");
    EXPECT_TRUE(HasBarrier(
        fogPass,
        "volumetricFog",
        VL::CompiledRenderGraphBarrierType::ShaderReadToStorage));

    const VL::CompiledRenderGraphPass& compositePass =
        RequireCompiledPass(graph, "fogComposite");
    EXPECT_EQ(compositePass.type, VL::RenderGraphPassType::FogComposite);
    EXPECT_TRUE(HasBarrier(
        compositePass,
        "volumetricFog",
        VL::CompiledRenderGraphBarrierType::StorageToShaderRead));
    ASSERT_EQ(compositePass.outputResources.size(), 1u);
    EXPECT_EQ(compositePass.outputResources[0].resource, "sceneColor");
    EXPECT_EQ(compositePass.outputResources[0].loadOp, "load");

    const size_t fogIndex = RequirePassIndex(graph, "volumetricFog");
    const size_t compositeIndex = RequirePassIndex(graph, "fogComposite");
    const size_t bloomIndex = RequirePassIndex(graph, "bloomPrefilter");
    const size_t particleIndex = RequirePassIndex(graph, "particle");
    EXPECT_LT(particleIndex, fogIndex);
    EXPECT_LT(fogIndex, compositeIndex);
    EXPECT_LT(compositeIndex, bloomIndex);
}

TEST(VolumetricFogGraph, RejectsIncompleteOrMisorderedFogPasses)
{
    nlohmann::json withoutComposite = LoadGraph();
    auto& passes = withoutComposite.at("passes");
    passes.erase(
        std::remove_if(
            passes.begin(), passes.end(),
            [](const nlohmann::json& pass)
            {
                return pass.value("type", std::string()) == "fogComposite";
            }),
        passes.end());
    EXPECT_FALSE(VL::RenderGraphCompiler{}.Compile(withoutComposite).IsSuccess());

    nlohmann::json swapped = LoadGraph();
    nlohmann::json& swappedPasses = swapped.at("passes");
    const auto fogIt = std::find_if(
        swappedPasses.begin(), swappedPasses.end(),
        [](const nlohmann::json& pass)
        {
            return pass.value("name", std::string()) == "volumetricFog";
        });
    const auto compositeIt = std::find_if(
        swappedPasses.begin(), swappedPasses.end(),
        [](const nlohmann::json& pass)
        {
            return pass.value("name", std::string()) == "fogComposite";
        });
    ASSERT_NE(fogIt, swappedPasses.end());
    ASSERT_NE(compositeIt, swappedPasses.end());
    std::iter_swap(fogIt, compositeIt);
    EXPECT_FALSE(VL::RenderGraphCompiler{}.Compile(swapped).IsSuccess());
}

TEST(VolumetricFogShaderContract, ComputeUsesLayeredStorageAndRadiusWindow)
{
    const std::string fogSource =
        ReadSource("shader/glsl/pass/volumetricFog.comp");
    EXPECT_NE(
        fogSource.find("uniform image2DArray fogImage"),
        std::string::npos);
    EXPECT_NE(
        fogSource.find("VL_FOG_MAX_LAYERS = 32"),
        std::string::npos);
    // 与 common/lighting.glsl 同一条半径窗公式。
    EXPECT_NE(
        fogSource.find("1.0 - normalizedDistanceSq * normalizedDistanceSq"),
        std::string::npos);
    // 方向光必须乘 CSM、点光/聚光必须乘局部阴影。
    EXPECT_NE(
        fogSource.find("CalculateCsmShadow"),
        std::string::npos);
    EXPECT_NE(
        fogSource.find("SampleLocalLightShadow"),
        std::string::npos);
    // 预乘输出：rgb=in-scatter，a=1-T。
    EXPECT_NE(
        fogSource.find("vec4(accumInScatter, 1.0 - accumTransmittance)"),
        std::string::npos);
}

TEST(VolumetricFogShaderContract, CompositeBlendsPremultipliedResult)
{
    const std::string source =
        ReadSource("shader/glsl/pass/fogComposite.frag");
    EXPECT_NE(source.find("uniform sampler2DArray fogVolume"), std::string::npos);
    EXPECT_NE(source.find("mix(fog0, fog1"), std::string::npos);
    EXPECT_NE(source.find("outColor = vec4("), std::string::npos);
}
