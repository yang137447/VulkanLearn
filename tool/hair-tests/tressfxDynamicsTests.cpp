#include "hair/tressfxDynamics.h"
#include "hair/tressfxSimulationParams.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
using VL::Hair::GuideSimulation;
using VL::Hair::SimulationConfig;

std::vector<Eigen::Vector3f> Chain()
{
    return {Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0),
        Eigen::Vector3f(0, 2, 0), Eigen::Vector3f(0, 3, 0)};
}

std::vector<Eigen::Vector3f> CurvedChain()
{
    return {Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0.6f),
        Eigen::Vector3f(0, 2, -0.6f), Eigen::Vector3f(0, 3, 0)};
}

float Curvature(const GuideSimulation& simulation)
{
    const auto& positions = simulation.Positions();
    float total = 0.0f;
    for (size_t local = 1; local + 1 < positions.size(); ++local)
    {
        total += (positions[local] -
            0.5f * (positions[local - 1] + positions[local + 1])).norm();
    }
    return total;
}

TEST(TressfxDynamics, RootsRemainFixedAndSegmentsStayNearRestLength)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.iterations = 8;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 60; ++i)
        simulation.Step(1.0f / 60.0f);
    const auto& positions = simulation.Positions();
    EXPECT_EQ(positions[0], Eigen::Vector3f(0, 0, 0));
    EXPECT_EQ(positions[1], Eigen::Vector3f(0, 1, 0));
    for (size_t i = 0; i < 3; ++i)
        EXPECT_NEAR((positions[i + 1] - positions[i]).norm(), 1.0f, 2e-3f);
}

TEST(TressfxDynamics, PauseAndResetAreDeterministic)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -10, 0);
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    simulation.Step(1.0f / 60.0f);
    const auto moved = simulation.Positions();
    simulation.SetPaused(true);
    simulation.Step(1.0f / 60.0f);
    EXPECT_EQ(simulation.Positions(), moved);
    simulation.SetPaused(false);
    simulation.Reset();
    EXPECT_EQ(simulation.Positions(), Chain());
    EXPECT_EQ(simulation.PreviousPositions(), Chain());
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0f);
}

TEST(TressfxDynamics, NonRootPointsRespondToAuthoredWind)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f::Zero();
    config.wind = Eigen::Vector3f(40.0f, 0.0f, 0.0f);
    config.iterations = 8;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 30; ++i)
        simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    EXPECT_EQ(positions[0], Eigen::Vector3f(0, 0, 0));
    EXPECT_EQ(positions[1], Eigen::Vector3f(0, 1, 0));
    EXPECT_GT(positions.back().x(), 0.0f);
}

TEST(TressfxDynamics, LongRunRemainsFinite)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 36000; ++i)
        simulation.Step(1.0f / 60.0f);
    for (const auto& position : simulation.Positions())
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_TRUE(std::isfinite(position[axis]));
}

TEST(TressfxDynamics, FixedSubstepsMakeFrameRateIndependentMotion)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.wind = Eigen::Vector3f::Zero();
    config.iterations = 8;
    config.rootFixedPoints = 1;
    // 允许 30 Hz 的帧 dt 原样进入 accumulator，避免 maxDt clamp 干扰对照。
    config.maxDt = 0.05f;
    GuideSimulation sixtyHertz(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);
    GuideSimulation thirtyHertz(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);

    for (int i = 0; i < 60; ++i)
        sixtyHertz.Step(1.0f / 60.0f);
    for (int i = 0; i < 30; ++i)
        thirtyHertz.Step(1.0f / 30.0f);

    ASSERT_EQ(sixtyHertz.Positions().size(), thirtyHertz.Positions().size());
    for (size_t point = 0; point < sixtyHertz.Positions().size(); ++point)
        EXPECT_NEAR((sixtyHertz.Positions()[point] - thirtyHertz.Positions()[point]).norm(),
            0.0f, 1.0e-4f);
    EXPECT_NEAR(sixtyHertz.SimulationTime(), thirtyHertz.SimulationTime(), 1.0e-5f);
}

TEST(TressfxDynamics, BacklogDropCapsSubstepsWithoutExploding)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -100, 0);
    config.wind = Eigen::Vector3f::Zero();
    config.iterations = 4;
    config.bendStiffness = 0.0f;
    config.fixedStep = 0.01f;
    config.maxDt = 0.2f;
    config.maxSubsteps = 3;
    GuideSimulation backlog(Chain(), 1, 4, {0, 1, 1, 1}, config);
    GuideSimulation reference(Chain(), 1, 4, {0, 1, 1, 1}, config);

    // 一次巨大 dt 最多推进 maxSubsteps 个 fixedStep；剩余 backlog 被丢弃。
    backlog.Step(1000.0f);
    for (int i = 0; i < 3; ++i)
        reference.Step(config.fixedStep);
    for (size_t point = 0; point < backlog.Positions().size(); ++point)
    {
        EXPECT_NEAR((backlog.Positions()[point] - reference.Positions()[point]).norm(),
            0.0f, 1.0e-5f);
        EXPECT_TRUE(backlog.Positions()[point].allFinite());
        EXPECT_LT(backlog.Positions()[point].norm(), 5.0f);
    }

    // 第二帧仍然只走 maxSubsteps，而不是补上未消费的 backlog。
    backlog.Step(1000.0f);
    for (int i = 0; i < 3; ++i)
        reference.Step(config.fixedStep);
    for (size_t point = 0; point < backlog.Positions().size(); ++point)
        EXPECT_NEAR((backlog.Positions()[point] - reference.Positions()[point]).norm(),
            0.0f, 1.0e-5f);
}

TEST(TressfxDynamics, BendStiffnessPreservesAuthoredCurvature)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.wind = Eigen::Vector3f::Zero();
    config.iterations = 8;
    config.rootFixedPoints = 1;
    config.fixedStep = 1.0f / 60.0f;

    SimulationConfig softConfig = config;
    softConfig.bendStiffness = 0.0f;
    config.bendStiffness = 0.8f;
    GuideSimulation bent(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);
    GuideSimulation limp(CurvedChain(), 1, 4, {0, 1, 1, 1}, softConfig);
    for (int i = 0; i < 180; ++i)
    {
        bent.Step(1.0f / 60.0f);
        limp.Step(1.0f / 60.0f);
    }

    const float bentCurvature = Curvature(bent);
    const float limpCurvature = Curvature(limp);
    EXPECT_GT(bentCurvature, limpCurvature);
    EXPECT_GT(bentCurvature, 1.0e-3f);
    EXPECT_LT(limpCurvature, bentCurvature * 0.5f);
}

TEST(TressfxDynamics, WindGustIsBoundedAndPeriodic)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(-80.0f, 40.0f, 20.0f);
    config.wind = Eigen::Vector3f(120.0f, -30.0f, 60.0f);
    config.windGust = 0.7f;
    config.windFrequency = 0.4f;
    const float magnitudeBound =
        config.gravity.norm() + config.wind.norm() * (1.0f + config.windGust);
    for (size_t guide = 0; guide < 32; ++guide)
    {
        for (int sample = 0; sample < 200; ++sample)
        {
            const float simulationTime = 0.013f * static_cast<float>(sample);
            const Eigen::Vector3f acceleration =
                VL::Hair::GuideAcceleration(config, guide, simulationTime);
            EXPECT_LE(acceleration.norm(), magnitudeBound + 1.0e-3f);
        }
    }

    const float sampleTime = 0.37f;
    const float period = 1.0f / config.windFrequency;
    EXPECT_NEAR(VL::Hair::WindGustFactor(config, 5, sampleTime),
        VL::Hair::WindGustFactor(config, 5, sampleTime + period), 2.0e-4f);
    EXPECT_NE(VL::Hair::WindGustFactor(config, 5, sampleTime),
        VL::Hair::WindGustFactor(config, 6, sampleTime));
}

TEST(TressfxDynamics, GuideSimulationParamsPackStd140Controls)
{
    TressfxSimulationConfig config;
    config.gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    config.wind = Eigen::Vector3f(120.0f, 0.0f, -3.0f);
    config.damping = 0.995f;
    config.iterations = 3;
    config.rootFixedPoints = 2;
    config.bendStiffness = 0.42f;
    config.windGust = 0.6f;
    config.windFrequency = 1.5f;
    config.fixedStep = 0.02f;
    config.maxSubsteps = 3;

    const VL::Hair::GuideSimulationParams params =
        VL::Hair::MakeGuideSimulationParams(config, 7, 32, 224, 0.75f);

    EXPECT_EQ(sizeof(VL::Hair::GuideSimulationParams), 80u);
    EXPECT_FLOAT_EQ(params.gravity.x(), 0.0f);
    EXPECT_FLOAT_EQ(params.gravity.y(), -980.0f);
    EXPECT_FLOAT_EQ(params.gravity.z(), 0.0f);
    EXPECT_FLOAT_EQ(params.wind.x(), 120.0f);
    EXPECT_FLOAT_EQ(params.wind.y(), 0.0f);
    EXPECT_FLOAT_EQ(params.wind.z(), -3.0f);
    EXPECT_FLOAT_EQ(params.controls.x(), 0.02f);
    EXPECT_FLOAT_EQ(params.controls.y(), 0.995f);
    EXPECT_FLOAT_EQ(params.controls.z(), 32.0f);
    EXPECT_FLOAT_EQ(params.controls.w(), 2.0f);
    EXPECT_EQ(params.iterations, 3u);
    EXPECT_EQ(params.guideCount, 7u);
    EXPECT_EQ(params.pointCount, 224u);
    EXPECT_FLOAT_EQ(params.motion.x(), 0.42f);
    EXPECT_FLOAT_EQ(params.motion.y(), 0.6f);
    EXPECT_FLOAT_EQ(params.motion.z(), 1.5f);
    EXPECT_FLOAT_EQ(params.motion.w(), 0.75f);
}
} // namespace
