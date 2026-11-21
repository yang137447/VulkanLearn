#include "material/loader/materialInstanceResolver.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "material/materialPipelineLayout.h"
#include "materialInstanceValidator.h"
#include "render/resource/terrainCompositedQualityReplay.h"
#include "renderGraph.h"
#include "shaderCompiler.h"

#include <gtest/gtest.h>
#include <fstream>

namespace
{
using Json = nlohmann::json;
const std::filesystem::path Root = VL_QUALITY_SOURCE_ROOT;
const std::filesystem::path Output = VL_QUALITY_TEST_OUTPUT;

Json ReadJson(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Quality material fixture file is unavailable");
    return Json::parse(input);
}

class QualityMaterialVariant : public testing::Test
{
protected:
    inline static ShaderVariantKey regularVariant;
    inline static GraphicsShaderVariantArtifact normal;
    inline static GraphicsShaderVariantArtifact capture;
    inline static std::vector<ShaderBinding> regularLayout;

    static void SetUpTestSuite()
    {
        const std::string materialPath = "shader/glsl/pass/M_terrainFeedback.json";
        const auto definition = ReadJson(Root / materialPath);
        const auto generated = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(
            Root / materialPath);
        ShaderCompiler compiler;
        compiler.Initialize(Root / "shader");
        Json receipt;
        for (int enabled : {0, 1})
        {
            Json instance{
                {"type", "materialInstance"}, {"name", "quality-variant-cpu-fixture"},
                {"material", materialPath}
            };
            if (enabled)
                instance["macros"] = {{"VL_TERRAIN_QUALITY_CAPTURE", 1}};
            const auto resolved = MaterialInstanceResolver::ResolveWithDefinition(
                "fixture/MI_quality.json", instance, definition);
            const auto plan = MaterialInstanceValidator::BuildLoadPlan(
                "fixture/MI_quality.json", {}, resolved.effectiveMaterialInstanceJson,
                resolved.materialPath, resolved.materialJson, false, false);
            ASSERT_FALSE(plan.baseShaderCompileRequest);
            ASSERT_EQ(plan.shaderVariantKey.shaderName, "pass/terrainFeedback");
            ASSERT_TRUE(HasMaterialMacroValue(plan.shaderVariantKey.macros,
                "VL_TERRAIN_QUALITY_CAPTURE", enabled ? "1" : "0"));
            auto request = compiler.CreateGraphicsVariantBuildRequest(plan.shaderVariantKey);
            const auto relative = std::filesystem::relative(
                generated.outputPath, Root / "shader/glsl").generic_string();
            request.sourceSnapshot[relative] = generated.generatedBytes;
            const auto built = compiler.CompileCandidate(request);
            plan.materialDescriptorSchema.ValidateShaderBindings(built.shaderBindings, request.normalizedKey);
            ASSERT_FALSE(built.committed);
            ASSERT_GT(built.shadercInvocations, 0u);
            GraphicsShaderVariantArtifact artifact;
            artifact.shaderBindings = built.shaderBindings;
            artifact.normalizedKey = built.normalizedKey;
            artifact.sourceFingerprint = built.sourceFingerprint;
            artifact.fragmentSpirv = built.outputs.at("runtimeFragment").spirv;
            artifact.vertexSpirv = built.outputs.at("runtimeVertex").spirv;
            if (!enabled)
            {
                regularVariant = plan.shaderVariantKey;
                normal = artifact;
                Renderpass pass{};
                pass.name = "terrainFeedback";
                const auto graph = ReadJson(Root / "config/renderGraphConfig.json");
                for (const auto& row : graph.at("passes"))
                    if (row.at("name") == "terrainFeedback")
                        for (const auto& source : row.at("input"))
                        {
                            VL::CompiledRenderGraphPassInputDescriptor input;
                            input.binding = source.at("binding");
                            input.resource = source.at("resource");
                            input.source = source.at("source");
                            pass.inputDescriptorPlan.push_back(input);
                        }
                ASSERT_EQ(pass.inputDescriptorPlan.size(), 5u);
                const auto layout = VL::BuildMaterialSurfacePipelineLayout(
                    pass, plan.materialDescriptorSchema, normal.shaderBindings);
                for (const auto& set : layout.setBindings)
                    regularLayout.insert(regularLayout.end(), set.begin(), set.end());
            }
            else
                capture = artifact;
            Json bindings = Json::array();
            for (const auto& binding : artifact.shaderBindings)
                bindings.push_back({{"set", binding.set}, {"binding", binding.binding},
                    {"type", uint32_t(binding.type)}, {"count", binding.descriptorCount},
                    {"stages", uint32_t(binding.stageFlags)}});
            const auto prefix = "production-macro-" + std::to_string(enabled);
            std::ofstream spv(Output / (prefix + ".frag.spv"), std::ios::binary);
            spv.write(reinterpret_cast<const char*>(artifact.fragmentSpirv.data()),
                artifact.fragmentSpirv.size() * sizeof(uint32_t));
            receipt[prefix] = {{"macros", request.macros}, {"normalizedKey", built.normalizedKey},
                {"sourceFingerprint", built.sourceFingerprint}, {"shadercInvocations", built.shadercInvocations},
                {"committed", built.committed}, {"bindings", bindings}};
        }
        std::ofstream report(Output / "production-material-variants.json");
        report << receipt.dump(2) << '\n';
    }
};

TEST_F(QualityMaterialVariant, MacroIsARealCompilerVariant)
{
    EXPECT_NE(normal.normalizedKey, capture.normalizedKey);
    EXPECT_NE(normal.sourceFingerprint, capture.sourceFingerprint);
    EXPECT_NE(normal.fragmentSpirv, capture.fragmentSpirv);
    EXPECT_EQ(VL::TerrainQualityCaptureVariant(regularVariant).GetNormalizedKey(),
        [&] { auto variant = regularVariant; variant.macros = {"VL_TERRAIN_QUALITY_CAPTURE=1"};
              return variant.GetNormalizedKey(); }());
}

TEST_F(QualityMaterialVariant, RegularPipelineHasNoCaptureBindings)
{
    for (const auto& binding : normal.shaderBindings)
        EXPECT_FALSE(binding.set == 3 && binding.binding >= 5);
    for (const auto& binding : regularLayout)
        EXPECT_FALSE(binding.set == 3 && binding.binding >= 5);
}

TEST_F(QualityMaterialVariant, ReplayOverridesOnlyItsOwnPassSetExtension)
{
    const auto layout = VL::TerrainQualityReplayLayout(regularLayout, capture);
    ASSERT_EQ(layout.setBindings[3].size(), 7u);
    EXPECT_EQ(layout.setBindings[3][5].type, vk::DescriptorType::eStorageBuffer);
    EXPECT_EQ(layout.setBindings[3][6].type, vk::DescriptorType::eCombinedImageSampler);
    EXPECT_EQ(layout.setBindings[3][5].descriptorCount, 1u);
    for (uint32_t set = 0; set < 3; ++set)
        for (const auto& binding : layout.setBindings[set])
        {
            const auto found = std::find_if(regularLayout.begin(), regularLayout.end(),
                [&](const ShaderBinding& row) { return row.set == set && row.binding == binding.binding; });
            ASSERT_NE(found, regularLayout.end());
            EXPECT_TRUE(HasSameShaderBindingLayout(*found, binding));
            EXPECT_EQ(found->stageFlags, binding.stageFlags);
        }
}

TEST_F(QualityMaterialVariant, NonCaptureArtifactCannotBuildReplayLayout)
{
    EXPECT_THROW(VL::TerrainQualityReplayLayout(regularLayout, normal), std::runtime_error);
}

TEST_F(QualityMaterialVariant, WrongDescriptorTypeOrCountCannotBuildReplay)
{
    for (bool wrongCount : {false, true})
    {
        auto broken = capture;
        for (auto& binding : broken.shaderBindings)
            if (binding.set == 3 && binding.binding == 5)
            {
                if (wrongCount)
                    binding.descriptorCount = 2;
                else
                    binding.type = vk::DescriptorType::eUniformBuffer;
            }
        EXPECT_THROW(VL::TerrainQualityReplayLayout(regularLayout, broken), std::runtime_error);
    }
}
}
