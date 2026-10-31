#include "hair/tressfxDynamics.h"
#include "hair/tressfxGroupSimulation.h"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using VL::Hair::GuideSimulation;
using VL::Hair::SimulationConfig;
using VL::Hair::TressfxPlacementBehavior;

TressfxSimulationConfig BaseConfig()
{
    TressfxSimulationConfig config;
    config.enabled = true;
    config.gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    config.damping = 0.10f;
    config.bendStiffness = 0.30f;
    config.globalShapeStiffness = 0.0f;
    config.globalShapeEffectiveRange = 0.0f;
    config.vspCoefficient = 0.0f;
    config.vspAccelThresholdMax = 140.0f;
    return config;
}

TressfxGuideGroupPlan Group(std::string name, double bend,
    double globalShape = 0.0)
{
    TressfxGuideGroupPlan group;
    group.name = std::move(name);
    group.guideRanges = {{0, 4}};
    group.simulationOverrides = {
        {"bendStiffness", bend},
        {"globalShapeStiffness", globalShape}};
    return group;
}

std::vector<Eigen::Vector3f> TwoGuideChain()
{
    std::vector<Eigen::Vector3f> rest;
    for (int guide = 0; guide < 2; ++guide)
        for (int local = 0; local < 4; ++local)
            rest.emplace_back(guide * 10.0f, static_cast<float>(local), 0.0f);
    return rest;
}

TEST(TressfxGroupSimulation, PlacementAlwaysWinsOverGroupOverrides)
{
    const TressfxSimulationConfig base = BaseConfig();
    std::vector<TressfxGuideGroupPlan> groups{
        Group("fringe", 0.80)};
    TressfxPlacementBehavior placement;
    placement.overrides = {{"damping", 0.25}, {"bendStiffness", 0.95}};

    const auto configs = VL::Hair::ResolveTressfxGroupSimulationConfigs(
        base, groups, placement, "test");
    ASSERT_EQ(configs.size(), 2u);
    // 索引 0 只有 base + placement；命名组的 bend 被 placement 最后覆盖。
    EXPECT_FLOAT_EQ(configs[0].damping, 0.25f);
    EXPECT_FLOAT_EQ(configs[0].bendStiffness, 0.95f);
    EXPECT_FLOAT_EQ(configs[1].damping, 0.25f);
    EXPECT_FLOAT_EQ(configs[1].bendStiffness, 0.95f);
    EXPECT_FLOAT_EQ(configs[1].globalShapeStiffness, 0.0f);
}

TEST(TressfxGroupSimulation, GroupOverrideAppliesWhenPlacementIsSilent)
{
    const TressfxSimulationConfig base = BaseConfig();
    std::vector<TressfxGuideGroupPlan> groups{
        Group("fringe", 0.80, 0.40)};
    TressfxPlacementBehavior placement;

    const auto configs = VL::Hair::ResolveTressfxGroupSimulationConfigs(
        base, groups, placement, "test");
    ASSERT_EQ(configs.size(), 2u);
    EXPECT_FLOAT_EQ(configs[0].bendStiffness, 0.30f);
    EXPECT_FLOAT_EQ(configs[0].globalShapeStiffness, 0.0f);
    EXPECT_FLOAT_EQ(configs[1].bendStiffness, 0.80f);
    EXPECT_FLOAT_EQ(configs[1].globalShapeStiffness, 0.40f);
    EXPECT_FLOAT_EQ(configs[1].damping, base.damping);
}

TEST(TressfxGroupSimulation, NoGroupsProducesOnlyIndexZero)
{
    TressfxPlacementBehavior placement;
    placement.overrides = {{"vspCoefficient", 0.3}};
    const auto configs = VL::Hair::ResolveTressfxGroupSimulationConfigs(
        BaseConfig(), {}, placement, "test");
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_FLOAT_EQ(configs[0].vspCoefficient, 0.3f);
    EXPECT_TRUE(VL::Hair::MakeTressfxGroupSimulationKey(configs).empty());
}

TEST(TressfxGroupSimulation, RejectsNonGroupOverrideField)
{
    TressfxGuideGroupPlan group;
    group.name = "bad";
    group.guideRanges = {{0, 1}};
    group.simulationOverrides = {{"gravity", {0.0, -1.0, 0.0}}};
    TressfxPlacementBehavior placement;
    EXPECT_THROW(VL::Hair::ResolveTressfxGroupSimulationConfigs(
        BaseConfig(), {group}, placement, "test"), std::runtime_error);
}

TEST(TressfxGroupSimulation, GpuPackingAndGuideSelectionMatchResolvedConfigs)
{
    const TressfxSimulationConfig base = BaseConfig();
    std::vector<TressfxGuideGroupPlan> groups{
        Group("fringe", 0.80, 0.40)};
    groups.push_back(Group("crown", 0.20, 0.10));
    groups[1].simulationOverrides["vspCoefficient"] = 0.5;
    TressfxPlacementBehavior placement;

    const auto configs = VL::Hair::ResolveTressfxGroupSimulationConfigs(
        base, groups, placement, "test");
    const auto gpu = VL::Hair::BuildTressfxGroupSimulationGpuData(configs);
    ASSERT_EQ(gpu.size(), 3u);
    EXPECT_EQ(sizeof(VL::Hair::TressfxGroupSimulationGpu), 32u);
    EXPECT_FALSE(VL::Hair::MakeTressfxGroupSimulationKey(configs).empty());

    // 与 shader 的 TressfxGroupSimulationIndex 相同：组 0/1/2 有效，越界回退 0。
    const uint32_t ids[] = {0u, 1u, 2u, 99u};
    for (uint32_t id : ids)
    {
        const uint32_t index = VL::Hair::SelectTressfxGroupSimulationIndex(
            id, configs.size());
        EXPECT_FLOAT_EQ(gpu[index].values[0], configs[index].damping);
        EXPECT_FLOAT_EQ(gpu[index].values[1], configs[index].bendStiffness);
        EXPECT_FLOAT_EQ(gpu[index].values[2],
            configs[index].globalShapeStiffness);
        EXPECT_FLOAT_EQ(gpu[index].values[3],
            configs[index].globalShapeEffectiveRange);
        EXPECT_FLOAT_EQ(gpu[index].values[4], configs[index].vspCoefficient);
        EXPECT_FLOAT_EQ(gpu[index].values[5],
            configs[index].vspAccelThresholdMax);
    }
    EXPECT_EQ(VL::Hair::SelectTressfxGroupSimulationIndex(2u, configs.size()), 2u);
    EXPECT_EQ(VL::Hair::SelectTressfxGroupSimulationIndex(99u, configs.size()), 0u);
}

TEST(TressfxGroupSimulation, GpuTableCapacityPadsWithResolvedBaseConfig)
{
    TressfxSimulationConfig base = BaseConfig();
    base.damping = 0.03f;
    TressfxSimulationConfig overrideConfig = base;
    overrideConfig.damping = 0.09f;

    const auto packed = VL::Hair::BuildTressfxGroupSimulationGpuData(
        {base, overrideConfig}, 8);
    ASSERT_EQ(packed.size(), 8u);
    EXPECT_FLOAT_EQ(packed[0].values[0], 0.03f);
    EXPECT_FLOAT_EQ(packed[1].values[0], 0.09f);
    for (size_t index = 2; index < packed.size(); ++index)
        EXPECT_FLOAT_EQ(packed[index].values[0], 0.03f)
            << "padding must fall back to the resolved base config";
}

TEST(TressfxGroupSimulation, CpuSolverUsesPerGuideDamping)
{
    SimulationConfig base;
    // 重力垂直于静止链（沿 +X），否则长度约束会沿链轴抵消位移。
    base.gravity = Eigen::Vector3f(980.0f, 0.0f, 0.0f);
    base.damping = 0.0f;
    base.iterations = 1;
    base.rootFixedPoints = 2;
    base.bendStiffness = 0.0f;
    base.globalShapeStiffness = 0.0f;
    base.vspCoefficient = 0.0f;
    SimulationConfig damped = base;
    damped.damping = 0.9f;

    GuideSimulation simulation(TwoGuideChain(), 2, 4, {0, 1, 1, 1, 0, 1, 1, 1},
        {base, damped}, {0u, 1u});
    simulation.Step(1.0f / 60.0f);
    simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    // 同一重力、同一静止链：未阻尼组保留更大速度，第二帧位移更远。
    // 第二根 guide 的静止 x = 10，比较时必须扣掉这个偏移。
    EXPECT_GT(positions[2].x(), positions[6].x() - 10.0f + 1.0e-4f);
    EXPECT_NEAR(positions[1].y(), 1.0f, 1.0e-6f);
    EXPECT_NEAR(positions[5].y(), 1.0f, 1.0e-6f);
}

TEST(TressfxGroupSimulation, CpuSolverUsesPerGuideGlobalShape)
{
    SimulationConfig base;
    base.gravity = Eigen::Vector3f(980.0f, 0.0f, 0.0f);
    base.damping = 0.035f;
    base.iterations = 1;
    base.rootFixedPoints = 2;
    base.bendStiffness = 0.0f;
    base.globalShapeStiffness = 0.0f;
    base.globalShapeEffectiveRange = 1.0f;
    base.vspCoefficient = 0.0f;
    SimulationConfig shaped = base;
    shaped.globalShapeStiffness = 1.0f;

    GuideSimulation simulation(TwoGuideChain(), 2, 4, {0, 1, 1, 1, 0, 1, 1, 1},
        {base, shaped}, {0u, 1u});
    simulation.Step(1.0f / 60.0f);
    simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    // 组 1 的 stiffness=1 把可移动点拉回 InitialPos，位移应小于组 0。
    EXPECT_LT(positions[6].x() - 10.0f, positions[2].x() - 1.0e-4f);
    EXPECT_NEAR(positions[6].x(), 10.0f, 1.0e-5f);
}

TEST(TressfxGroupSimulation, CpuSolverUsesPerGuideBendStiffness)
{
    SimulationConfig base;
    base.gravity = Eigen::Vector3f(980.0f, 0.0f, 0.0f);
    base.damping = 0.035f;
    base.iterations = 1;
    base.rootFixedPoints = 2;
    base.bendStiffness = 0.0f;
    base.globalShapeStiffness = 0.0f;
    base.vspCoefficient = 0.0f;
    SimulationConfig stiff = base;
    stiff.bendStiffness = 0.9f;

    GuideSimulation simulation(TwoGuideChain(), 2, 4, {0, 1, 1, 1, 0, 1, 1, 1},
        {base, stiff}, {0u, 1u});
    simulation.Step(1.0f / 60.0f);
    simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    // LocalShape 把可移动点拉回 rest 段方向；高 bend 组在 +X 重力下位移更小。
    EXPECT_LT(positions[6].x() - 10.0f, positions[2].x() - 1.0e-4f);
}

TEST(TressfxGroupSimulation, CpuSolverUsesPerGuideGlobalShapeRange)
{
    SimulationConfig base;
    base.gravity = Eigen::Vector3f(980.0f, 0.0f, 0.0f);
    base.damping = 0.035f;
    base.iterations = 1;
    base.rootFixedPoints = 2;
    base.bendStiffness = 0.0f;
    base.globalShapeStiffness = 1.0f;
    base.globalShapeEffectiveRange = 0.0f;
    base.vspCoefficient = 0.0f;
    SimulationConfig ranged = base;
    ranged.globalShapeEffectiveRange = 1.0f;

    GuideSimulation simulation(TwoGuideChain(), 2, 4, {0, 1, 1, 1, 0, 1, 1, 1},
        {base, ranged}, {0u, 1u});
    simulation.Step(1.0f / 60.0f);
    simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    // 相同 stiffness，只有组 1 的作者范围覆盖全部点；组 0 范围 0 等于关闭。
    EXPECT_LT(positions[6].x() - 10.0f, positions[2].x() - 1.0e-4f);
    EXPECT_NEAR(positions[6].x(), 10.0f, 1.0e-5f);
}

TEST(TressfxGroupSimulation, CpuSolverUsesPerGuideVspCoefficient)
{
    SimulationConfig base;
    base.gravity = Eigen::Vector3f(980.0f, 0.0f, 0.0f);
    base.damping = 0.035f;
    base.iterations = 1;
    base.rootFixedPoints = 1;
    base.bendStiffness = 0.0f;
    base.globalShapeStiffness = 0.0f;
    base.vspCoefficient = 0.0f;
    base.vspAccelThresholdMax = 1.0e7f;
    SimulationConfig vsp = base;
    vsp.vspCoefficient = 1.0f;

    GuideSimulation simulation(TwoGuideChain(), 2, 4, {0, 1, 1, 1, 0, 1, 1, 1},
        {base, vsp}, {0u, 1u});
    for (int step = 0; step < 4; ++step)
        simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    // 两组只差 VSP 系数；位移差非零证明 solver 按 guide 选择了不同配置。
    EXPECT_GT(std::abs((positions[2].x()) - (positions[6].x() - 10.0f)), 1.0e-4f);
}

TEST(TressfxGroupSimulation, CpuSolverUsesPerGuideVspThreshold)
{
    SimulationConfig base;
    base.gravity = Eigen::Vector3f(980.0f, 0.0f, 0.0f);
    base.damping = 0.035f;
    base.iterations = 1;
    base.rootFixedPoints = 1;
    base.bendStiffness = 0.0f;
    base.globalShapeStiffness = 0.0f;
    base.vspCoefficient = 0.5f;
    base.vspAccelThresholdMax = 1.0e7f;
    SimulationConfig forced = base;
    // 极低阈值让任意伪加速度都强制 VspCoeff = 1。
    forced.vspAccelThresholdMax = 1.0e-6f;

    GuideSimulation simulation(TwoGuideChain(), 2, 4, {0, 1, 1, 1, 0, 1, 1, 1},
        {base, forced}, {0u, 1u});
    for (int step = 0; step < 4; ++step)
        simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    EXPECT_GT(std::abs((positions[2].x()) - (positions[6].x() - 10.0f)), 1.0e-4f);
}

TEST(TressfxGroupSimulation, RebindGroupsSwapsConfigsAndMappingWithoutResettingHistory)
{
    std::vector<Eigen::Vector3f> rest = TwoGuideChain();
    std::vector<float> mass(rest.size(), 1.0f);
    mass[0] = 0.0f;
    mass[4] = 0.0f;
    SimulationConfig base;
    base.iterations = 1;
    base.damping = 0.05f;
    SimulationConfig stiff = base;
    stiff.damping = 0.9f;

    GuideSimulation simulation(rest, 2, 4, mass, {base, stiff}, {1u, 0u});
    EXPECT_FLOAT_EQ(simulation.ConfigForGuide(0).damping, 0.9f);
    EXPECT_FLOAT_EQ(simulation.ConfigForGuide(1).damping, 0.05f);

    simulation.Step(1.0f / 60.0f);
    const float sampleX = simulation.Positions()[5].x();
    const float sampleY = simulation.Positions()[5].y();
    const uint32_t frame = simulation.SimulationFrame();

    // 组编辑只替换配置与 guide->组映射，不重建求解器、不重置姿态历史。
    SimulationConfig soft = base;
    soft.damping = 0.0f;
    simulation.RebindGroups({base, soft}, {0u, 1u});
    EXPECT_FLOAT_EQ(simulation.ConfigForGuide(0).damping, 0.05f);
    EXPECT_FLOAT_EQ(simulation.ConfigForGuide(1).damping, 0.0f);
    EXPECT_FLOAT_EQ(simulation.Positions()[5].x(), sampleX);
    EXPECT_FLOAT_EQ(simulation.Positions()[5].y(), sampleY);
    EXPECT_EQ(simulation.SimulationFrame(), frame);

    // guide 映射长度必须与几何一致，越界/错长度不再静默回退。
    EXPECT_THROW(simulation.RebindGroups({base}, {0u}), std::invalid_argument);
}

} // namespace
