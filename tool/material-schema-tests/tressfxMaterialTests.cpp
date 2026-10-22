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
        std::ifstream stream(std::string(VL_SOURCE_ROOT) + "/shader/glsl/hair/M_strandProbe.json");
        ASSERT_TRUE(stream);
        stream >> definition;
        const auto resolved = MaterialInstanceResolver::ResolveWithDefinition("MI_probe.json",
            {{"type", "materialInstance"}, {"material", "shader/glsl/hair/M_strandProbe.json"}},
            definition);
        effective = resolved.effectiveMaterialInstanceJson;
    }
    MaterialInstanceBuildPlan Plan()
    {
        return MaterialInstanceValidator::BuildLoadPlan("MI_probe.json", {}, effective,
            "shader/glsl/hair/M_strandProbe.json", definition, false);
    }
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
}
