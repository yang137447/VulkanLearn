#include "materialInstanceValidator.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/materialDescriptorSchema.h"

#include <gtest/gtest.h>
#include <fstream>

namespace
{
class TressfxMaterial : public ::testing::Test
{
protected:
    void SetUp() override
    {
        LoadMaterial("M_strandProbe");
    }
    void LoadMaterial(const std::string& name)
    {
        materialPath = "shader/glsl/hair/" + name + ".json";
        std::ifstream stream(std::string(VL_SOURCE_ROOT) + "/" + materialPath);
        if (!stream)
            throw std::runtime_error("Missing strand test material: " + materialPath);
        stream >> definition;
        const auto resolved = MaterialInstanceResolver::ResolveWithDefinition("MI_probe.json",
            {{"type", "materialInstance"}, {"material", materialPath}},
            definition);
        effective = resolved.effectiveMaterialInstanceJson;
    }
    MaterialInstanceBuildPlan Plan()
    {
        return MaterialInstanceValidator::BuildLoadPlan("MI_probe.json", {}, effective,
            materialPath, definition, false, false);
    }
    std::string materialPath;
    nlohmann::json definition;
    nlohmann::json effective;
};

TEST_F(TressfxMaterial, UsesDistinctVertexFactoryAndDepthWritingForwardContract)
{
    const auto plan = Plan();
    ASSERT_TRUE(plan.baseShaderCompileRequest);
    EXPECT_EQ(plan.baseShaderCompileRequest->vertexFactoryKey, "TressFXStrands");
    EXPECT_TRUE(plan.surfacePassPipelineContractKey.depthWriteEnable);
}

TEST_F(TressfxMaterial, RejectsDeferredRouting)
{
    effective["renderStates"]["renderMode"] = "Opaque";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, RejectsCoverageWithoutDepthWrites)
{
    effective["renderStates"]["renderMode"] = "TransparentAlphaBlend";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, AllowsCoverageEndpointsAndHardEdges)
{
    const auto schema = VL::MaterialDescriptorSchema::Build(definition, "M_strandProbe.json");
    effective["parameters"]["u_edgeFeather"] = 0.0;
    effective["parameters"]["u_tintColor"][3] = 0.0;
    EXPECT_NO_THROW(schema.ValidateInstanceValues(effective["parameters"], {}, {}, "MI_probe.json"));
    effective["parameters"]["u_edgeFeather"] = 0.5;
    effective["parameters"]["u_tintColor"][3] = 1.0;
    EXPECT_NO_THROW(schema.ValidateInstanceValues(effective["parameters"], {}, {}, "MI_probe.json"));
}

TEST_F(TressfxMaterial, RejectsCoverageProbabilityAboveOne)
{
    const auto schema = VL::MaterialDescriptorSchema::Build(definition, "M_strandProbe.json");
    effective["parameters"]["u_tintColor"][3] = 1.01;
    EXPECT_THROW(schema.ValidateInstanceValues(effective["parameters"], {}, {}, "MI_probe.json"),
        std::runtime_error);
}

TEST_F(TressfxMaterial, RejectsHairIdWithoutHairEvaluator)
{
    effective["shadingModel"] = "Hair";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, RejectsCullingCameraFacingRibbon)
{
    effective["renderStates"]["cullMode"] = "Back";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, RejectsCombinedSkinningFactory)
{
    effective["macros"]["VL_SKINNED_MESH"] = 1;
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, RejectsMaterialVertexDisplacement)
{
    definition["features"]["modifiesMeshPosition"] = true;
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, RejectsStandaloneShaderWithoutFactoryComposition)
{
    definition.erase("shaderEvaluation");
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, DefaultLitUsesStrandFactoryAndDepthWritingForwardContract)
{
    LoadMaterial("M_strandPbr");
    const auto plan = Plan();
    ASSERT_TRUE(plan.baseShaderCompileRequest);
    EXPECT_EQ(effective.at("shadingModel"), "DefaultLit");
    EXPECT_EQ(plan.baseShaderCompileRequest->vertexFactoryKey, "TressFXStrands");
    EXPECT_EQ(plan.shaderVariantKey.renderMode, RenderMode::TransparentAlphaBlendWriteDepth);
    EXPECT_TRUE(plan.surfacePassPipelineContractKey.depthWriteEnable);
    EXPECT_EQ(plan.cullMode, vk::CullModeFlagBits::eNone);
}

TEST_F(TressfxMaterial, DefaultLitRejectsDeferredRouting)
{
    LoadMaterial("M_strandPbr");
    effective["renderStates"]["renderMode"] = "Opaque";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, DefaultLitRejectsHairIdWithoutEvaluator)
{
    LoadMaterial("M_strandPbr");
    effective["shadingModel"] = "Hair";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, DefaultLitRejectsCullingCameraFacingRibbon)
{
    LoadMaterial("M_strandPbr");
    effective["renderStates"]["cullMode"] = "Back";
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, DefaultLitRejectsMaterialVertexDisplacement)
{
    LoadMaterial("M_strandPbr");
    definition["features"]["modifiesMeshPosition"] = true;
    EXPECT_THROW(Plan(), std::runtime_error);
}

TEST_F(TressfxMaterial, DefaultLitAcceptsPbrScalarParameters)
{
    LoadMaterial("M_strandPbr");
    const auto schema = VL::MaterialDescriptorSchema::Build(definition, materialPath);
    effective["parameters"]["u_roughness"] = 0.0;
    effective["parameters"]["u_specular"] = 1.0;
    EXPECT_NO_THROW(schema.ValidateInstanceValues(effective["parameters"], {}, {}, "MI_probe.json"));
}

TEST_F(TressfxMaterial, DefaultLitRequiresPbrScalarParameters)
{
    LoadMaterial("M_strandPbr");
    const auto schema = VL::MaterialDescriptorSchema::Build(definition, materialPath);
    effective["parameters"].erase("u_roughness");
    EXPECT_THROW(schema.ValidateInstanceValues(effective["parameters"], {}, {}, "MI_probe.json"),
        std::runtime_error);
}
}
