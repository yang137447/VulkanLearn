#include "hair/tressfxDynamics.h"
#include "hair/tressfxSimulationParams.h"
#include "hair/tressfxSourceSchedule.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

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
    EXPECT_EQ(simulation.PreviousPreviousPositions(), Chain());
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0f);
}

TEST(TressfxDynamics, NonRootPointsRespondToAuthoredWind)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f::Zero();
    // 修正后的风力带 |v|²·1e-2 量纲，作者幅值需要远大于旧的“风速”量级。
    config.wind = Eigen::Vector3f(1.0e6f, 0.0f, 0.0f);
    config.iterations = 8;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 30; ++i)
        simulation.Step(1.0f / 60.0f);

    const auto& positions = simulation.Positions();
    EXPECT_EQ(positions[0], Eigen::Vector3f(0, 0, 0));
    EXPECT_EQ(positions[1], Eigen::Vector3f(0, 1, 0));
    // TressFX 的 Force = -cross(cross(v,w),v)，单位段 +X 风给出 -X 方向的力。
    EXPECT_LT(positions[2].x(), 0.0f);
    EXPECT_GT(positions[2].x(), -2.0f);
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

TEST(TressfxDynamics, VariableTimeStepClampsToMaxDtAndAdvancesTime)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -100, 0);
    config.maxDt = 0.01f;
    GuideSimulation clamped(Chain(), 1, 4, {0, 1, 1, 1}, config);
    GuideSimulation explicitStep(Chain(), 1, 4, {0, 1, 1, 1}, config);
    // 一次巨大的帧增量只推进一个 maxDt 的 solver step。
    clamped.Step(1.0f);
    explicitStep.Step(0.01f);
    EXPECT_FLOAT_EQ(clamped.SimulationTime(), 0.01f);
    for (size_t point = 0; point < clamped.Positions().size(); ++point)
        EXPECT_TRUE(clamped.Positions()[point].isApprox(explicitStep.Positions()[point],
            1.0e-6f));

    // 零/负 dt 不推进，也不改变 simulationTime。
    const auto before = clamped.Positions();
    clamped.Step(0.0f);
    clamped.Step(-1.0f);
    EXPECT_FLOAT_EQ(clamped.SimulationTime(), 0.01f);
    for (size_t point = 0; point < clamped.Positions().size(); ++point)
        EXPECT_EQ(clamped.Positions()[point], before[point]);
}

TEST(TressfxDynamics, VariableTimeStepIsNotFrameRateIndependent)
{
    // VL 选择每帧一次变量步长；60 Hz 与 120 Hz 的约束次数和累计截断时间不同。
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.iterations = 8;
    config.rootFixedPoints = 1;
    config.maxDt = 0.016f;
    GuideSimulation sixtyHertz(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);
    GuideSimulation oneTwentyHertz(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 60; ++i)
        sixtyHertz.Step(1.0f / 60.0f);
    for (int i = 0; i < 120; ++i)
        oneTwentyHertz.Step(1.0f / 120.0f);
    float maximumDifference = 0.0f;
    for (size_t point = 0; point < sixtyHertz.Positions().size(); ++point)
        maximumDifference = std::max(maximumDifference,
            (sixtyHertz.Positions()[point] - oneTwentyHertz.Positions()[point]).norm());
    EXPECT_GT(maximumDifference, 1.0e-3f);
}

TEST(TressfxDynamics, BendStiffnessPreservesAuthoredCurvature)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.wind = Eigen::Vector3f::Zero();
    config.iterations = 8;
    config.rootFixedPoints = 1;

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
}

TEST(TressfxDynamics, GlobalShapePullsOnlyTheAuthoredPrefixRange)
{
    SimulationConfig config;
    // 重力与直链垂直，长度约束会把发丝整体甩向 -Z；global shape 把 local 1
    // 钉在 rest，因此受约束的发丝不会整根摆过去。
    config.gravity = Eigen::Vector3f(0, 0, -980);
    config.rootFixedPoints = 1;
    config.bendStiffness = 0.0f;
    config.iterations = 8;
    config.globalShapeStiffness = 1.0f;
    // 4 点、比例 0.5：local 0、1 受约束；local 2 只能通过长度约束跟随。
    config.globalShapeEffectiveRange = 0.5f;
    GuideSimulation shaped(Chain(), 1, 4, {0, 1, 1, 1}, config);

    SimulationConfig off = config;
    off.globalShapeStiffness = 0.0f;
    GuideSimulation unshaped(Chain(), 1, 4, {0, 1, 1, 1}, off);

    for (int i = 0; i < 120; ++i)
    {
        shaped.Step(1.0f / 60.0f);
        unshaped.Step(1.0f / 60.0f);
    }
    // stiffness = 1 且 local 1 在作用范围内：每步都被拉回 InitialPos 附近；
    // 长度约束仍会在同一 step 内移动它，因此与未受约束的链比较位移幅度。
    const float shapedOffset =
        (shaped.Positions()[1] - Eigen::Vector3f(0, 1, 0)).norm();
    const float unshapedOffset =
        (unshaped.Positions()[1] - Eigen::Vector3f(0, 1, 0)).norm();
    EXPECT_GT(shapedOffset, 0.1f);
    EXPECT_LT(shapedOffset, unshapedOffset);
    // 受约束的链没有整体摆到 -Z，第二个点的幅值明显更小。
    EXPECT_LT(shaped.Positions()[2].norm(), unshaped.Positions()[2].norm());
}

TEST(TressfxDynamics, VspOffLeavesTheSolveUnchanged)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.rootFixedPoints = 1;
    config.iterations = 4;
    config.vspCoefficient = 0.0f;
    config.vspAccelThresholdMax = 1.0e9f;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 30; ++i)
        simulation.Step(1.0f / 60.0f);
    for (const auto& position : simulation.Positions())
        EXPECT_TRUE(position.allFinite());
}

TEST(TressfxDynamics, CapsuleProjectsInsidePointToSurface)
{
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 10, 0);
    capsule.radius = 1.0f;
    Eigen::Vector3f point(0.2f, 5.0f, 0.0f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(point, capsule));
    EXPECT_NEAR(point.x(), 1.0f, 1.0e-5f);
    EXPECT_NEAR(point.y(), 5.0f, 1.0e-5f);
    EXPECT_NEAR(point.z(), 0.0f, 1.0e-5f);
    EXPECT_NEAR((point - VL::Hair::ClosestPointOnCapsule(capsule, point)).norm(),
        capsule.radius, 1.0e-5f);
}

TEST(TressfxDynamics, CapsuleEndpointClampProjectsBothEnds)
{
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 10, 0);
    capsule.radius = 1.0f;

    Eigen::Vector3f below(0.0f, -0.5f, 0.2f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(below, capsule));
    EXPECT_EQ(VL::Hair::ClosestPointOnCapsule(capsule, below), capsule.a);
    EXPECT_NEAR((below - capsule.a).norm(), capsule.radius, 1.0e-5f);

    Eigen::Vector3f above(0.0f, 10.4f, -0.3f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(above, capsule));
    EXPECT_EQ(VL::Hair::ClosestPointOnCapsule(capsule, above), capsule.b);
    EXPECT_NEAR((above - capsule.b).norm(), capsule.radius, 1.0e-5f);
}

TEST(TressfxDynamics, CapsuleLeavesOutsidePointUntouched)
{
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 10, 0);
    capsule.radius = 1.0f;
    for (const Eigen::Vector3f outside : {Eigen::Vector3f(3, 5, 0), Eigen::Vector3f(1, 5, 0)})
    {
        Eigen::Vector3f point = outside;
        EXPECT_FALSE(VL::Hair::ResolveCapsuleCollision(point, capsule));
        EXPECT_EQ(point.x(), outside.x());
        EXPECT_EQ(point.y(), outside.y());
        EXPECT_EQ(point.z(), outside.z());
    }
}

TEST(TressfxDynamics, DegenerateCapsuleBehavesAsSphere)
{
    VL::Hair::TressfxCollider sphere;
    sphere.a = Eigen::Vector3f(1, 2, 3);
    sphere.b = sphere.a;
    sphere.radius = 2.0f;

    Eigen::Vector3f side(1.0f, 2.0f, 4.5f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(side, sphere));
    EXPECT_NEAR((side - sphere.a).norm(), sphere.radius, 1.0e-5f);
    EXPECT_NEAR(side.z(), 5.0f, 1.0e-5f);

    // 点恰好落在球心：退化方向固定为 +Y。
    Eigen::Vector3f center = sphere.a;
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(center, sphere));
    EXPECT_NEAR(center.y(), sphere.a.y() + sphere.radius, 1.0e-5f);
    EXPECT_NEAR((center - sphere.a).norm(), sphere.radius, 1.0e-5f);
}

TEST(TressfxDynamics, CapsuleAxisDegenerateUsesPerpendicularNormal)
{
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 10, 0);
    capsule.radius = 1.0f;
    Eigen::Vector3f point(0.0f, 5.0f, 0.0f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(point, capsule));
    EXPECT_NEAR(point.y(), 5.0f, 1.0e-5f);
    EXPECT_NEAR(std::hypot(point.x(), point.z()), capsule.radius, 1.0e-5f);
}

TEST(TressfxDynamics, CollisionNeverMovesFixedRootPoints)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f::Zero();
    config.rootFixedPoints = 2;
    config.bendStiffness = 0.0f;
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 3, 0);
    capsule.radius = 5.0f;
    config.capsules.push_back(capsule);
    const auto rest = Chain();
    GuideSimulation simulation(rest, 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 10; ++i)
        simulation.Step(1.0f / 60.0f);
    EXPECT_EQ(simulation.Positions()[0], rest[0]);
    EXPECT_EQ(simulation.Positions()[1], rest[1]);
}

TEST(TressfxDynamics, StrandStaysOutsideCapsuleAcrossSteps)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -50, 0);
    config.rootFixedPoints = 1;
    config.bendStiffness = 0.2f;
    config.iterations = 8;
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 3, 0);
    capsule.radius = 1.5f;
    config.capsules.push_back(capsule);
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    for (int i = 0; i < 600; ++i)
        simulation.Step(1.0f / 60.0f);

    for (size_t point = 1; point < simulation.Positions().size(); ++point)
    {
        const Eigen::Vector3f& position = simulation.Positions()[point];
        EXPECT_TRUE(position.allFinite());
        const float distance =
            (position - VL::Hair::ClosestPointOnCapsule(capsule, position)).norm();
        EXPECT_GE(distance, capsule.radius - 1.0e-3f);
    }
}

TEST(TressfxDynamics, GuideSimulationParamsPackStd140Controls)
{
    TressfxSimulationConfig config;
    config.gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    config.wind = Eigen::Vector3f(100000.0f, 0.0f, 0.0f);
    config.damping = 0.035f;
    config.iterations = 3;
    config.rootFixedPoints = 2;
    config.bendStiffness = 0.42f;
    config.globalShapeStiffness = 0.25f;
    config.globalShapeEffectiveRange = 0.75f;
    config.vspCoefficient = 0.4f;
    config.vspAccelThresholdMax = 6000.0f;
    VL::Hair::TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(1, 2, 3);
    capsule.b = Eigen::Vector3f(4, 5, 6);
    capsule.radius = 7.0f;
    config.capsules.push_back(capsule);

    Eigen::Matrix4f skinning = Eigen::Matrix4f::Identity();
    skinning(0, 3) = 12.0f;
    skinning(1, 3) = 34.0f;
    skinning(2, 3) = 56.0f;
    const VL::Hair::GuideSimulationParams params =
        VL::Hair::MakeGuideSimulationParams(config, 7, 32, 224, 0.016f, 0, skinning);

    EXPECT_EQ(sizeof(VL::Hair::GuideSimulationParams), 800u);
    EXPECT_EQ(offsetof(VL::Hair::GuideSimulationParams, skinning), 160u);
    EXPECT_EQ(offsetof(VL::Hair::GuideSimulationParams, historyToCurrentLocal), 224u);
    EXPECT_EQ(offsetof(VL::Hair::GuideSimulationParams, vspStiffness), 288u);
    EXPECT_EQ(offsetof(VL::Hair::GuideSimulationParams, localShapeStiffness), 544u);
    EXPECT_TRUE(params.historyToCurrentLocal.isIdentity());
    EXPECT_TRUE(params.skinning.isApprox(skinning));
    EXPECT_TRUE(params.skinningQuat.isApprox(Eigen::Vector4f(0.0f, 0.0f, 0.0f, 1.0f)));
    EXPECT_FLOAT_EQ(params.gravity.y(), -980.0f);
    // frame 0 ⇒ wM = 100000 * 0.5 = 50000；四路是 ±40° 的锥角。
    const float wM = 50000.0f;
    const float cosine = std::cos(VL::Hair::TressfxWindConeAngleRadians);
    const float sine = std::sin(VL::Hair::TressfxWindConeAngleRadians);
    EXPECT_NEAR(params.wind0.x(), wM * cosine, 1.0e-2f);
    EXPECT_NEAR(params.wind0.z(), -wM * sine, 1.0e-2f);
    EXPECT_NEAR(params.wind1.z(), wM * sine, 1.0e-2f);
    EXPECT_NEAR(params.wind2.y(), wM * sine, 1.0e-2f);
    EXPECT_NEAR(params.wind3.y(), -wM * sine, 1.0e-2f);
    EXPECT_FLOAT_EQ(params.controls.x(), 0.016f);
    // controls.y 是作者 damping 本身（TressFX DampingCoeff），不再取 -ln。
    EXPECT_FLOAT_EQ(params.controls.y(), 0.035f);
    EXPECT_FLOAT_EQ(params.controls.z(), 32.0f);
    EXPECT_FLOAT_EQ(params.controls.w(), 2.0f);
    EXPECT_FLOAT_EQ(params.shape.x(), 0.25f);
    EXPECT_FLOAT_EQ(params.shape.y(), 0.75f);
    EXPECT_FLOAT_EQ(params.shape.z(), 0.4f);
    EXPECT_FLOAT_EQ(params.shape.w(), 6000.0f);
    EXPECT_EQ(params.iterations, 3u);
    EXPECT_EQ(params.guideCount, 7u);
    EXPECT_EQ(params.pointCount, 224u);
    EXPECT_EQ(params.colliderCount, 1u);
    EXPECT_FLOAT_EQ(params.motion.x(), 0.42f);
    // 旧的 gust/frequency/simulationTime 已删除，预留通道为 0。
    EXPECT_FLOAT_EQ(params.motion.y(), 0.0f);
    EXPECT_FLOAT_EQ(params.motion.z(), 0.0f);
    EXPECT_FLOAT_EQ(params.motion.w(), 0.0f);
}

TEST(TressfxDynamics, AuthoredCurvesPackIntoStd140AndSetMotionFlags)
{
    TressfxSimulationConfig config;
    config.bendStiffness = 0.3f;
    config.vspStiffness.resize(64);
    config.localShapeStiffness.resize(64);
    for (size_t local = 0; local < 64; ++local)
    {
        config.vspStiffness[local] = static_cast<float>(local) / 63.0f;
        config.localShapeStiffness[local] = 0.95f - static_cast<float>(local) / 128.0f;
    }
    VL::Hair::GuideSimulationParams params =
        VL::Hair::MakeGuideSimulationParams(config, 1, 64, 64, 0.0125f, 0);
    // motion.w bit0/bit1 = 两个作者数组都存在。
    EXPECT_FLOAT_EQ(params.motion.w(), 3.0f);
    EXPECT_FLOAT_EQ(params.vspStiffness[0].x(), 0.0f);
    EXPECT_FLOAT_EQ(params.vspStiffness[0].y(), 1.0f / 63.0f);
    EXPECT_FLOAT_EQ(params.vspStiffness[1].x(), 4.0f / 63.0f);
    EXPECT_FLOAT_EQ(params.vspStiffness[15].w(), 1.0f);
    EXPECT_FLOAT_EQ(params.localShapeStiffness[0].x(), 0.95f);
    EXPECT_FLOAT_EQ(params.localShapeStiffness[1].x(), 0.95f - 4.0f / 128.0f);
    EXPECT_FLOAT_EQ(params.localShapeStiffness[15].w(), 0.95f - 63.0f / 128.0f);

    // 只作者一条曲线时只置对应 bit；未作者数组保持零填充。
    TressfxSimulationConfig vspOnly;
    vspOnly.vspStiffness = {0.0f, 0.5f, 1.0f, 1.0f};
    const VL::Hair::GuideSimulationParams vspParams =
        VL::Hair::MakeGuideSimulationParams(vspOnly, 1, 4, 4, 0.0125f, 0);
    EXPECT_FLOAT_EQ(vspParams.motion.w(), 1.0f);
    EXPECT_FLOAT_EQ(vspParams.vspStiffness[0].y(), 0.5f);
    for (const Eigen::Vector4f& group : vspParams.localShapeStiffness)
        EXPECT_TRUE(group.isZero());

    TressfxSimulationConfig localOnly;
    localOnly.localShapeStiffness = {0.25f, 0.5f, 0.75f, 0.95f};
    const VL::Hair::GuideSimulationParams localParams =
        VL::Hair::MakeGuideSimulationParams(localOnly, 1, 4, 4, 0.0125f, 0);
    EXPECT_FLOAT_EQ(localParams.motion.w(), 2.0f);
    EXPECT_FLOAT_EQ(localParams.localShapeStiffness[0].x(), 0.25f);
    EXPECT_TRUE(localParams.vspStiffness[0].isZero());
}

TEST(TressfxDynamics, AuthoredLocalShapeCurveWorksWithZeroScalarStiffness)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.iterations = 8;
    config.rootFixedPoints = 1;
    // 标量为 0 时，作者数组仍应逐点驱动 LocalShape。
    config.bendStiffness = 0.0f;
    config.localShapeStiffness = {0.95f, 0.95f, 0.95f, 0.95f};
    GuideSimulation shaped(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);

    SimulationConfig limpConfig = config;
    limpConfig.localShapeStiffness = {0.0f, 0.0f, 0.0f, 0.0f};
    GuideSimulation limp(CurvedChain(), 1, 4, {0, 1, 1, 1}, limpConfig);
    for (int i = 0; i < 180; ++i)
    {
        shaped.Step(1.0f / 60.0f);
        limp.Step(1.0f / 60.0f);
    }
    EXPECT_GT(Curvature(shaped), Curvature(limp));
}

TEST(TressfxDynamics, DefaultTimeStepMatchesTressFxSixteenMillisecondClamp)
{
    EXPECT_FLOAT_EQ(VL::Hair::TressfxMaxTimeStepSeconds, 0.016f);
    SimulationConfig config;
    EXPECT_FLOAT_EQ(config.maxDt, 0.016f);
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    // 1 秒的帧增量只推进一个 TressFX 上限步。
    simulation.Step(1.0f);
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.016f);

    // 资产不能声明超过 TressFX 硬上限的 maxDt。
    SimulationConfig oversized = config;
    oversized.maxDt = 0.017f;
    EXPECT_THROW(GuideSimulation(Chain(), 1, 4, {0, 1, 1, 1}, oversized),
        std::invalid_argument);
}

TEST(TressfxDynamics, DefaultVspThresholdUsesSourceLengthUnitConversion)
{
    const SimulationConfig cpuConfig;
    const TressfxSimulationConfig assetConfig;
    EXPECT_FLOAT_EQ(cpuConfig.vspAccelThresholdMax, 140.0f);
    EXPECT_FLOAT_EQ(cpuConfig.vspAccelThresholdMax,
        assetConfig.vspAccelThresholdMax);
}

TEST(TressfxDynamics, WindFrameAdvancesWithEachStepAndDrivesCorners)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f::Zero();
    config.wind = Eigen::Vector3f(1.0e6f, 0.0f, 0.0f);
    config.bendStiffness = 0.0f;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    EXPECT_EQ(simulation.SimulationFrame(), 0u);
    simulation.Step(0.016f);
    EXPECT_EQ(simulation.SimulationFrame(), 1u);
    // 同一作者风在不同帧号下的 wM 不同：第 0 帧是 0.5 倍，第 1000 帧约 1.208 倍。
    const float frameZero = VL::Hair::TressfxWindFrameModulation(0);
    const float frameThousand = VL::Hair::TressfxWindFrameModulation(1000);
    EXPECT_LT(frameZero, frameThousand);
    for (int i = 0; i < 1000; ++i)
        simulation.Step(0.016f);
    EXPECT_EQ(simulation.SimulationFrame(), 1001u);
}

TEST(TressfxDynamics, StepFrameAdvancesSourcePhysicalTimePerFrameRate)
{
    // 30/48 Hz → 2 × 0.0125 = 0.025 s；60/120/144/240 Hz → 1 × 0.0125 s。
    struct Case
    {
        float hz;
        float expectedSeconds;
        uint32_t expectedSubsteps;
    };
    const Case cases[] = {{30.0f, 0.025f, 2}, {48.0f, 0.025f, 2},
        {60.0f, 0.0125f, 1}, {120.0f, 0.0125f, 1}, {144.0f, 0.0125f, 1},
        {240.0f, 0.0125f, 1}};
    for (const Case& testCase : cases)
    {
        SCOPED_TRACE(testCase.hz);
        SimulationConfig config;
        config.gravity = Eigen::Vector3f(0, -980, 0);
        GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
        simulation.StepFrame(1.0f / testCase.hz);
        EXPECT_FLOAT_EQ(simulation.SimulationTime(), testCase.expectedSeconds);
        EXPECT_EQ(simulation.SimulationFrame(), 1u);
        // 单帧物理时间只能是 0 / 0.0125 / 0.025 三者之一。
        EXPECT_EQ(static_cast<uint32_t>(std::lround(
                      simulation.SimulationTime() /
                      VL::Hair::TressfxSourceSolverStepSeconds)),
            testCase.expectedSubsteps);
    }
}

TEST(TressfxDynamics, StepFrameMatchesTwoFixedStepsWithoutWind)
{
    // 无风时同一帧两个 substep 与两次固定步完全等价；区别只在 Frame 计数。
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    config.iterations = 8;
    config.wind = Eigen::Vector3f::Zero();
    GuideSimulation native(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);
    GuideSimulation legacy(CurvedChain(), 1, 4, {0, 1, 1, 1}, config);
    native.StepFrame(1.0f / 30.0f);
    legacy.Step(VL::Hair::TressfxSourceSolverStepSeconds);
    legacy.Step(VL::Hair::TressfxSourceSolverStepSeconds);
    for (size_t point = 0; point < native.Positions().size(); ++point)
        EXPECT_EQ(native.Positions()[point], legacy.Positions()[point]);
    EXPECT_FLOAT_EQ(native.SimulationTime(), legacy.SimulationTime());
    // 原生路径每帧只 +1 风帧；旧 Step 每调用一次就 +1。
    EXPECT_EQ(native.SimulationFrame(), 1u);
    EXPECT_EQ(legacy.SimulationFrame(), 2u);
}

TEST(TressfxDynamics, StepFrameIgnoresMaxDtAndCapsAtTwoSubsteps)
{
    // maxDt 只服务旧 Step；StepFrame 必须仍按 0.0125 推进，不受资产收紧影响。
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -100, 0);
    config.maxDt = 0.005f;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    simulation.StepFrame(1.0f / 60.0f);
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0125f);
    // 卡顿帧仍只有 NumLoops < 2 的上限：2 × 0.0125 s，不补算丢失时间。
    simulation.StepFrame(1.0f);
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0375f);
    EXPECT_EQ(simulation.SimulationFrame(), 2u);
}

TEST(TressfxDynamics, StepFrameConsumesZeroDeltaWithoutAdvancingOrUnpausing)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f(0, -980, 0);
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    const auto rest = simulation.Positions();
    for (const float delta : {0.0f, -0.0f, -1.0f / 60.0f,
             std::numeric_limits<float>::quiet_NaN()})
    {
        SCOPED_TRACE(delta);
        simulation.StepFrame(delta);
        EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0f);
        EXPECT_EQ(simulation.SimulationFrame(), 0u);
        EXPECT_EQ(simulation.Positions(), rest);
    }
    // 暂停同样冻结；Reset 仍是显式调用，不受 StepFrame 调度影响。
    simulation.StepFrame(1.0f / 60.0f);
    const auto moved = simulation.Positions();
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0125f);
    simulation.SetPaused(true);
    simulation.StepFrame(1.0f / 30.0f);
    EXPECT_EQ(simulation.Positions(), moved);
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0125f);
    simulation.SetPaused(false);
    simulation.Reset();
    EXPECT_EQ(simulation.Positions(), Chain());
    EXPECT_FLOAT_EQ(simulation.SimulationTime(), 0.0f);
    EXPECT_EQ(simulation.SimulationFrame(), 0u);
}

TEST(TressfxDynamics, StepFrameAdvancesWindPhaseOncePerFrameNotPerSubstep)
{
    SimulationConfig config;
    config.gravity = Eigen::Vector3f::Zero();
    config.wind = Eigen::Vector3f(1.0e6f, 0.0f, 0.0f);
    config.bendStiffness = 0.0f;
    GuideSimulation simulation(Chain(), 1, 4, {0, 1, 1, 1}, config);
    // 30 Hz 有两个 substep，但 wind frame 只 +1；60 Hz 一帧同样 +1。
    simulation.StepFrame(1.0f / 30.0f);
    EXPECT_EQ(simulation.SimulationFrame(), 1u);
    simulation.StepFrame(1.0f / 60.0f);
    EXPECT_EQ(simulation.SimulationFrame(), 2u);
    // 0 步帧不消费风相位。
    simulation.StepFrame(0.0f);
    EXPECT_EQ(simulation.SimulationFrame(), 2u);
}
} // namespace
