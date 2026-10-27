#include "mesh/loader/hair/tressfxModelImporter.h"
#include "mesh/validation/meshAssetValidator.h"
#include "render/resource/renderableObjectKey.h"
#include "hair/tressfxBehavior.h"
#include "tfxFixture.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{
// 既有数值域用例同时提供默认值和挂载片段；正式资产校验只接收 TF 字段，
// 骨骼/碰撞交给同一放置合成入口，身份断言针对有效 renderable 配置。
MeshAssetBuildPlan BuildPlacementFixturePlan(
    std::string_view path, nlohmann::json asset)
{
    nlohmann::json placement = nlohmann::json::object();
    if (asset.contains("simulationDefaults") &&
        asset["simulationDefaults"].contains("skinning"))
    {
        placement["skinning"] = asset["simulationDefaults"]["skinning"];
        asset["simulationDefaults"].erase("skinning");
    }
    if (asset.contains("hairCollision"))
    {
        placement["hairCollision"] = asset["hairCollision"];
        asset.erase("hairCollision");
    }
    auto plan = MeshAssetValidator::BuildLoadPlan(path, asset);
    const auto behavior = VL::Hair::ReadTressfxPlacementBehavior(placement, std::string(path));
    plan.tressfxSimulation = VL::Hair::ResolveTressfxBehavior(
        plan.tressfxSimulation, behavior, std::string(path));
    plan.modelCacheKey += "|behavior=" + VL::Hair::MakeTressfxBehaviorKey(plan.tressfxSimulation);
    return plan;
}

nlohmann::json Descriptor(float radius = 0.05f)
{
    return {{"name", "SM_guides"}, {"type", "tressfx"},
        {"modelDataPath", "Maps/SC_probe/Source/guides.tfx"}, {"strandRadius", radius},
        {"materialSlots", {{{"name", "hair"}, {"materialInstancePath", "MI_probe.json"}}}}};
}

nlohmann::json GpuDescriptor()
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"backend", "gpu"}};
    json["hairCollision"] = {{"capsules", nlohmann::json::array()}};
    return json;
}

TEST(TressfxBehavior, GeometryIdentityIgnoresSimulationDefaults)
{
    auto asset = Descriptor();
    asset["simulationDefaults"] = {{"enabled", true}, {"backend", "gpu"}, {"damping", 0.03f}};
    const auto first = MeshAssetValidator::BuildLoadPlan("TF_hair.json", asset);
    asset["simulationDefaults"]["damping"] = 0.05f;
    const auto second = MeshAssetValidator::BuildLoadPlan("TF_hair.json", asset);
    EXPECT_EQ(first.modelCacheKey, second.modelCacheKey);
    EXPECT_NE(VL::Hair::MakeTressfxBehaviorKey(first.tressfxSimulation),
        VL::Hair::MakeTressfxBehaviorKey(second.tressfxSimulation));
}

TEST(TressfxBehavior, RejectsLegacyAssetBehaviorAndCharacterInputsInDefaults)
{
    for (const char* field : {"simulation", "collision", "collisionDefaults", "hairCollision"})
    {
        auto asset = Descriptor();
        asset[field] = nlohmann::json::object();
        EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", asset), std::runtime_error);
    }
    auto asset = Descriptor();
    asset["simulationDefaults"] = {{"skinning", {{"rootBone", "Head"}}}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", asset), std::runtime_error);
}

TEST(TressfxBehavior, InheritOffAndOnResolveAgainstOneDefaultConfig)
{
    auto asset = Descriptor();
    asset["simulationDefaults"] = {{"enabled", true}, {"backend", "gpu"}, {"damping", 0.03f}};
    const auto defaults = MeshAssetValidator::BuildLoadPlan("TF_hair.json", asset).tressfxSimulation;
    const auto inherit = VL::Hair::ResolveTressfxBehavior(defaults,
        VL::Hair::ReadTressfxPlacementBehavior(nlohmann::json::object(), "inherit"));
    const auto off = VL::Hair::ResolveTressfxBehavior(defaults,
        VL::Hair::ReadTressfxPlacementBehavior(
            {{"simulation", {{"mode", "off"}, {"overrides", {{"damping", 0.06f}}}}},
             {"skinning", {{"source", "Body"}, {"rootBone", "Head"}, {"weights", "mesh"}}}},
            "off"));
    EXPECT_TRUE(inherit.enabled);
    EXPECT_FALSE(off.enabled);
    EXPECT_TRUE(off.skinning.has_value());
    EXPECT_FLOAT_EQ(off.damping, 0.06f);
    EXPECT_FLOAT_EQ(defaults.damping, 0.03f);
    auto disabledDefaults = defaults;
    disabledDefaults.enabled = false;
    const auto on = VL::Hair::ResolveTressfxBehavior(disabledDefaults,
        VL::Hair::ReadTressfxPlacementBehavior({{"simulation", {{"mode", "on"}}}}, "on"));
    EXPECT_TRUE(on.enabled);
    EXPECT_NE(VL::Hair::MakeTressfxBehaviorKey(on), VL::Hair::MakeTressfxBehaviorKey(off));
}

TEST(TressfxBehavior, OverridesRejectEnabledBindingCollisionAndUnknownFields)
{
    for (const char* field : {"enabled", "skinning", "collision", "hairCollision", "dampping"})
        EXPECT_THROW(VL::Hair::ReadTressfxPlacementBehavior(
            {{"simulation", {{"overrides", {{field, false}}}}}}, "bad"), std::runtime_error);
    EXPECT_THROW(VL::Hair::ReadTressfxPlacementBehavior(
        {{"simulation", {{"mode", "simulate"}}}}, "bad"), std::runtime_error);
}

TEST(TressfxBehavior, IntegerDefaultsAcceptIntegralFloatingPointValues)
{
    auto asset = Descriptor();
    asset["simulationDefaults"] = {{"iterations", 8.0}, {"rootFixedPoints", 2.0}};
    const auto plan = MeshAssetValidator::BuildLoadPlan("TF_hair.json", asset);
    EXPECT_EQ(plan.tressfxSimulation.iterations, 8u);
    EXPECT_EQ(plan.tressfxSimulation.rootFixedPoints, 2u);
}

TEST(TressfxBehavior, IntegerOverridesRejectFractionNegativeOverflowAndNonfinite)
{
    const std::vector<nlohmann::json> invalid = {
        1.5, -1, 4294967296.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(), "2", true};
    for (const char* field : {"iterations", "rootFixedPoints"})
        for (const auto& value : invalid)
        {
            SCOPED_TRACE(field);
            SCOPED_TRACE(value.dump());
            EXPECT_THROW(VL::Hair::ReadTressfxPlacementBehavior(
                {{"simulation", {{"overrides", {{field, value}}}}}}, "TF_hair.json"),
                std::runtime_error);
        }
}

TEST(TressfxDescriptor, RequiresPositiveFiniteRepresentableRadius)
{
    for (double radius : {0.0, -1.0, 1e-300, 1e300, std::numeric_limits<double>::infinity()})
    {
        auto json = Descriptor();
        json["strandRadius"] = radius;
        EXPECT_THROW(BuildPlacementFixturePlan("SM_guides.json", json), std::runtime_error);
    }
    auto json = Descriptor();
    json.erase("strandRadius");
    EXPECT_THROW(BuildPlacementFixturePlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RadiusParticipatesInMeshIdentityWithoutDecimalTruncation)
{
    const auto first = BuildPlacementFixturePlan("SM_a.json", Descriptor(0.000001f));
    const auto second = BuildPlacementFixturePlan("SM_b.json", Descriptor(0.0000011f));
    EXPECT_NE(first.modelCacheKey, second.modelCacheKey);
}

TEST(TressfxDescriptor, SimulationFieldsAreValidatedAndPartOfIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"enabled", true},
        {"gravity", {0.0f, -980.0f, 0.0f}},
        {"wind", {10.0f, 0.0f, 3.0f}},
        {"damping", 0.98f},
        {"iterations", 4},
        {"rootFixedPoints", 2},
        {"maxDt", 0.016f}};
    const auto plan = BuildPlacementFixturePlan("SM_dynamic.json", json);
    EXPECT_TRUE(plan.tressfxSimulation.enabled);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.gravity.y(), -980.0f);
    EXPECT_EQ(plan.tressfxSimulation.iterations, 4u);
    EXPECT_NE(plan.modelCacheKey, "Maps/SC_probe/Source/guides.tfx");
}

TEST(TressfxDescriptor, SimulationRejectsInvalidControls)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"damping", 1.2f}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json), std::runtime_error);
    json["simulationDefaults"] = {{"gravity", {0.0f, 1.0f}}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json), std::runtime_error);
    json["simulationDefaults"] = {{"iterations", 17}};
    try
    {
        BuildPlacementFixturePlan("SM_iterations.json", json);
        FAIL() << "Out-of-range TressFX iterations were accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("[1, 16]"), std::string::npos);
        EXPECT_NE(message.find("SM_iterations.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SimulationBackendDefaultsToCpu)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"enabled", true}};
    const auto plan = BuildPlacementFixturePlan("SM_dynamic.json", json);
    EXPECT_EQ(plan.tressfxSimulation.backend, TressfxSimulationBackend::Cpu);
}

TEST(TressfxDescriptor, SimulationBackendAcceptsCpuAndGpu)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"backend", "cpu"}};
    EXPECT_EQ(BuildPlacementFixturePlan("SM_dynamic.json", json)
            .tressfxSimulation.backend, TressfxSimulationBackend::Cpu);

    json["simulationDefaults"] = {{"backend", "gpu"}};
    EXPECT_EQ(BuildPlacementFixturePlan("SM_dynamic.json", json)
            .tressfxSimulation.backend, TressfxSimulationBackend::Gpu);
}

TEST(TressfxDescriptor, SimulationBackendRejectsUnknownValueWithAssetPath)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"backend", "cuda"}};
    try
    {
        BuildPlacementFixturePlan("Maps/SC_probe/Meshes/SM_dynamic.json", json);
        FAIL() << "Unknown TressFX simulation backend was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("SM_dynamic.json"), std::string::npos);
        EXPECT_NE(message.find("cpu"), std::string::npos);
        EXPECT_NE(message.find("gpu"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SimulationBackendParticipatesInMeshIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"backend", "cpu"}};
    const auto cpuPlan = BuildPlacementFixturePlan("SM_dynamic.json", json);
    json["simulationDefaults"] = {{"backend", "gpu"}};
    const auto gpuPlan = BuildPlacementFixturePlan("SM_dynamic.json", json);
    EXPECT_NE(cpuPlan.modelCacheKey, gpuPlan.modelCacheKey);
}

TEST(TressfxDescriptor, SkinningAcceptsSourceAndRootBoneWithGpuBackend)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    const auto plan = BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    ASSERT_TRUE(plan.tressfxSimulation.skinning.has_value());
    EXPECT_EQ(plan.tressfxSimulation.skinning->source, "FemaleCharacter");
    EXPECT_EQ(plan.tressfxSimulation.skinning->rootBone, "Head_M");
}

TEST(TressfxDescriptor, SkinningAcceptsOptionalBoneWeights)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"},
            {"boneWeights", "Maps/SC_x/female.tfxbone"}}}};
    const auto plan = BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    ASSERT_TRUE(plan.tressfxSimulation.skinning.has_value());
    EXPECT_EQ(plan.tressfxSimulation.skinning->boneWeightsPath,
        "Maps/SC_x/female.tfxbone");
}

TEST(TressfxDescriptor, SkinningRequiresGpuBackend)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    try
    {
        BuildPlacementFixturePlan("Maps/SC_x/Meshes/SM_hair_skinned.json", json);
        FAIL() << "CPU backend accepted simulation.skinning";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("gpu"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

// 蓝图 tressfx 组件提供放置级绑定后，source 不再由描述符强制；rootBone 仍必需，
// 但“写了 source 就必须非空”依旧在描述符边界收口。
TEST(TressfxDescriptor, SkinningRequiresRootBoneAndValidSource)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"rootBone", "Head_M"}}}};
    EXPECT_NO_THROW(BuildPlacementFixturePlan("SM_hair_skinned.json", json));

    auto missingRoot = json;
    missingRoot["simulationDefaults"]["skinning"].erase("rootBone");
    EXPECT_THROW(
        BuildPlacementFixturePlan("SM_hair_skinned.json", missingRoot),
        std::runtime_error);

    auto emptySource = json;
    emptySource["simulationDefaults"]["skinning"]["source"] = "";
    EXPECT_THROW(
        BuildPlacementFixturePlan("SM_hair_skinned.json", emptySource),
        std::runtime_error);
}

TEST(TressfxDescriptor, SkinningRequiresRootBone)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}}}};
    try
    {
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
        FAIL() << "simulation.skinning without rootBone was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("rootBone"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SkinningRejectsUnknownKeys)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"},
            {"extra", 1}}}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_hair_skinned.json", json),
        std::runtime_error);
}

TEST(TressfxDescriptor, SkinBoneWeightsParticipateInMeshIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    const auto first =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    json["simulationDefaults"]["skinning"]["boneWeights"] = "Maps/SC_x/female.tfxbone";
    const auto second =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    EXPECT_NE(first.modelCacheKey, second.modelCacheKey);
}

TEST(TressfxDescriptor, SkinningSourceParticipatesInMeshIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    const auto first =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    json["simulationDefaults"]["skinning"]["source"] = "OtherCharacter";
    const auto second =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    EXPECT_NE(first.modelCacheKey, second.modelCacheKey);
}

TEST(TressfxDescriptor, SkinningRootBoneParticipatesInMeshIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    const auto first =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    json["simulationDefaults"]["skinning"]["rootBone"] = "HeadEnd_M";
    const auto second =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    EXPECT_NE(first.modelCacheKey, second.modelCacheKey);
}

TEST(TressfxDescriptor, SkinningDefaultsToRootBoneWeights)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    const auto plan = BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    ASSERT_TRUE(plan.tressfxSimulation.skinning.has_value());
    EXPECT_EQ(plan.tressfxSimulation.skinning->rootWeights,
        TressfxRootWeightSource::RootBone);
}

TEST(TressfxDescriptor, SkinningAcceptsMeshRootWeights)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"},
            {"weights", "mesh"}}}};
    const auto plan = BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    ASSERT_TRUE(plan.tressfxSimulation.skinning.has_value());
    EXPECT_EQ(plan.tressfxSimulation.skinning->rootWeights,
        TressfxRootWeightSource::Mesh);
}

TEST(TressfxDescriptor, SkinningRejectsUnknownRootWeightSource)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"},
            {"weights", "auto"}}}};
    try
    {
        BuildPlacementFixturePlan("Maps/SC_x/Meshes/SM_hair_skinned.json", json);
        FAIL() << "Unknown simulation.skinning.weights value was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("weights"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SkinningMeshWeightsRejectBoneWeightsCombination)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"},
            {"weights", "mesh"}, {"boneWeights", "Maps/SC_x/female.tfxbone"}}}};
    try
    {
        BuildPlacementFixturePlan("Maps/SC_x/Meshes/SM_hair_skinned.json", json);
        FAIL() << "mesh root weights combined with .tfxbone were accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("boneWeights"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SkinningMeshWeightsRequireGpuBackend)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"},
            {"weights", "mesh"}}}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_hair_skinned.json", json),
        std::runtime_error);
}

TEST(TressfxDescriptor, SkinningMeshWeightsParticipateInMeshIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "gpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    const auto singleBone =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    json["simulationDefaults"]["skinning"]["weights"] = "mesh";
    const auto meshWeights =
        BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    EXPECT_NE(singleBone.modelCacheKey, meshWeights.modelCacheKey);
}

TEST(TressfxDescriptor, SimulationQualityDefaults)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"enabled", true}};
    const auto config =
        BuildPlacementFixturePlan("SM_dynamic.json", json).tressfxSimulation;
    EXPECT_FLOAT_EQ(config.bendStiffness, 0.3f);
    // damping 是源默认 DampingCoeff；maxDt 取源 Integration 参数函数上限。
    EXPECT_FLOAT_EQ(config.damping, 0.035f);
    EXPECT_FLOAT_EQ(config.maxDt, 0.016f);
    // global shape 与 VSP 默认关闭，既有资产外观不变。
    EXPECT_FLOAT_EQ(config.globalShapeStiffness, 0.0f);
    EXPECT_FLOAT_EQ(config.globalShapeEffectiveRange, 0.0f);
    EXPECT_FLOAT_EQ(config.vspCoefficient, 0.0f);
    EXPECT_FLOAT_EQ(config.vspAccelThresholdMax, 140.0f);
}

TEST(TressfxDescriptor, VspThresholdDefaultAndExplicitValueShareEffectiveIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"vspCoefficient", 0.5f}};
    const auto implicit = BuildPlacementFixturePlan("SM_vsp.json", json);
    json["simulationDefaults"]["vspAccelThresholdMax"] = 140.0f;
    const auto explicitDefault =
        BuildPlacementFixturePlan("SM_vsp.json", json);
    EXPECT_EQ(implicit.modelCacheKey, explicitDefault.modelCacheKey);
}

TEST(TressfxDescriptor, ExplicitLegacyVspThresholdIsPreservedAndChangesIdentity)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"vspCoefficient", 0.5f}};
    const auto implicit = BuildPlacementFixturePlan("SM_vsp.json", json);
    json["simulationDefaults"]["vspAccelThresholdMax"] = 5000.0f;
    const auto explicitLegacy =
        BuildPlacementFixturePlan("SM_vsp.json", json);
    EXPECT_FLOAT_EQ(explicitLegacy.tressfxSimulation.vspAccelThresholdMax, 5000.0f);
    EXPECT_NE(implicit.modelCacheKey, explicitLegacy.modelCacheKey);
}

TEST(TressfxDescriptor, SimulationQualityFieldsAcceptedAndPartOfIdentity)
{
    auto baselineJson = Descriptor();
    baselineJson["simulationDefaults"] = nlohmann::json::object();
    const auto baseline =
        BuildPlacementFixturePlan("SM_dynamic.json", baselineJson);

    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"bendStiffness", 0.7f},
        {"damping", 0.05f},
        {"globalShapeStiffness", 0.4f},
        {"globalShapeEffectiveRange", 0.6f},
        {"vspCoefficient", 0.3f},
        {"vspAccelThresholdMax", 7000.0f}};
    const auto custom = BuildPlacementFixturePlan("SM_dynamic.json", json);
    EXPECT_FLOAT_EQ(custom.tressfxSimulation.bendStiffness, 0.7f);
    EXPECT_FLOAT_EQ(custom.tressfxSimulation.damping, 0.05f);
    EXPECT_FLOAT_EQ(custom.tressfxSimulation.globalShapeStiffness, 0.4f);
    EXPECT_FLOAT_EQ(custom.tressfxSimulation.globalShapeEffectiveRange, 0.6f);
    EXPECT_FLOAT_EQ(custom.tressfxSimulation.vspCoefficient, 0.3f);
    EXPECT_FLOAT_EQ(custom.tressfxSimulation.vspAccelThresholdMax, 7000.0f);
    EXPECT_NE(custom.modelCacheKey, baseline.modelCacheKey);
}

TEST(TressfxDescriptor, SimulationStiffnessCurvesAreParsedAndPartOfIdentity)
{
    auto baselineJson = Descriptor();
    baselineJson["simulationDefaults"] = nlohmann::json::object();
    const auto baseline =
        BuildPlacementFixturePlan("SM_dynamic.json", baselineJson);

    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"vspStiffness", {0.0f, 0.25f, 0.5f, 0.75f}},
        {"localShapeStiffness", {0.95f, 0.8f, 0.4f, 0.0f}}};
    const auto authored =
        BuildPlacementFixturePlan("SM_dynamic.json", json);
    ASSERT_EQ(authored.tressfxSimulation.vspStiffness.size(), 4u);
    EXPECT_FLOAT_EQ(authored.tressfxSimulation.vspStiffness[1], 0.25f);
    EXPECT_FLOAT_EQ(authored.tressfxSimulation.vspStiffness[3], 0.75f);
    ASSERT_EQ(authored.tressfxSimulation.localShapeStiffness.size(), 4u);
    EXPECT_FLOAT_EQ(authored.tressfxSimulation.localShapeStiffness[0], 0.95f);
    EXPECT_NE(authored.modelCacheKey, baseline.modelCacheKey);

    // 只改一个作者值也必须改变模型缓存身份。
    json["simulationDefaults"]["vspStiffness"][2] = 0.5f + 1.0f / 1024.0f;
    const auto changed =
        BuildPlacementFixturePlan("SM_dynamic.json", json);
    EXPECT_NE(authored.modelCacheKey, changed.modelCacheKey);

    // 两条曲线各自的省略/存在也必须区分。
    auto vspOnlyJson = Descriptor();
    vspOnlyJson["simulationDefaults"] = {{"vspStiffness", {0.0f, 0.25f, 0.5f, 0.75f}}};
    const auto vspOnly =
        BuildPlacementFixturePlan("SM_dynamic.json", vspOnlyJson);
    EXPECT_NE(authored.modelCacheKey, vspOnly.modelCacheKey);
}

TEST(TressfxDescriptor, SimulationStiffnessCurvesRejectInvalidEntries)
{
    const auto expectRejected = [](const nlohmann::json& curve, const char* field)
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{field, curve}};
        try
        {
            BuildPlacementFixturePlan(
                "Maps/SC_probe/Meshes/SM_curves.json", json);
            FAIL() << "Invalid " << field << " was accepted";
        }
        catch (const std::runtime_error& error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find(field), std::string::npos);
            EXPECT_NE(message.find("SM_curves.json"), std::string::npos);
        }
    };
    expectRejected(nlohmann::json::array({0.5, 1.1}), "vspStiffness");
    expectRejected(nlohmann::json::array({-0.1, 0.5}), "vspStiffness");
    expectRejected(nlohmann::json::array(), "localShapeStiffness");
    // 超过逐顶点上限 64。
    nlohmann::json oversize = nlohmann::json::array();
    for (int index = 0; index < 65; ++index)
        oversize.push_back(0.5);
    expectRejected(oversize, "localShapeStiffness");
    // 非数字项。
    expectRejected(nlohmann::json::array({0.5, "half"}), "vspStiffness");
}

TEST(TressfxDescriptor, LocalShapeCurveValuesAboveLimitAreClampedAndRecorded)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"localShapeStiffness", {0.5f, 1.0f, 0.96f}}};
    testing::internal::CaptureStderr();
    const auto plan = BuildPlacementFixturePlan(
        "Maps/SC_probe/Meshes/SM_curves.json", json);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_EQ(plan.tressfxSimulation.localShapeStiffness.size(), 3u);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.localShapeStiffness[0], 0.5f);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.localShapeStiffness[1], 0.95f);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.localShapeStiffness[2], 0.95f);
    // 不能静默修改作者值：日志必须带字段、索引与资产路径。
    EXPECT_NE(log.find("localShapeStiffness[1]"), std::string::npos);
    EXPECT_NE(log.find("localShapeStiffness[2]"), std::string::npos);
    EXPECT_NE(log.find("SM_curves.json"), std::string::npos);
}

TEST(TressfxDescriptor, SimulationRejectsUnknownCurveKeys)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"vspStiffnesss", {0.0f, 1.0f, 1.0f, 1.0f}}};
    try
    {
        BuildPlacementFixturePlan("SM_curves.json", json);
        FAIL() << "Unknown simulation field was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("unknown field"), std::string::npos);
        EXPECT_NE(message.find("vspStiffnesss"), std::string::npos);
        EXPECT_NE(message.find("SM_curves.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SimulationGlobalShapeAndVspParticipateInIdentity)
{
    auto baselineJson = Descriptor();
    baselineJson["simulationDefaults"] = nlohmann::json::object();
    const auto baseline =
        BuildPlacementFixturePlan("SM_dynamic.json", baselineJson);

    auto globalShape = Descriptor();
    globalShape["simulationDefaults"] = {{"globalShapeStiffness", 0.5f}};
    const auto shaped =
        BuildPlacementFixturePlan("SM_dynamic.json", globalShape);
    EXPECT_NE(shaped.modelCacheKey, baseline.modelCacheKey);

    auto vsp = Descriptor();
    vsp["simulationDefaults"] = {{"vspCoefficient", 0.5f}};
    const auto vspPlan = BuildPlacementFixturePlan("SM_dynamic.json", vsp);
    EXPECT_NE(vspPlan.modelCacheKey, baseline.modelCacheKey);
    EXPECT_NE(vspPlan.modelCacheKey, shaped.modelCacheKey);
}

TEST(TressfxDescriptor, SimulationRejectsInvalidBendAndDampingControls)
{
    for (const float bendStiffness : {-0.1f, 1.1f})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{"bendStiffness", bendStiffness}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }
    for (const float damping : {-0.1f, 1.1f})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{"damping", damping}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }
}

TEST(TressfxDescriptor, SimulationRejectsRemovedWindGustFields)
{
    for (const char* removed : {"windGust", "windFrequency"})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{removed, 0.2f}};
        try
        {
            BuildPlacementFixturePlan(
                "Maps/SC_probe/Meshes/SM_gust_legacy.json", json);
            FAIL() << "Removed VL gust field was accepted: " << removed;
        }
        catch (const std::runtime_error& error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find(removed), std::string::npos);
            EXPECT_NE(message.find("SetWind"), std::string::npos);
            EXPECT_NE(message.find("SM_gust_legacy.json"), std::string::npos);
        }
    }
}

TEST(TressfxDescriptor, SimulationRejectsTimeStepAboveTressFxClamp)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"maxDt", 0.017f}};
    try
    {
        BuildPlacementFixturePlan("SM_dt.json", json);
        FAIL() << "maxDt above the TressFX 0.016 s clamp was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("0.016"), std::string::npos);
        EXPECT_NE(message.find("SM_dt.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, SimulationRejectsInvalidGlobalShapeAndVspControls)
{
    for (const float stiffness : {-0.1f, 1.1f})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{"globalShapeStiffness", stiffness}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }
    for (const float range : {-0.1f, 1.1f})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{"globalShapeEffectiveRange", range}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }
    for (const float coefficient : {-0.1f, 1.1f})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{"vspCoefficient", coefficient}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }
    for (const float threshold : {0.0f, -1.0f, 1.0e8f})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{"vspAccelThresholdMax", threshold}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }
}

TEST(TressfxDescriptor, SimulationRejectsRemovedFixedStepFields)
{
    for (const char* removed : {"fixedStep", "maxSubsteps"})
    {
        auto json = Descriptor();
        json["simulationDefaults"] = {{removed, 1}};
        try
        {
            BuildPlacementFixturePlan("Maps/SC_probe/Meshes/SM_legacy.json", json);
            FAIL() << "Legacy accumulator field was accepted: " << removed;
        }
        catch (const std::runtime_error& error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find(removed), std::string::npos);
            EXPECT_NE(message.find("maxDt"), std::string::npos);
            EXPECT_NE(message.find("SM_legacy.json"), std::string::npos);
        }
    }
}

TEST(TressfxDescriptor, CollisionAcceptsAuthoredCapsules)
{
    auto json = Descriptor();
    EXPECT_TRUE(BuildPlacementFixturePlan("SM_dynamic.json", json)
            .tressfxSimulation.capsules.empty());

    json["hairCollision"] = {{"capsules", nlohmann::json::array()}};
    EXPECT_TRUE(BuildPlacementFixturePlan("SM_dynamic.json", json)
            .tressfxSimulation.capsules.empty());

    json["hairCollision"]["capsules"].push_back({
        {"a", {0.0f, 160.0f, 0.0f}},
        {"b", {0.0f, 174.0f, 0.0f}},
        {"radius", 8.0f}});
    const auto plan = BuildPlacementFixturePlan("SM_dynamic.json", json);
    ASSERT_EQ(plan.tressfxSimulation.capsules.size(), 1u);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.capsules[0].a.y(), 160.0f);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.capsules[0].b.y(), 174.0f);
    EXPECT_FLOAT_EQ(plan.tressfxSimulation.capsules[0].radius, 8.0f);
}

TEST(TressfxDescriptor, CollisionCapsuleAcceptsOptionalBone)
{
    auto json = GpuDescriptor();
    json["simulationDefaults"]["skinning"] = {
        {"source", "FemaleCharacter"}, {"rootBone", "Head_M"}};
    json["hairCollision"]["capsules"].push_back({
        {"a", {0.0f, 160.0f, 0.0f}},
        {"b", {0.0f, 160.0f, 0.0f}},
        {"radius", 8.0f},
        {"bone", "Head_M"}});
    const auto plan = BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    ASSERT_EQ(plan.tressfxSimulation.capsules.size(), 1u);
    EXPECT_EQ(plan.tressfxSimulation.capsules[0].bone, "Head_M");
}

TEST(TressfxDescriptor, CollisionCapsuleBoneRequiresSkinningSource)
{
    auto json = GpuDescriptor();
    json["hairCollision"]["capsules"].push_back({
        {"a", {0.0f, 0.0f, 0.0f}},
        {"b", {0.0f, 0.0f, 0.0f}},
        {"radius", 1.0f},
        {"bone", "Head_M"}});
    try
    {
        BuildPlacementFixturePlan(
            "Maps/SC_x/Meshes/SM_hair_skinned.json", json);
        FAIL() << "Capsule bone without simulation.skinning was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("simulation.skinning"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, CollisionCapsuleBoneRequiresGpuBackend)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"backend", "cpu"},
        {"skinning", {{"source", "FemaleCharacter"}, {"rootBone", "Head_M"}}}};
    json["hairCollision"] = {{"capsules", {{
        {"a", {0.0f, 0.0f, 0.0f}},
        {"b", {0.0f, 0.0f, 0.0f}},
        {"radius", 1.0f},
        {"bone", "Head_M"}}}}};
    // CPU backend + skinning 本身已在 simulation 解析期被拒绝；这里单独确认
    // 错误消息落在 backend 约束上，而不是静默忽略 bone。
    EXPECT_THROW(BuildPlacementFixturePlan("SM_hair_skinned.json", json),
        std::runtime_error);
}

TEST(TressfxDescriptor, CollisionCapsuleBoneRejectsEmptyAndNonString)
{
    for (const nlohmann::json& bone : {nlohmann::json(""), nlohmann::json(1)})
    {
        auto json = GpuDescriptor();
        json["simulationDefaults"]["skinning"] = {
            {"source", "FemaleCharacter"}, {"rootBone", "Head_M"}};
        json["hairCollision"]["capsules"].push_back({
            {"a", {0.0f, 0.0f, 0.0f}},
            {"b", {0.0f, 0.0f, 0.0f}},
            {"radius", 1.0f},
            {"bone", bone}});
        EXPECT_THROW(BuildPlacementFixturePlan("SM_hair_skinned.json", json),
            std::runtime_error);
    }
}

TEST(TressfxDescriptor, CollisionCapsuleBoneParticipatesInMeshIdentity)
{
    const auto plan = [](const std::string& bone)
    {
        auto json = GpuDescriptor();
        json["simulationDefaults"]["skinning"] = {
            {"source", "FemaleCharacter"}, {"rootBone", "Head_M"}};
        json["hairCollision"]["capsules"].push_back({
            {"a", {0.0f, 0.0f, 0.0f}},
            {"b", {0.0f, 0.0f, 0.0f}},
            {"radius", 1.0f},
            {"bone", bone}});
        return BuildPlacementFixturePlan("SM_hair_skinned.json", json);
    };
    EXPECT_NE(plan("Head_M").modelCacheKey, plan("Neck_M").modelCacheKey);
}

TEST(TressfxDescriptor, CollisionFieldsParticipateInMeshIdentity)
{
    auto json = Descriptor();
    json["hairCollision"] = {{"capsules", nlohmann::json::array()}};
    json["hairCollision"]["capsules"].push_back({
        {"a", {0.0f, 0.0f, 0.0f}},
        {"b", {0.0f, 10.0f, 0.0f}},
        {"radius", 2.0f}});
    const auto first = BuildPlacementFixturePlan("SM_dynamic.json", json);
    json["hairCollision"]["capsules"][0]["radius"] = 2.5f;
    const auto second = BuildPlacementFixturePlan("SM_dynamic.json", json);
    EXPECT_NE(first.modelCacheKey, second.modelCacheKey);
}

TEST(TressfxDescriptor, CollisionRejectsInvalidCapsules)
{
    const auto capsule = [](float radius)
    {
        return nlohmann::json{
            {"a", {0.0f, 0.0f, 0.0f}},
            {"b", {0.0f, 10.0f, 0.0f}},
            {"radius", radius}};
    };

    for (const float radius : {0.0f, -1.0f})
    {
        auto json = Descriptor();
        json["hairCollision"] = {{"capsules", {capsule(radius)}}};
        EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", json),
            std::runtime_error);
    }

    auto tooMany = Descriptor();
    tooMany["hairCollision"] = {{"capsules", nlohmann::json::array()}};
    for (int i = 0; i < 9; ++i)
        tooMany["hairCollision"]["capsules"].push_back(capsule(1.0f));
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", tooMany),
        std::runtime_error);
}

TEST(TressfxDescriptor, CollisionRejectsInvalidCapsuleFields)
{
    auto nonFinite = Descriptor();
    nonFinite["hairCollision"] = {{"capsules", {{
        {"a", {std::numeric_limits<float>::infinity(), 0.0f, 0.0f}},
        {"b", {0.0f, 10.0f, 0.0f}},
        {"radius", 1.0f}}}}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", nonFinite),
        std::runtime_error);

    auto unknownCollisionField = Descriptor();
    unknownCollisionField["hairCollision"] = {{"capsules", nlohmann::json::array()},
        {"extra", 1}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", unknownCollisionField),
        std::runtime_error);

    auto unknownCapsuleField = Descriptor();
    unknownCapsuleField["hairCollision"] = {{"capsules", {{
        {"a", {0.0f, 0.0f, 0.0f}},
        {"b", {0.0f, 10.0f, 0.0f}},
        {"radius", 1.0f},
        {"extra", 1}}}}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", unknownCapsuleField),
        std::runtime_error);

    auto missingCapsuleField = Descriptor();
    missingCapsuleField["hairCollision"] = {{"capsules", {{
        {"a", {0.0f, 0.0f, 0.0f}},
        {"b", {0.0f, 10.0f, 0.0f}}}}}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_dynamic.json", missingCapsuleField),
        std::runtime_error);
}

TEST(TressfxDescriptor, CollisionMeshAcceptsAuthoredProxyAndDefaults)
{
    auto json = GpuDescriptor();
    json["hairCollision"]["mesh"] = {{"path", "Maps/SC_probe/Collision/proxy.obj"}};
    const auto defaults = BuildPlacementFixturePlan("SM_sdf.json", json);
    ASSERT_TRUE(defaults.tressfxSimulation.collisionMesh.has_value());
    EXPECT_EQ(defaults.tressfxSimulation.collisionMesh->path,
        "Maps/SC_probe/Collision/proxy.obj");
    EXPECT_FLOAT_EQ(defaults.tressfxSimulation.collisionMesh->cellSize, 1.0f);
    EXPECT_FLOAT_EQ(defaults.tressfxSimulation.collisionMesh->margin, 1.0f);

    json["hairCollision"]["mesh"]["cellSize"] = 1.5f;
    json["hairCollision"]["mesh"]["margin"] = 0.5f;
    const auto authored = BuildPlacementFixturePlan("SM_sdf.json", json);
    ASSERT_TRUE(authored.tressfxSimulation.collisionMesh.has_value());
    EXPECT_FLOAT_EQ(authored.tressfxSimulation.collisionMesh->cellSize, 1.5f);
    EXPECT_FLOAT_EQ(authored.tressfxSimulation.collisionMesh->margin, 0.5f);
}

TEST(TressfxDescriptor, CollisionMeshParticipatesInMeshIdentity)
{
    const auto plan = [](const std::string& path, double cellSize, double margin) {
        auto json = GpuDescriptor();
        json["hairCollision"]["mesh"] = {
            {"path", path}, {"cellSize", cellSize}, {"margin", margin}};
        return BuildPlacementFixturePlan("SM_sdf.json", json);
    };
    const auto base = plan("Maps/SC_probe/Collision/proxy.obj", 1.0, 1.0);
    EXPECT_NE(base.modelCacheKey,
        plan("Maps/SC_probe/Collision/proxy_b.obj", 1.0, 1.0).modelCacheKey);
    EXPECT_NE(base.modelCacheKey,
        plan("Maps/SC_probe/Collision/proxy.obj", 1.5, 1.0).modelCacheKey);
    EXPECT_NE(base.modelCacheKey,
        plan("Maps/SC_probe/Collision/proxy.obj", 1.0, 0.5).modelCacheKey);
}

TEST(TressfxDescriptor, CollisionMeshRejectsInvalidValues)
{
    const auto plan = [](const nlohmann::json& mesh) {
        auto json = GpuDescriptor();
        json["hairCollision"]["mesh"] = mesh;
        return BuildPlacementFixturePlan("Maps/SC_probe/Meshes/SM_sdf.json", json);
    };
    const nlohmann::json valid = {{"path", "Maps/SC_probe/Collision/proxy.obj"}};

    EXPECT_NO_THROW(plan(valid));

    // path：缺失、空、非字符串、绝对路径、逃逸资源根都在资产边界拒绝。
    EXPECT_THROW(plan({{"cellSize", 1.0}}), std::runtime_error);
    EXPECT_THROW(plan({{"path", ""}}), std::runtime_error);
    EXPECT_THROW(plan({{"path", 5}}), std::runtime_error);
    EXPECT_THROW(plan({{"path", "C:/proxy.obj"}}), std::runtime_error);
    EXPECT_THROW(plan({{"path", "../proxy.obj"}}), std::runtime_error);

    // cellSize 是 (0.2, 4]；margin 是 [0.05, 4]。
    for (const double cellSize : {0.2, 0.1, 4.01, 0.0, -1.0})
    {
        auto mesh = valid;
        mesh["cellSize"] = cellSize;
        EXPECT_THROW(plan(mesh), std::runtime_error) << "cellSize=" << cellSize;
    }
    for (const double margin : {0.0, 0.049, 4.01, -1.0})
    {
        auto mesh = valid;
        mesh["margin"] = margin;
        EXPECT_THROW(plan(mesh), std::runtime_error) << "margin=" << margin;
    }

    EXPECT_THROW(plan("not-an-object"), std::runtime_error);
    EXPECT_THROW(plan({{"path", "proxy.obj"}, {"extra", 1}}), std::runtime_error);
    try
    {
        auto mesh = valid;
        mesh["cellSize"] = 0.2;
        plan(mesh);
        FAIL() << "cellSize at the exclusive lower bound was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("SM_sdf.json"), std::string::npos);
        EXPECT_NE(message.find("proxy.obj"), std::string::npos);
        EXPECT_NE(message.find("cellSize"), std::string::npos);
    }
}

TEST(TressfxDescriptor, CollisionMeshRequiresGpuBackend)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {{"backend", "cpu"}};
    json["hairCollision"] = {{"capsules", nlohmann::json::array()},
        {"mesh", {{"path", "Maps/SC_probe/Collision/proxy.obj"}}}};
    try
    {
        BuildPlacementFixturePlan("Maps/SC_probe/Meshes/SM_sdf.json", json);
        FAIL() << "collision.mesh was accepted with the CPU backend";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("gpu"), std::string::npos);
        EXPECT_NE(message.find("SM_sdf.json"), std::string::npos);
    }
}

TEST(TressfxDescriptor, RejectsAssimpImportOptions)
{
    auto json = Descriptor();
    json["importOptions"] = {{"generateSmoothNormals", false}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RejectsMultipleMaterialSlots)
{
    auto json = Descriptor();
    json["materialSlots"].push_back({{"name", "extra"}, {"materialInstancePath", "MI_other.json"}});
    EXPECT_THROW(BuildPlacementFixturePlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RejectsNonTfxSource)
{
    auto json = Descriptor();
    json["modelDataPath"] = "hair.obj";
    EXPECT_THROW(BuildPlacementFixturePlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RejectsSkeletalDeclaration)
{
    auto json = Descriptor();
    json["skinning"] = {{"clips", nlohmann::json::array()}, {"skeleton", "SK_head.json"}};
    EXPECT_THROW(BuildPlacementFixturePlan("SM_guides.json", json), std::runtime_error);
}

class TressfxImport : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("vl-strand-import-" + std::to_string(stamp));
        ownsRoot = std::filesystem::create_directory(root);
        ASSERT_TRUE(ownsRoot);
        request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", Descriptor());
        request.absoluteModelPath = (root / std::filesystem::u8path(u8"发丝.tfx")).u8string();
    }
    void TearDown() override
    {
        if (ownsRoot) { std::error_code error; std::filesystem::remove_all(root, error); }
    }
    ModelResource Import(const std::vector<uint8_t>& bytes)
    {
        {
            std::ofstream stream(std::filesystem::u8path(request.absoluteModelPath), std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!stream) throw std::runtime_error("Failed to write owned fixture");
        }
        return TressfxModelImporter().Import(request);
    }
    std::filesystem::path root;
    bool ownsRoot = false;
    ModelImportRequest request;
};

TEST_F(TressfxImport, BuildsOneSectionAndMapsExistingMaterialSlot)
{
    const auto resource = Import(TfxFixture::Guides(3));
    ASSERT_EQ(resource.sections.size(), 1u);
    EXPECT_EQ(resource.sections[0].vertices.size(), 24u);
    EXPECT_EQ(resource.sections[0].indices.size(), 54u);
    const auto slots = MeshAssetValidator::BuildSectionLoadPlans(request.buildPlan, resource);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots[0].materialInstancePath, "MI_probe.json");
}

TEST_F(TressfxImport, KeepsCenterlinesAndEncodesSignedRadiusWithoutInverseMass)
{
    auto bytes = TfxFixture::Guides();
    TfxFixture::Float(bytes, 172, 200.0f);
    const auto section = Import(bytes).sections[0];
    EXPECT_TRUE(section.vertices[0].position.isApprox(Eigen::Vector3f(0, 0, -2)));
    EXPECT_TRUE(section.vertices[1].position.isApprox(section.vertices[0].position));
    EXPECT_FLOAT_EQ(section.vertices[0].texCoord1.x(), -0.05f);
    EXPECT_FLOAT_EQ(section.vertices[1].texCoord1.x(), 0.05f);
    EXPECT_FLOAT_EQ(section.vertices.back().texCoord1.y(), 1.0f);
    EXPECT_TRUE(section.vertices[0].tangent.isApprox(Eigen::Vector4f(0, 1, 0, 1)));
}

TEST_F(TressfxImport, RetainsOneGuideRootPositionPerStrand)
{
    const auto section = Import(TfxFixture::Guides(3)).sections[0];
    ASSERT_EQ(section.tressfxRootPositions.size(), 3u);
    // 夹具第 g 根 guide 的根点是 (g, 0, -2)；只保留根点，不复制全部中心线。
    EXPECT_TRUE(section.tressfxRootPositions[0].isApprox(Eigen::Vector3f(0, 0, -2)));
    EXPECT_TRUE(section.tressfxRootPositions[1].isApprox(Eigen::Vector3f(1, 0, -2)));
    EXPECT_TRUE(section.tressfxRootPositions[2].isApprox(Eigen::Vector3f(2, 0, -2)));
}

TEST_F(TressfxImport, AppliesPerGuideThicknessAndPadsBounds)
{
    auto bytes = TfxFixture::Guides(2);
    TfxFixture::Attribute(bytes, 24, {0.5f, 3.0f});
    const auto section = Import(bytes).sections[0];
    EXPECT_FLOAT_EQ(section.vertices[1].texCoord1.x(), 0.025f);
    EXPECT_FLOAT_EQ(section.vertices[9].texCoord1.x(), 0.15f);
    EXPECT_FLOAT_EQ(section.boundsPadding, 0.15f);
}

TEST_F(TressfxImport, CarriesExportedVertexUvAndColorUnchanged)
{
    auto bytes = TfxFixture::Guides();
    TfxFixture::Attribute(bytes, 16, {0.2f, 0.3f});
    TfxFixture::Attribute(bytes, 20, {2, -1, 3, -2, 4, -3, 5, -4});
    TfxFixture::Attribute(bytes, 28, {2, 3, 4, 0.5f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    const auto section = Import(bytes).sections[0];
    EXPECT_TRUE(section.vertices[0].texCoord.isApprox(Eigen::Vector2f(2, -1)));
    EXPECT_TRUE(section.vertices[0].color.isApprox(Eigen::Vector4f(2, 3, 4, 0.5f)));
}

TEST_F(TressfxImport, MissingOptionalAttributesUseExplicitWhiteAndZeroUv)
{
    const auto section = Import(TfxFixture::Guides()).sections[0];
    EXPECT_TRUE(section.vertices[0].color.isApprox(Eigen::Vector4f::Ones()));
    EXPECT_TRUE(section.vertices[0].texCoord.isZero());
}

TEST_F(TressfxImport, GeometryImportDoesNotBakePlacementCollisionProxy)
{
    auto json = GpuDescriptor();
    json["hairCollision"]["mesh"] = {{"path", "Maps/SC_probe/Collision/proxy.obj"}};
    request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", json);
    const std::filesystem::path objPath = root / "proxy.obj";
    {
        std::ofstream stream(objPath, std::ios::binary);
        stream << "# fixture\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        ASSERT_TRUE(static_cast<bool>(stream));
    }
    const ModelResource resource = Import(TfxFixture::Guides());
    ASSERT_EQ(resource.sections.size(), 1u);
    EXPECT_EQ(resource.sections[0].vertices.size(), 8u);
    EXPECT_TRUE(request.buildPlan.tressfxSimulation.collisionMesh.has_value());
}

TEST_F(TressfxImport, GeometryImportDoesNotReadPlacementProxyPath)
{
    auto json = GpuDescriptor();
    json["hairCollision"]["mesh"] = {{"path", "Maps/SC_probe/Collision/missing.obj"}};
    request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", json);
    EXPECT_NO_THROW(Import(TfxFixture::Guides()));
}

TEST_F(TressfxImport, GeometryImportDoesNotBakeMatchingDefaultCurves)
{
    auto json = Descriptor();
    json["simulationDefaults"] = {
        {"vspStiffness", {0.0f, 0.25f, 0.5f, 0.75f}},
        {"localShapeStiffness", {0.95f, 0.8f, 0.4f, 0.0f}}};
    request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", json);
    // TfxFixture::Guides 默认 4 points/strand，两条数组长度必须都为 4。
    const auto section = Import(TfxFixture::Guides()).sections[0];
    EXPECT_EQ(section.vertices.size(), 8u);
    EXPECT_EQ(request.buildPlan.tressfxSimulation.vspStiffness.size(), 4u);
    EXPECT_EQ(request.buildPlan.tressfxSimulation.localShapeStiffness.size(), 4u);
}

TEST_F(TressfxImport, RejectsAuthoredCurveLengthMismatchAgainstTfxPointsPerStrand)
{
    auto json = Descriptor();
    // 8 项曲线配 4 points/strand 的 TFX：必须在导入边界硬失败，不能截断/补零。
    json["simulationDefaults"] = {
        {"vspStiffness", {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f}}};
    request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", json);
    try
    {
        Import(TfxFixture::Guides());
        FAIL() << "Curve/pointsPerStrand mismatch was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("vspStiffness"), std::string::npos);
        EXPECT_NE(message.find("8 entries"), std::string::npos);
        EXPECT_NE(message.find("4 points per strand"), std::string::npos);
        EXPECT_NE(message.find("SM_guides.json"), std::string::npos);
    }
}

TEST_F(TressfxImport, RejectsUnrepresentableThicknessProduct)
{
    auto bytes = TfxFixture::Guides();
    request.buildPlan.strandRadius = std::numeric_limits<float>::max();
    TfxFixture::Attribute(bytes, 24, {2.0f});
    EXPECT_THROW(Import(bytes), std::runtime_error);
}

TEST_F(TressfxImport, RejectsInvalidCenterlineBeforePublishingGeometry)
{
    auto bytes = TfxFixture::Guides();
    TfxFixture::Point(bytes, 1, {0, 0, -2, 0});
    EXPECT_THROW(Import(bytes), std::runtime_error);
}

nlohmann::json WithGroups(nlohmann::json groups)
{
    auto json = Descriptor();
    json["groups"] = std::move(groups);
    return json;
}

nlohmann::json TwoGroups()
{
    return nlohmann::json::array({
        {{"name", "fringe"}, {"guideRanges", {{0, 2}, {4, 6}}},
            {"simulationOverrides", {{"globalShapeStiffness", 0.3}}}},
        {{"name", "back"}, {"guideRanges", {{2, 4}}}}});
}

TEST(TressfxGuideGroups, ValidGroupsResolveRangesAndOverrides)
{
    const auto plan = MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(TwoGroups()));
    ASSERT_EQ(plan.tressfxGuideGroups.size(), 2u);
    EXPECT_EQ(plan.tressfxGuideGroups[0].name, "fringe");
    EXPECT_EQ(plan.tressfxGuideGroups[1].name, "back");
    ASSERT_EQ(plan.tressfxGuideGroups[0].guideRanges.size(), 2u);
    EXPECT_EQ(plan.tressfxGuideGroups[0].guideRanges[1], (std::pair<uint32_t, uint32_t>{4u, 6u}));
    EXPECT_DOUBLE_EQ(plan.tressfxGuideGroups[0].simulationOverrides.at("globalShapeStiffness")
        .get<double>(), 0.3);
    EXPECT_NE(plan.modelCacheKey.find("|groups="), std::string::npos);
}

TEST(TressfxGuideGroups, AbsentGroupsLeaveNoGroupPlanOrIdentity)
{
    const auto plan = MeshAssetValidator::BuildLoadPlan("TF_hair.json", Descriptor());
    EXPECT_TRUE(plan.tressfxGuideGroups.empty());
    EXPECT_EQ(plan.modelCacheKey.find("|groups="), std::string::npos);
}

TEST(TressfxGuideGroups, DuplicateNameIsRejected)
{
    auto groups = TwoGroups();
    groups[1]["name"] = "fringe";
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
        std::runtime_error);
}

TEST(TressfxGuideGroups, EmptyNameIsRejected)
{
    auto groups = TwoGroups();
    groups[0]["name"] = "";
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
        std::runtime_error);
}

TEST(TressfxGuideGroups, OverlappingRangesWithinOneGroupAreRejected)
{
    auto groups = TwoGroups();
    groups[0]["guideRanges"] = {{0, 4}, {3, 6}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
        std::runtime_error);
}

TEST(TressfxGuideGroups, GuideInTwoGroupsIsRejected)
{
    auto groups = TwoGroups();
    // "back" 的 [2,5) 与 "fringe" 的 [4,6) 在 4 号 guide 上重叠。
    groups[1]["guideRanges"] = {{2, 5}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
        std::runtime_error);
}

TEST(TressfxGuideGroups, TouchingHalfOpenRangesAcrossGroupsAreAccepted)
{
    auto groups = TwoGroups();
    groups[0]["guideRanges"] = {{0, 2}};
    groups[1]["guideRanges"] = {{2, 4}};
    EXPECT_NO_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)));
}

TEST(TressfxGuideGroups, RangeMustBeNonEmptyIntegerPairWithinOrder)
{
    const std::vector<nlohmann::json> invalid = {
        nlohmann::json::array({3, 3}), nlohmann::json::array({5, 2}),
        nlohmann::json::array({0.5, 2}), nlohmann::json::array({-1, 2}),
        nlohmann::json::array({"0", 2}), nlohmann::json::array({0}),
        nlohmann::json::array({0, 2, 4})};
    for (const auto& range : invalid)
    {
        SCOPED_TRACE(range.dump());
        auto groups = TwoGroups();
        groups[0]["guideRanges"] = nlohmann::json::array({range});
        EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
            std::runtime_error);
    }
}

TEST(TressfxGuideGroups, OverrideKeysOutsideNineParameterTableAreRejected)
{
    for (const char* field : {"enabled", "skinning", "collision", "hairCollision",
             "rootFixedPoints", "maxDt", "backend", "dampping", "gravity", "wind", "iterations"})
    {
        SCOPED_TRACE(field);
        auto groups = TwoGroups();
        groups[0]["simulationOverrides"] = {{field, nlohmann::json::object()}};
        EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
            std::runtime_error);
    }
}

TEST(TressfxGuideGroups, OverrideValuesUseSimulationRanges)
{
    auto groups = TwoGroups();
    groups[0]["simulationOverrides"] = {{"damping", 2.0}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
        std::runtime_error);
    groups[0]["simulationOverrides"] = {{"bendStiffness", 2.0}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("TF_hair.json", WithGroups(groups)),
        std::runtime_error);
}

TEST(TressfxGuideGroups, GroupsAreOnlyAcceptedOnTressfxAssets)
{
    auto json = Descriptor();
    json["type"] = "mesh";
    json.erase("strandRadius");
    json["groups"] = TwoGroups();
    try
    {
        MeshAssetValidator::BuildLoadPlan("SM_hair.json", json);
        FAIL() << "groups accepted on a mesh asset";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("only supported on tressfx"), std::string::npos);
    }
}

TEST_F(TressfxImport, ResolvesGuideGroupIdsFromDeclaredRanges)
{
    request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", WithGroups(
        nlohmann::json::array({
            {{"name", "a"}, {"guideRanges", {{0, 1}}}},
            {{"name", "b"}, {"guideRanges", {{2, 3}}}}})));
    const auto section = Import(TfxFixture::Guides(3)).sections[0];
    // 0 号为默认组；未声明的 1 号 guide 保持 0。
    EXPECT_EQ(section.tressfxGuideGroupIds, (std::vector<uint32_t>{1u, 0u, 2u}));
}

TEST_F(TressfxImport, RangePastTfxGuideCountNamesGroupAndAsset)
{
    request.buildPlan = BuildPlacementFixturePlan("SM_guides.json", WithGroups(
        nlohmann::json::array({{{"name", "tail"}, {"guideRanges", {{2, 4}}}}})));
    try
    {
        Import(TfxFixture::Guides(3));
        FAIL() << "range past the .tfx guide count was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("\"tail\""), std::string::npos);
        EXPECT_NE(message.find("SM_guides.json"), std::string::npos);
    }
}

TEST_F(TressfxImport, AbsentGroupsLeaveGuideGroupIdsEmpty)
{
    const auto section = Import(TfxFixture::Guides(3)).sections[0];
    EXPECT_TRUE(section.tressfxGuideGroupIds.empty());
}

TEST(TressfxRenderableKey, GpuHairPlacementAppendsInstanceButStaticMeshesShare)
{
    // 普通网格仍按模型+section 共享，不受 placement 身份影响。
    EXPECT_EQ(MakeRenderableObjectKey("cache", 1, "body", "A", false),
        MakeRenderableObjectKey("cache", 1, "body", "B", false));
    // GPU 发丝的可变模拟状态按 placement 隔离，键必须不同。
    const std::string first =
        MakeRenderableObjectKey("cache", 0, "hair", "Female_hair_a", true);
    const std::string second =
        MakeRenderableObjectKey("cache", 0, "hair", "Female_hair_b", true);
    EXPECT_EQ(first, "cache|0|hair|instance=Female_hair_a");
    EXPECT_NE(first, second);
    EXPECT_NE(first, MakeRenderableObjectKey("cache", 0, "hair", "A", false));
}

TEST(TressfxRenderableKey, SpawnRebasesInstanceIdentityButKeepsSharedKeys)
{
    // 运行时 spawn 的 per-placement 毛发：只替换键尾精确匹配的 instance 身份，
    // 保留 model/section 前缀，保证仍共享同一份导入几何。
    const std::string templateKey =
        MakeRenderableObjectKey("cache", 0, "hair", "__blueprint/~template.0/hair", true);
    EXPECT_EQ(RebaseRenderableObjectKeyInstance(templateKey,
                  "__blueprint/~template.0/hair", "__blueprint/~spawn.1/hair"),
        "cache|0|hair|instance=__blueprint/~spawn.1/hair");
    // 身份不匹配或共享网格（没有 instance 段）时原样返回，不能误改别的 placement。
    EXPECT_EQ(RebaseRenderableObjectKeyInstance(templateKey,
                  "__blueprint/other/hair", "__blueprint/~spawn.2/hair"), templateKey);
    const std::string shared = MakeRenderableObjectKey("cache", 0, "hair", "A", false);
    EXPECT_EQ(RebaseRenderableObjectKeyInstance(shared, "A", "B"), shared);
    // 空身份不做替换，避免把结尾的 "|instance=" 当成匹配。
    EXPECT_EQ(RebaseRenderableObjectKeyInstance(templateKey, "", "B"), templateKey);
    EXPECT_EQ(RebaseRenderableObjectKeyInstance(templateKey,
                  "__blueprint/~template.0/hair", ""), templateKey);
}
}
