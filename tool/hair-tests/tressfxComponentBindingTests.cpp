#include "hair/tressfxSkinning.h"
#include "mesh/validation/meshAssetValidator.h"
#include "render/resource/placementBindingCacheKey.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{
nlohmann::json Descriptor()
{
    return {{"name", "TF_hair"}, {"type", "tressfx"},
        {"modelDataPath", "Maps/SC_x/Source/hair.tfx"}, {"strandRadius", 0.05f},
        {"simulationDefaults", {{"enabled", true}, {"backend", "gpu"}}},
        {"materialSlots", {{{"name", "hair"}, {"materialInstancePath", "MI_hair.json"}}}}};
}

nlohmann::json SkinnedPlacement()
{
    return {{"skinning", {{"rootBone", "Head_M"}}}};
}

TressfxSimulationConfig SkinnedConfig()
{
    return VL::Hair::ResolveTressfxBehavior(
        VL::Hair::ReadTressfxSimulationDefaults(Descriptor(), "TF_hair.json"),
        VL::Hair::ReadTressfxPlacementBehavior(SkinnedPlacement(), "Hair"));
}

VL::PlacementBinding Binding()
{
    VL::PlacementBinding binding;
    binding.kind = VL::PlacementBinding::Kind::TressfxSkinning;
    binding.sourceSceneObject = "__blueprint/first/body";
    return binding;
}

std::string CacheKey(const VL::PlacementBinding& binding,
    const TressfxSimulationConfig& config, const std::string& resource = "proxy:v1",
    const std::string& source = "SK:v1;body:v1")
{
    return VL::MakePlacementBindingCacheKey(
        "hair.tfx", binding, config, resource, source);
}
} // namespace

// 蓝图父 mesh 提供来源身份，因此放置作者可以省略 source。
TEST(TressfxComponentBinding, SkinningSourceIsOptional)
{
    const auto config = SkinnedConfig();
    ASSERT_TRUE(config.skinning.has_value());
    EXPECT_TRUE(config.skinning->source.empty());
    EXPECT_EQ(config.skinning->rootBone, "Head_M");
    EXPECT_FALSE(config.skinning->rootWeightsAuthored);
}

TEST(TressfxComponentBinding, PresentSourceMustBeNonEmpty)
{
    auto json = SkinnedPlacement();
    json["skinning"]["source"] = "";
    EXPECT_THROW(VL::Hair::ReadTressfxPlacementBehavior(json, "Hair"), std::runtime_error);
}

TEST(TressfxComponentBinding, AuthoredWeightsAreDistinguishableFromDefault)
{
    auto json = SkinnedPlacement();
    json["skinning"]["weights"] = "rootBone";
    const auto placement = VL::Hair::ReadTressfxPlacementBehavior(json, "Hair");
    ASSERT_TRUE(placement.skinning.has_value());
    EXPECT_TRUE(placement.skinning->rootWeightsAuthored);
    EXPECT_EQ(placement.skinning->rootWeights, TressfxRootWeightSource::RootBone);
}

TEST(TressfxComponentBinding, StandaloneSceneWithoutWeightsKeepsSingleRootBone)
{
    auto node = SkinnedPlacement();
    node["skinning"]["source"] = "Body";
    const auto config = VL::Hair::ResolveTressfxBehavior(
        VL::Hair::ReadTressfxSimulationDefaults(Descriptor(), "TF_hair.json"),
        VL::Hair::ReadTressfxPlacementBehavior(node, "Hair"));
    ASSERT_TRUE(config.skinning);
    EXPECT_EQ(VL::Hair::ResolveTressfxRootWeightChoice(false,
        config.skinning->rootWeights == TressfxRootWeightSource::Mesh,
        config.skinning->rootWeightsAuthored, "", false), 0);
}

TEST(TressfxComponentBinding, ComponentBindingUpgradesUnspecifiedWeightsToMesh)
{
    using VL::Hair::ResolveTressfxRootWeightChoice;
    // 组件绑定 + 放置未写 weights => mesh 派生。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, false, "", false), 1);
    // 场景路径（无组件）默认保持单根骨。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(false, false, false, "", false), 0);
    // 放置显式 rootBone 必须被尊重，即使由组件挂载。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, true, "", false), 0);
    // 组件显式覆盖优先于有效配置。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(false, false, false, "mesh", false), 1);
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, true, true, "rootBone", false), 0);
    // 显式 .tfxbone 优先于“组件绑定自动升级”；只有显式 mesh 才算冲突。
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, false, "", true), 0);
    EXPECT_EQ(ResolveTressfxRootWeightChoice(true, false, false, "mesh", true), -2);
}

TEST(TressfxComponentBinding, AssetDefaultsDoNotOwnSkinning)
{
    auto json = Descriptor();
    json["simulationDefaults"]["skinning"] = {{"rootBone", "Head_M"}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", json), std::runtime_error);
}

TEST(TressfxComponentBinding, OffKeepsSkinningAndCollisionDeclaration)
{
    auto placement = SkinnedPlacement();
    placement["simulation"] = {{"mode", "off"}};
    placement["hairCollision"] = {{"mesh", {{"path", "Source/body.obj"}}}};
    const auto config = VL::Hair::ResolveTressfxBehavior(
        VL::Hair::ReadTressfxSimulationDefaults(Descriptor(), "TF_hair.json"),
        VL::Hair::ReadTressfxPlacementBehavior(placement, "Hair"));
    EXPECT_FALSE(config.enabled);
    ASSERT_TRUE(config.skinning.has_value());
    EXPECT_EQ(config.skinning->rootBone, "Head_M");
    ASSERT_TRUE(config.collisionMesh.has_value());
    EXPECT_EQ(config.collisionMesh->path, "Source/body.obj");
}

TEST(TressfxComponentBinding, CacheReusesPayloadAcrossPlacementSourceRebasing)
{
    auto first = Binding();
    first.tressfx = SkinnedConfig();
    first.tressfx->skinning->source = first.sourceSceneObject;
    auto second = first;
    VL::RebasePlacementBindingSource(second, "__blueprint/first/", "__blueprint/second/");
    EXPECT_EQ(second.sourceSceneObject, "__blueprint/second/body");
    EXPECT_EQ(second.tressfx->skinning->source, second.sourceSceneObject);
    EXPECT_EQ(CacheKey(first, *first.tressfx), CacheKey(second, *second.tressfx));
}

TEST(TressfxComponentBinding, CacheSeparatesRootBoneOverrides)
{
    auto binding = Binding();
    const auto config = SkinnedConfig();
    const auto previous = CacheKey(binding, config);
    binding.rootBone = "Neck_M";
    EXPECT_NE(previous, CacheKey(binding, config));
}

TEST(TressfxComponentBinding, CacheSeparatesWeightOverrides)
{
    auto binding = Binding();
    const auto config = SkinnedConfig();
    const auto previous = CacheKey(binding, config);
    binding.weights = "rootBone";
    EXPECT_NE(previous, CacheKey(binding, config));
}

TEST(TressfxComponentBinding, CacheSeparatesEffectiveSimulationModes)
{
    const auto binding = Binding();
    auto config = SkinnedConfig();
    const auto enabled = CacheKey(binding, config);
    config.enabled = false;
    EXPECT_NE(enabled, CacheKey(binding, config));
}

TEST(TressfxComponentBinding, CacheSeparatesEffectiveCapsuleBones)
{
    const auto binding = Binding();
    auto config = SkinnedConfig();
    config.capsules.push_back({});
    config.capsules.back().bone = "Head_M";
    const auto previous = CacheKey(binding, config);
    config.capsules.back().bone = "Neck_M";
    EXPECT_NE(previous, CacheKey(binding, config));
}

TEST(TressfxComponentBinding, CacheSeparatesPreparedInputContent)
{
    const auto binding = Binding();
    const auto config = SkinnedConfig();
    EXPECT_NE(CacheKey(binding, config, "weightsBlake3:v1"),
        CacheKey(binding, config, "weightsBlake3:v2"));
}

TEST(TressfxComponentBinding, CacheSeparatesSourceSkeletonContent)
{
    const auto binding = Binding();
    const auto config = SkinnedConfig();
    EXPECT_NE(CacheKey(binding, config, "", "samePath;SKBlake3:v1"),
        CacheKey(binding, config, "", "samePath;SKBlake3:v2"));
}

TEST(TressfxComponentBinding, CacheSeparatesSourceBodyContent)
{
    const auto binding = Binding();
    const auto config = SkinnedConfig();
    EXPECT_NE(CacheKey(binding, config, "", "SK:v1;modelBlake3:v1"),
        CacheKey(binding, config, "", "SK:v1;modelBlake3:v2"));
}
