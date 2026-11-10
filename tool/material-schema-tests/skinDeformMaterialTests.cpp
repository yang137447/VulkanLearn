#include "materialInstanceValidator.h"
#include "material/loader/materialInstanceResolver.h"

#include <gtest/gtest.h>
#include <fstream>

namespace
{

class SkinDeformMaterial : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::ifstream stream(std::string(VL_SOURCE_ROOT) + "/shader/glsl/M_pbr.json");
        ASSERT_TRUE(stream.good()) << "Missing shader/glsl/M_pbr.json";
        stream >> definition;
        const auto resolved = MaterialInstanceResolver::ResolveWithDefinition("MI_skin_probe.json",
            {{"type", "materialInstance"}, {"material", "shader/glsl/M_pbr.json"}},
            definition);
        effective = resolved.effectiveMaterialInstanceJson;
    }

    MaterialInstanceBuildPlan Plan(bool prerenderSkin)
    {
        return MaterialInstanceValidator::BuildLoadPlan(
            "MI_skin_probe.json", {}, effective, "shader/glsl/M_pbr.json",
            definition, false, prerenderSkin);
    }

    nlohmann::json definition;
    nlohmann::json effective;
};

} // namespace

TEST_F(SkinDeformMaterial, PrerenderMacroChangesSkinnedVariantIdentity)
{
    effective["macros"]["VL_SKINNED_MESH"] = 1;
    const MaterialInstanceBuildPlan blended = Plan(false);
    const MaterialInstanceBuildPlan prerendered = Plan(true);

    ASSERT_TRUE(blended.baseShaderCompileRequest);
    ASSERT_TRUE(prerendered.baseShaderCompileRequest);
    // 两条路径都仍是 SkinnedMesh vertex factory；区别只在 variant 身份。
    EXPECT_EQ(blended.baseShaderCompileRequest->vertexFactoryKey, "SkinnedMesh");
    EXPECT_EQ(prerendered.baseShaderCompileRequest->vertexFactoryKey, "SkinnedMesh");
    EXPECT_FALSE(HasMaterialMacroValue(
        blended.shaderVariantKey.macros, "VL_SKINNED_PRERENDERED", "1"));
    EXPECT_TRUE(HasMaterialMacroValue(
        prerendered.shaderVariantKey.macros, "VL_SKINNED_PRERENDERED", "1"));
    // shader 缓存与 Material 缓存都必须看到两个不同身份，避免复用旧 SPIR-V/pipeline。
    EXPECT_NE(blended.shaderVariantKey.GetNormalizedKey(),
        prerendered.shaderVariantKey.GetNormalizedKey());
    EXPECT_NE(blended.materialKey, prerendered.materialKey);
}

TEST_F(SkinDeformMaterial, PrerenderMacroIsNotAddedToStaticMeshes)
{
    effective["macros"]["VL_SKINNED_MESH"] = 0;
    const MaterialInstanceBuildPlan plan = Plan(true);
    EXPECT_FALSE(HasMaterialMacroValue(
        plan.shaderVariantKey.macros, "VL_SKINNED_PRERENDERED", "1"));
}

TEST_F(SkinDeformMaterial, AuthoredPrerenderMacroIsRejected)
{
    effective["macros"]["VL_SKINNED_MESH"] = 1;
    effective["macros"]["VL_SKINNED_PRERENDERED"] = 1;
    EXPECT_THROW(Plan(false), std::runtime_error);
}
