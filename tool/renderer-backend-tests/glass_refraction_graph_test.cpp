#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
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

nlohmann::json LoadJson(const std::string& relativePath)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    if (!input)
    {
        throw std::runtime_error("Missing fixture: " + relativePath);
    }
    return nlohmann::json::parse(input);
}

std::string LoadText(const std::string& relativePath)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

const VL::CompiledRenderGraphPass& RequireCompiledPass(
    const VL::CompiledRenderGraph& graph,
    const std::string& name)
{
    for (const VL::CompiledRenderGraphPass& pass : graph.passes)
    {
        if (pass.name == name)
        {
            return pass;
        }
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
        {
            return index;
        }
    }
    throw std::runtime_error("Missing pass order entry: " + name);
}

const VL::CompiledRenderGraphPassInputDescriptor* FindInput(
    const VL::CompiledRenderGraphPass& pass,
    const std::string& resource)
{
    for (const auto& input : pass.inputDescriptors)
    {
        if (input.resource == resource)
        {
            return &input;
        }
    }
    return nullptr;
}

const nlohmann::json& RequireParameter(
    const nlohmann::json& material,
    const std::string& name)
{
    return material.at("parameters").at(name);
}
} // namespace

TEST(GlassRefractionGraph, CopiesSceneColorBeforeTransparentPass)
{
    const auto result = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const VL::CompiledRenderGraph& graph = result.Value();

    const VL::CompiledRenderGraphPass& copyPass =
        RequireCompiledPass(graph, "sceneColorCopyPass");
    EXPECT_EQ(copyPass.type, VL::RenderGraphPassType::PostProcess);
    ASSERT_EQ(copyPass.outputResources.size(), 1u);
    EXPECT_EQ(copyPass.outputResources[0].resource, "sceneColorCopy");
    EXPECT_EQ(copyPass.outputResources[0].loadOp, "clear");
    EXPECT_EQ(copyPass.outputResources[0].storeOp, "store");
    const auto* sourceInput = FindInput(copyPass, "sceneColor");
    ASSERT_NE(sourceInput, nullptr);
    EXPECT_EQ(sourceInput->binding, 0u);

    EXPECT_LT(
        RequirePassIndex(graph, "sky"),
        RequirePassIndex(graph, "sceneColorCopyPass"));
    EXPECT_LT(
        RequirePassIndex(graph, "sceneColorCopyPass"),
        RequirePassIndex(graph, "forwardTransparent"));
}

TEST(GlassRefractionGraph, TransparentPassSamplesCopyAndGBufferNormals)
{
    const auto result = VL::RenderGraphCompiler{}.Compile(LoadGraph());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const VL::CompiledRenderGraphPass& transparentPass =
        RequireCompiledPass(result.Value(), "forwardTransparent");

    const auto* copyInput = FindInput(transparentPass, "sceneColorCopy");
    ASSERT_NE(copyInput, nullptr);
    EXPECT_EQ(copyInput->binding, 8u);
    const auto* normalInput = FindInput(transparentPass, "gbufferA");
    ASSERT_NE(normalInput, nullptr);
    EXPECT_EQ(normalInput->binding, 9u);

    // sceneColor is the transparent pass output. Sampling it in the same pass
    // would be a feedback loop, so the copy must be the only color source.
    EXPECT_TRUE(
        std::find(
            transparentPass.inputResources.begin(),
            transparentPass.inputResources.end(),
            "sceneColor") == transparentPass.inputResources.end());
}

TEST(GlassRefractionGraph, CopyResourceIsSampledHalfFloat)
{
    const nlohmann::json graph = LoadGraph();
    for (const nlohmann::json& resource : graph.at("resources"))
    {
        if (resource.at("name") != "sceneColorCopy")
        {
            continue;
        }
        EXPECT_EQ(resource.at("format"), "R16G16B16A16_SFLOAT");
        EXPECT_EQ(resource.at("widthScale"), 1);
        EXPECT_EQ(resource.at("heightScale"), 1);
        std::set<std::string> usage;
        for (const nlohmann::json& entry : resource.at("usage"))
        {
            usage.insert(entry.get<std::string>());
        }
        EXPECT_TRUE(usage.count("colorAttachment") == 1);
        EXPECT_TRUE(usage.count("sampled") == 1);
        return;
    }
    FAIL() << "sceneColorCopy resource is missing";
}

TEST(GlassRefractionGraph, MaterialUsesUnlitTransparentPath)
{
    const nlohmann::json material =
        LoadJson("shader/glsl/M_glassRefraction.json");
    EXPECT_EQ(material.at("shadingModel"), "Unlit");
    EXPECT_EQ(
        material.at("renderStates").at("renderMode"),
        "TransparentAlphaBlend");
    EXPECT_TRUE(
        std::filesystem::is_regular_file(
            std::filesystem::path(VL_TEST_SOURCE_ROOT) /
            "shader/glsl/M_glassRefraction.vertex.glsl"));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(
            std::filesystem::path(VL_TEST_SOURCE_ROOT) /
            "shader/glsl/M_glassRefraction.surface.glsl"));

    EXPECT_DOUBLE_EQ(
        RequireParameter(material, "u_ior").at("range").at("min").get<double>(),
        1.0);
    EXPECT_DOUBLE_EQ(
        RequireParameter(material, "u_ior").at("range").at("max").get<double>(),
        2.5);
    EXPECT_DOUBLE_EQ(
        RequireParameter(material, "u_refractionStrength")
            .at("range").at("min").get<double>(),
        0.0);
    EXPECT_DOUBLE_EQ(
        RequireParameter(material, "u_refractionStrength")
            .at("range").at("max").get<double>(),
        4.0);
    EXPECT_DOUBLE_EQ(
        RequireParameter(material, "u_opacity")
            .at("range").at("max").get<double>(),
        1.0);
    EXPECT_DOUBLE_EQ(
        RequireParameter(material, "u_roughness")
            .at("range").at("max").get<double>(),
        1.0);
}

TEST(GlassRefractionGraph, SurfaceSamplesCopyBindingAndComputesSnellOffset)
{
    const std::string surface =
        LoadText("shader/glsl/M_glassRefraction.surface.glsl");
    EXPECT_NE(
        surface.find("layout(set = 3, binding = 8) uniform sampler2D sceneColorCopy"),
        std::string::npos);
    EXPECT_NE(
        surface.find("layout(set = 3, binding = 9) uniform sampler2D gbufferA"),
        std::string::npos);
    EXPECT_NE(surface.find("refract(incident, worldNormal, eta)"),
        std::string::npos);
    EXPECT_NE(surface.find("ProjectWorldToScreen(exitPosition)"),
        std::string::npos);
    EXPECT_EQ(surface.find("texture(sceneColor,"), std::string::npos);
    EXPECT_EQ(surface.find("sampler2D sceneColor;"), std::string::npos);
}

TEST(GlassRefractionGraph, GalleryGlassInstanceTargetsRefractionMaterial)
{
    const std::filesystem::path resources =
        std::filesystem::path(VL_TEST_SOURCE_ROOT).parent_path() /
        "VukanLearnResources";
    const std::filesystem::path instancePath =
        resources /
        "Maps/SC_particle_ue_impact_variants/Materials/"
        "MI_ue_gallery_wall_glass.json";
    if (!std::filesystem::is_regular_file(instancePath))
    {
        GTEST_SKIP() << "Resource worktree is not attached";
    }

    std::ifstream input(instancePath);
    const nlohmann::json instance = nlohmann::json::parse(input);
    EXPECT_EQ(instance.at("material"), "shader/glsl/M_glassRefraction.json");
    EXPECT_TRUE(instance.contains("parameters"));
    EXPECT_TRUE(instance.at("parameters").contains("u_refractionStrength"));
    EXPECT_TRUE(instance.at("parameters").contains("u_ior"));
    EXPECT_TRUE(instance.at("parameters").contains("u_opacity"));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(
            resources / "Common/Materials/Pass/MI_sceneColorCopy.json"));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(
            resources / "Common/Materials/MI_glass_refraction.json"));
}
