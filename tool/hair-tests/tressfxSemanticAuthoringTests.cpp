#include "hair/tressfxBehavior.h"
#include "hair/tressfxGroupSimulation.h"
#include "hair/tressfxLiveParamsApply.h"

#include <gtest/gtest.h>

namespace
{
using nlohmann::json;
using namespace VL::Hair;

TressfxSimulationConfig ReadDefaults(const json& values)
{
    auto authored = values;
    if (authored.value("solver", json()) == "cosseratXpbdV1" && !authored.contains("backend"))
        authored["backend"] = "gpu";
    return ReadTressfxSimulationDefaults({{"simulationDefaults", authored}}, "semantic test");
}

TEST(TressfxSemanticAuthoring, LegacyDefaultsRemainTressfx)
{
    const auto config = ReadDefaults({{"bendStiffness", 0.4}, {"backend", "gpu"}});
    EXPECT_EQ(config.solver, TressfxSimulationSolver::Tressfx);
    EXPECT_FLOAT_EQ(config.bendStiffness, 0.4f);
    EXPECT_TRUE(config.tressfxRawParametersAuthored);
    EXPECT_FALSE(WriteTressfxSimulationValues(config).contains("solverProfile"));
}

TEST(TressfxSemanticAuthoring, RodRoundTripOmitsRawParameters)
{
    const auto config = ReadDefaults({{"solver", "cosseratXpbdV1"}, {"backend", "gpu"}});
    const auto written = WriteTressfxSimulationValues(config);
    EXPECT_EQ(written.at("solverProfile"), "effectiveBundleV1");
    for (auto field = written.begin(); field != written.end(); ++field)
        EXPECT_FALSE(IsTressfxRawSimulationField(field.key()));
    const auto restored = ReadDefaults(written);
    EXPECT_EQ(restored.solver, TressfxSimulationSolver::CosseratXpbdV1);
    EXPECT_EQ(restored.solverProfile, TressfxSolverProfile::EffectiveBundleV1);
    EXPECT_EQ(MakeTressfxBehaviorKey(config), MakeTressfxBehaviorKey(restored));
}

TEST(TressfxSemanticAuthoring, RejectsUnknownSolverProfileAndWrongKinds)
{
    for (const auto& values : {
             json{{"solver", "unknown"}}, json{{"solver", 1}},
             json{{"solver", "rod"}},
             json{{"solver", "cosseratXpbdV1"}, {"solverProfile", "stronghold"}},
             json{{"solver", "cosseratXpbdV1"}, {"solverProfile", false}},
             json{{"solver", "cosseratXpbdV1"}, {"unknown", 1}},
             json{{"solver", "tressfx"}, {"solverProfile", "effectiveBundleV1"}},
             json{{"solverProfile", "effectiveBundleV1"}}})
        EXPECT_THROW(ReadDefaults(values), std::exception);
}

TEST(TressfxSemanticAuthoring, EveryRawFieldIsRejectedByRod)
{
    const auto baseline = WriteTressfxSimulationValues(TressfxSimulationConfig{});
    for (auto field = baseline.begin(); field != baseline.end(); ++field)
    {
        if (!IsTressfxRawSimulationField(field.key()))
            continue;
        EXPECT_THROW(ReadDefaults({{"solver", "cosseratXpbdV1"}, {field.key(), field.value()}}),
            std::exception) << field.key();
    }
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratXpbdV1"}, {"vspStiffness", {0.5}}}),
        std::exception);
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratXpbdV1"}, {"localShapeStiffness", {0.5}}}),
        std::exception);
}

TEST(TressfxSemanticAuthoring, PlacementAndGroupUseResolvedSolver)
{
    const auto base = ReadDefaults({{"solver", "cosseratXpbdV1"}});
    auto placement = ReadTressfxPlacementBehavior(
        {{"simulation", {{"overrides", {{"solverProfile", "effectiveBundleV1"}}}}}}, "test");
    TressfxGuideGroupPlan group;
    group.name = "fringe";
    group.simulationOverrides = {{"solverProfile", "effectiveBundleV1"}};
    const auto configs = ResolveTressfxGroupSimulationConfigs(base, {group}, placement);
    ASSERT_EQ(configs.size(), 2u);
    EXPECT_EQ(configs[1].solver, TressfxSimulationSolver::CosseratXpbdV1);
    group.simulationOverrides = {{"bendStiffness", 0.2}};
    EXPECT_THROW(ResolveTressfxGroupSimulationConfigs(base, {group}, placement),
        std::exception);
    placement.overrides = {{"damping", 0.2}};
    EXPECT_THROW(ResolveTressfxBehavior(base, placement), std::exception);
    placement.overrides = {{"solverProfile", "effectiveBundleV1"}};
    EXPECT_THROW(ResolveTressfxBehavior(TressfxSimulationConfig{}, placement),
        std::exception);
}

TEST(TressfxSemanticAuthoring, SolverSwitchRejectsAuthoredLegacyDefaults)
{
    TressfxPlacementBehavior placement;
    placement.overrides = {{"solver", "cosseratXpbdV1"}, {"backend", "gpu"}};
    EXPECT_EQ(ResolveTressfxBehavior(TressfxSimulationConfig{}, placement).solver,
        TressfxSimulationSolver::CosseratXpbdV1);
    const auto authored = ReadDefaults({{"bendStiffness", 0.3}});
    EXPECT_THROW(ResolveTressfxBehavior(authored, placement), std::exception);
    TressfxGuideGroupPlan group;
    group.name = "fringe";
    group.simulationOverrides = {{"bendStiffness", 0.3}};
    EXPECT_THROW(ResolveTressfxGroupSimulationConfigs(
        TressfxSimulationConfig{}, {group}, placement), std::exception);
}

TEST(TressfxSemanticAuthoring, RodCpuRuntimeIsRejectedInsteadOfUsingTressfxReference)
{
    EXPECT_THROW(ReadTressfxSimulationDefaults(
        {{"simulationDefaults", {{"solver", "cosseratXpbdV1"}, {"backend", "cpu"}}}},
        "test"), std::exception);
    EXPECT_THROW(ReadTressfxSimulationDefaults(
        {{"simulationDefaults", {{"solver", "cosseratXpbdV1"}}}}, "test"), std::exception);
    auto base = ReadDefaults({{"solver", "cosseratXpbdV1"}});
    TressfxPlacementBehavior placement;
    placement.overrides = {{"backend", "cpu"}};
    EXPECT_THROW(ResolveTressfxBehavior(base, placement), std::exception);
}

TEST(TressfxSemanticAuthoring, LiveRawEditsAreAtomicAndProfileRequiresReload)
{
    auto config = ReadDefaults({{"solver", "cosseratXpbdV1"}});
    const auto before = MakeTressfxBehaviorKey(config);
    HairLiveParameterRequest request;
    request.objectName = "hair";
    request.field = "bendStiffness";
    request.valueCount = 1;
    request.values[0] = 0.5f;
    std::string error;
    EXPECT_FALSE(ApplyHairLiveParameter(config, request, error));
    EXPECT_NE(error.find("rod"), std::string::npos);
    EXPECT_EQ(before, MakeTressfxBehaviorKey(config));
    EXPECT_TRUE(IsHairReloadOnlyField("solver"));
    EXPECT_TRUE(IsHairReloadOnlyField("solverProfile"));
    request.field = "gravity";
    request.valueCount = 3;
    request.values[0] = 0;
    request.values[1] = -500;
    request.values[2] = 0;
    EXPECT_TRUE(ApplyHairLiveParameter(config, request, error));
    EXPECT_FLOAT_EQ(config.gravity.y(), -500);
}

TEST(TressfxSemanticAuthoring, ImplicitV3RoundTripAndMathIdentityDifferFromXpbdV1)
{
    const auto v3 = ReadDefaults({
        {"solver", "cosseratImplicitV3"}, {"backend", "gpu"},
        {"solverProfile", "effectiveBundleV1"}});
    EXPECT_EQ(v3.solver, TressfxSimulationSolver::CosseratImplicitV3);
    EXPECT_TRUE(IsRodSimulationSolver(v3.solver));
    const auto written = WriteTressfxSimulationValues(v3);
    EXPECT_EQ(written.at("solver"), "cosseratImplicitV3");
    EXPECT_FALSE(written.contains("bendStiffness"));
    EXPECT_FALSE(written.contains("algorithmVersion"));
    EXPECT_EQ(ReadDefaults(written).solver, v3.solver);
    const auto v1 = ReadDefaults({{"solver", "cosseratXpbdV1"}, {"backend", "gpu"}});
    EXPECT_NE(MakeTressfxBehaviorKey(v3), MakeTressfxBehaviorKey(v1));
    const auto identity = json::parse(MakeTressfxBehaviorKey(v3));
    EXPECT_EQ(identity.at("algorithmVersion"), 3);
    EXPECT_EQ(identity.at("profileMathVersion"), 3);
    EXPECT_EQ(TressfxSolverAlgorithmVersion(v1.solver), 1u);
    EXPECT_NE(MakeTressfxGroupSimulationKey({v3, v3}),
        MakeTressfxGroupSimulationKey({v1, v1}));
}

TEST(TressfxSemanticAuthoring, ImplicitV3RejectsAllRawParametersAndCpuRuntime)
{
    const auto legacy = WriteTressfxSimulationValues(TressfxSimulationConfig{});
    for (auto field = legacy.begin(); field != legacy.end(); ++field)
    {
        if (!IsTressfxRawSimulationField(field.key()))
            continue;
        EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV3"}, {"backend", "gpu"},
            {field.key(), field.value()}}), std::exception) << field.key();
    }
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV3"},
        {"backend", "gpu"}, {"localShapeStiffness", {0.2}}}), std::exception);
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV3"},
        {"backend", "gpu"}, {"vspStiffness", {0.2}}}), std::exception);
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV3"}}), std::exception);
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV3"}, {"backend", "cpu"}}),
        std::exception);
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV3"}, {"backend", "gpu"},
        {"solverProfile", "unknown"}}), std::exception);
    EXPECT_THROW(ReadDefaults({{"solver", "cosseratImplicitV2"}, {"backend", "gpu"}}),
        std::exception);
}

TEST(TressfxSemanticAuthoring, ImplicitV3PlacementGroupsAndLiveRulesStaySemantic)
{
    const auto v3 = ReadDefaults({{"solver", "cosseratImplicitV3"}, {"backend", "gpu"}});
    auto placement = ReadTressfxPlacementBehavior(
        {{"simulation", {{"overrides", {{"solverProfile", "effectiveBundleV1"}}}}}}, "test");
    TressfxGuideGroupPlan group;
    group.name = "fringe";
    group.simulationOverrides = {{"solverProfile", "effectiveBundleV1"}};
    const auto configs = ResolveTressfxGroupSimulationConfigs(v3, {group}, placement);
    ASSERT_EQ(configs.size(), 2u);
    EXPECT_EQ(configs[1].solver, TressfxSimulationSolver::CosseratImplicitV3);
    group.simulationOverrides = {{"bendStiffness", 0.2}};
    EXPECT_THROW(ResolveTressfxGroupSimulationConfigs(v3, {group}, placement), std::exception);
    placement.overrides = {{"solver", "cosseratImplicitV3"}, {"backend", "gpu"}};
    EXPECT_THROW(ResolveTressfxBehavior(ReadDefaults({{"bendStiffness", 0.2}}), placement),
        std::exception);
    auto candidate = v3;
    const auto before = MakeTressfxBehaviorKey(candidate);
    HairLiveParameterRequest request;
    request.field = "bendStiffness";
    request.valueCount = 1;
    request.values[0] = 0.4f;
    std::string error;
    EXPECT_FALSE(ApplyHairLiveParameter(candidate, request, error));
    EXPECT_EQ(MakeTressfxBehaviorKey(candidate), before);
}
}
