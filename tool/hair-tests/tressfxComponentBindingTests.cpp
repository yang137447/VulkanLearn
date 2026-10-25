#include "hair/tressfxSkinning.h"
#include "mesh/validation/meshAssetValidator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{
nlohmann::json Descriptor()
{
    return {{"name", "SM_hair"}, {"type", "tressfx"},
        {"modelDataPath", "Maps/SC_x/Source/hair.tfx"}, {"strandRadius", 0.05f},
        {"materialSlots", {{{"name", "hair"}, {"materialInstancePath", "MI_hair.json"}}}}};
}

nlohmann::json SkinnedDescriptor()
{
    auto json = Descriptor();
    json["simulation"] = {{"backend", "gpu"}, {"skinning", {{"rootBone", "Head_M"}}}};
    return json;
}
} // namespace

// 蓝图 tressfx 组件提供放置级绑定，因此描述符可以省略 source；场景路径仍要求写。
TEST(TressfxComponentBinding, SkinningSourceIsOptional)
{
    const auto plan = MeshAssetValidator::BuildLoadPlan("SM_hair.json", SkinnedDescriptor());
    ASSERT_TRUE(plan.tressfxSimulation.skinning.has_value());
    EXPECT_TRUE(plan.tressfxSimulation.skinning->source.empty());
    EXPECT_EQ(plan.tressfxSimulation.skinning->rootBone, "Head_M");
    EXPECT_FALSE(plan.tressfxSimulation.skinning->rootWeightsAuthored);
}

TEST(TressfxComponentBinding, PresentSourceMustBeNonEmpty)
{
    auto json = SkinnedDescriptor();
    json["simulation"]["skinning"]["source"] = "";
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_hair.json", json), std::runtime_error);
}

TEST(TressfxComponentBinding, AuthoredWeightsAreDistinguishableFromDefault)
{
    auto json = SkinnedDescriptor();
    json["simulation"]["skinning"]["weights"] = "rootBone";
    const auto plan = MeshAssetValidator::BuildLoadPlan("SM_hair.json", json);
    ASSERT_TRUE(plan.tressfxSimulation.skinning.has_value());
    EXPECT_TRUE(plan.tressfxSimulation.skinning->rootWeightsAuthored);
    EXPECT_EQ(plan.tressfxSimulation.skinning->rootWeights, TressfxRootWeightSource::RootBone);
}

TEST(TressfxComponentBinding, ComponentBindingUpgradesUnspecifiedWeightsToMesh)
{
    using VL::Hair::ResolveTressfxRootWeightChoice;
    // 组件绑定 + 描述符未写 weights => mesh 派生。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, false, "", false), 1);
    // 场景路径（无组件）默认保持单根骨。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(false, false, false, "", false), 0);
    // SM 显式 rootBone 必须被尊重，即使由组件挂载。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, true, "", false), 0);
    // 组件显式覆盖优先于 SM。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(false, false, false, "mesh", false), 1);
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, true, true, "rootBone", false), 0);
    // 显式 .tfxbone 优先于“组件绑定自动升级”；只有显式 mesh 才算冲突。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, false, "", true), 0);
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, false, "mesh", true), -2);
}
