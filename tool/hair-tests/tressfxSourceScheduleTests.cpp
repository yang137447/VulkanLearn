#include "hair/tressfxSourceSchedule.h"

#include "hair/tressfxCollision.h"
#include "hair/tressfxDynamics.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

// 这些测试固定“源调度/参数语义”的推导、运行时计划函数与旧路径的对照，不驱动 GPU。
// 任何一条失败都意味着 documents/rationale/tressfx5-hair-timing-params-record.md
// 里的闭式表或落差值需要一起改。
namespace
{
using VL::Hair::TressfxCollider;
using VL::Hair::TressfxSourceSchedule;
using VL::Hair::TressfxSourceScheduleForDelta;

constexpr float kTolerance = 1.0e-6f;

struct FrameRateCase
{
    float hz;
    uint32_t substeps;
    float framePhysicalSeconds;
    float realtimeMultiplier;
};

// 20/30/48/60/120/144/240 Hz 的闭式结果（文档表格同源）。
constexpr std::array<FrameRateCase, 7> kFrameRateTable{{
    {20.0f, 2, 0.025f, 0.5f},
    {30.0f, 2, 0.025f, 0.75f},
    {48.0f, 2, 0.025f, 1.2f},
    {60.0f, 1, 0.0125f, 0.75f},
    {120.0f, 1, 0.0125f, 1.5f},
    {144.0f, 1, 0.0125f, 1.8f},
    {240.0f, 1, 0.0125f, 3.0f},
}};
} // namespace

TEST(TressfxSourceSchedule, SubstepsPhysicalTimeAndRateMultiplierPerFrameRate)
{
    for (const FrameRateCase& testCase : kFrameRateTable)
    {
        SCOPED_TRACE(testCase.hz);
        const float delta = 1.0f / testCase.hz;
        const TressfxSourceSchedule schedule =
            TressfxSourceScheduleForDelta(delta);
        EXPECT_EQ(schedule.substeps, testCase.substeps);
        EXPECT_FLOAT_EQ(schedule.stepSeconds,
            VL::Hair::TressfxSourceSolverStepSeconds);
        EXPECT_NEAR(schedule.framePhysicalSeconds,
            testCase.framePhysicalSeconds, kTolerance);
        EXPECT_NEAR(schedule.realtimeMultiplier,
            testCase.realtimeMultiplier, 1.0e-5f);
        // 每帧物理时间只可能是 0 / 0.0125 / 0.025 三个值之一。
        EXPECT_TRUE(schedule.framePhysicalSeconds == 0.0f ||
            schedule.framePhysicalSeconds == 0.0125f ||
            schedule.framePhysicalSeconds == 0.025f);
    }
}

TEST(TressfxSourceSchedule, NonPositiveDeltaAdvancesNothing)
{
    for (const float delta : {0.0f, -0.0f, -1.0f / 60.0f})
    {
        SCOPED_TRACE(delta);
        const TressfxSourceSchedule schedule =
            TressfxSourceScheduleForDelta(delta);
        EXPECT_EQ(schedule.substeps, 0u);
        EXPECT_FLOAT_EQ(schedule.framePhysicalSeconds, 0.0f);
        // delta == 0 时倍率无定义，约定记 0。
        EXPECT_FLOAT_EQ(schedule.realtimeMultiplier, 0.0f);
    }
    // NaN 与负值一样不进入 while 循环。
    const TressfxSourceSchedule nanSchedule =
        TressfxSourceScheduleForDelta(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(nanSchedule.substeps, 0u);
}

TEST(TressfxSourceSchedule, FirstDeductionBoundaryKeepsOneStep)
{
    // 恰好等于 0.0167 时，扣一次后剩余为 0，while 结束：只有 1 个 substep。
    const TressfxSourceSchedule atBoundary = TressfxSourceScheduleForDelta(
        VL::Hair::TressfxSourceLoopDeductionSeconds);
    EXPECT_EQ(atBoundary.substeps, 1u);
    EXPECT_FLOAT_EQ(atBoundary.framePhysicalSeconds, 0.0125f);

    // 只比边界大一个 ulp 就会进入第二轮。
    const float justAbove = std::nextafter(
        VL::Hair::TressfxSourceLoopDeductionSeconds,
        std::numeric_limits<float>::infinity());
    const TressfxSourceSchedule aboveBoundary =
        TressfxSourceScheduleForDelta(justAbove);
    EXPECT_EQ(aboveBoundary.substeps, 2u);

    // 1/60 s = 0.0166667 < 0.0167：60 Hz 只有 1 步，物理时间反而是 0.75 倍。
    EXPECT_EQ(TressfxSourceScheduleForDelta(1.0f / 60.0f).substeps, 1u);
}

TEST(TressfxSourceSchedule, SecondDeductionBoundaryAndHardCap)
{
    // 2 * 0.0167 = 0.0334：扣两次后剩余 <= 0，仍然是 2 步。
    const TressfxSourceSchedule atSecondBoundary = TressfxSourceScheduleForDelta(
        2.0f * VL::Hair::TressfxSourceLoopDeductionSeconds);
    EXPECT_EQ(atSecondBoundary.substeps, 2u);
    EXPECT_NEAR(atSecondBoundary.framePhysicalSeconds, 0.025f, kTolerance);
    EXPECT_EQ(TressfxSourceScheduleForDelta(0.0334f).substeps, 2u);

    // 更大的帧增量被 NumLoops < 2 截断，不会补算丢失的物理时间。
    for (const float delta : {0.04f, 0.1f, 0.25f, 10.0f})
    {
        SCOPED_TRACE(delta);
        const TressfxSourceSchedule schedule =
            TressfxSourceScheduleForDelta(delta);
        EXPECT_EQ(schedule.substeps, 2u);
        EXPECT_NEAR(schedule.framePhysicalSeconds, 0.025f, kTolerance);
    }
}

TEST(TressfxSourceSchedule, SolverStepIsConstantAndNotTheDeductionAmount)
{
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceSolverStepSeconds, 0.0125f);
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceLoopDeductionSeconds, 0.0167f);
    EXPECT_NE(VL::Hair::TressfxSourceSolverStepSeconds,
        VL::Hair::TressfxSourceLoopDeductionSeconds);
    // 两个 substep 的 dt 相同：循环里只扣局部剩余时间，不改 Instance->TimeStep。
    for (const float delta : {0.02f, 0.033f, 0.5f})
    {
        SCOPED_TRACE(delta);
        EXPECT_FLOAT_EQ(TressfxSourceScheduleForDelta(delta).stepSeconds,
            VL::Hair::TressfxSourceSolverStepSeconds);
    }
    // 0.0167 不是每帧物理时间的倍数，也不能由它反推 dt。
    EXPECT_NEAR(2.0f * VL::Hair::TressfxSourceSolverStepSeconds, 0.025f, kTolerance);
}

TEST(TressfxSourceSchedule, IntegrationClampNeverBindsOnSourcePath)
{
    // Integration 的 0.016 clamp 只作用于参数函数入参；入参已经是 0.0125。
    EXPECT_LT(VL::Hair::TressfxSourceSolverStepSeconds,
        VL::Hair::TressfxSourceIntegrationTimeStepClampSeconds);
    // 帧增量本身却经常超过 0.016；若有人直接把帧增量传给 Integration，
    // 30 Hz 的 0.0333 就会被截到 0.016，两次 dispatch 都变成 0.016 s。
    const float delta30 = 1.0f / 30.0f;
    EXPECT_GT(delta30, VL::Hair::TressfxSourceIntegrationTimeStepClampSeconds);
    EXPECT_FLOAT_EQ(
        std::min(delta30, VL::Hair::TressfxSourceIntegrationTimeStepClampSeconds),
        0.016f);
    // VL 的 maxDt 正是借用了这个 clamp；它限制的是“每帧唯一一步”的 dt。
    EXPECT_FLOAT_EQ(VL::Hair::TressfxMaxTimeStepSeconds,
        VL::Hair::TressfxSourceIntegrationTimeStepClampSeconds);
}

TEST(TressfxSourceSchedule, WindPhaseAdvancesOncePerRenderFrameAcrossSubsteps)
{
    EXPECT_EQ(VL::Hair::TressfxSourceWindPhaseAdvancePerRenderFrame, 1u);
    const TressfxSourceSchedule twoSteps =
        TressfxSourceScheduleForDelta(1.0f / 30.0f);
    ASSERT_EQ(twoSteps.substeps, 2u);
    // 同一渲染帧的两轮 substep 读到同一个 frame，因此相位相同。
    const uint32_t frame = 1234;
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceWindModulationForFrame(frame),
        VL::Hair::TressfxWindFrameModulation(frame));
    EXPECT_NE(VL::Hair::TressfxSourceWindModulationForFrame(frame),
        VL::Hair::TressfxSourceWindModulationForFrame(frame + 1));
    // 60 Hz 帧率与 30 Hz 的相位推进速度相同：每渲染帧 +1，而不是每秒 +N。
    EXPECT_EQ(TressfxSourceScheduleForDelta(1.0f / 60.0f).substeps, 1u);
}

TEST(TressfxSourceSchedule, PlanGuideSubstepsKeepsResetInTheFirstSubstep)
{
    // 源 reset 只在循环体首轮 Integration 生效；同一轮的后续约束/风/碰撞仍录制，
    // 第二轮是正常步进（TressFXCoreSimulation.cpp:1135-1214）。
    for (const float delta : {1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f})
    {
        SCOPED_TRACE(delta);
        const VL::Hair::GuideSubstepPlan plan =
            VL::Hair::PlanGuideSubsteps(delta, true, false);
        EXPECT_TRUE(plan.HasWork());
        EXPECT_TRUE(plan.resetFirstSubstep);
        EXPECT_FALSE(plan.rebaseFirstSubstep);
        EXPECT_TRUE(plan.FirstSubstepUsesSeparateParams());
        for (uint32_t substep = 0; substep < plan.schedule.substeps; ++substep)
            EXPECT_TRUE(plan.RunsConstraintChain(substep));
        EXPECT_FALSE(plan.RunsConstraintChain(plan.schedule.substeps));
    }
    // 30 Hz 是两轮：reset 只归首轮，第二轮没有 reset 参数。
    const VL::Hair::GuideSubstepPlan twoStepPlan =
        VL::Hair::PlanGuideSubsteps(1.0f / 30.0f, true, false);
    ASSERT_EQ(twoStepPlan.schedule.substeps, 2u);
    EXPECT_TRUE(twoStepPlan.FirstSubstepUsesSeparateParams());
    EXPECT_TRUE(twoStepPlan.RunsConstraintChain(1));
}

TEST(TressfxSourceSchedule, PlanGuideSubstepsDefersResetWhenDeltaHasNoSubsteps)
{
    // 源 while 在 delta <= 0 时不执行：reset 事件不能在本帧被消费，留到下一个
    // 正 delta 帧。计划函数必须返回“无工作”，而不是把 reset 做成空 dispatch。
    for (const float delta : {0.0f, -0.0f, -1.0f / 60.0f,
             std::numeric_limits<float>::quiet_NaN()})
    {
        SCOPED_TRACE(delta);
        const VL::Hair::GuideSubstepPlan plan =
            VL::Hair::PlanGuideSubsteps(delta, true, true);
        EXPECT_FALSE(plan.HasWork());
        EXPECT_EQ(plan.schedule.substeps, 0u);
        EXPECT_FALSE(plan.resetFirstSubstep);
        EXPECT_FALSE(plan.rebaseFirstSubstep);
        EXPECT_FALSE(plan.FirstSubstepUsesSeparateParams());
    }
    // 同一个 reset 请求在下一个正 delta 帧仍然生效（history 由调用方保持未提交）。
    const VL::Hair::GuideSubstepPlan next =
        VL::Hair::PlanGuideSubsteps(1.0f / 60.0f, true, true);
    EXPECT_TRUE(next.HasWork());
    EXPECT_TRUE(next.resetFirstSubstep);
    EXPECT_FALSE(next.rebaseFirstSubstep);
}

TEST(TressfxSourceSchedule, PlanGuideSubstepsRebasesHistoryOnlyInTheFirstSubstep)
{
    // VL 的 placement 换基不是源行为：它只在首个 substep 的 Integration 应用一次，
    // 后续 substep 的历史已经位于当前局部空间，不能再乘一次。
    const VL::Hair::GuideSubstepPlan moved =
        VL::Hair::PlanGuideSubsteps(1.0f / 30.0f, false, true);
    ASSERT_EQ(moved.schedule.substeps, 2u);
    EXPECT_FALSE(moved.resetFirstSubstep);
    EXPECT_TRUE(moved.rebaseFirstSubstep);
    EXPECT_TRUE(moved.FirstSubstepUsesSeparateParams());
    // reset 优先：三份历史会被直接写回 InitialPos，不再需要换基。
    const VL::Hair::GuideSubstepPlan resetAndMoved =
        VL::Hair::PlanGuideSubsteps(1.0f / 30.0f, true, true);
    EXPECT_TRUE(resetAndMoved.resetFirstSubstep);
    EXPECT_FALSE(resetAndMoved.rebaseFirstSubstep);
    // 无 reset/placement 的正常帧不需要独立参数块。
    const VL::Hair::GuideSubstepPlan normal =
        VL::Hair::PlanGuideSubsteps(1.0f / 30.0f, false, false);
    EXPECT_TRUE(normal.HasWork());
    EXPECT_FALSE(normal.FirstSubstepUsesSeparateParams());
}

TEST(TressfxSourceDamping, PerStepMatchesSourceFormula)
{
    constexpr float damping = 0.035f;
    const float expected =
        std::exp(-damping * VL::Hair::TressfxSourceSolverStepSeconds * 60.0f);
    EXPECT_NEAR(VL::Hair::TressfxSourceDampingDecayPerStep(damping),
        expected, kTolerance);
    // 60 Hz 一帧一步，30 Hz 一帧两步；两步的保留比例是单步的平方。
    const float oneStep =
        VL::Hair::TressfxSourceDampingRetentionPerFrame(damping, 1.0f / 60.0f);
    const float twoSteps =
        VL::Hair::TressfxSourceDampingRetentionPerFrame(damping, 1.0f / 30.0f);
    EXPECT_NEAR(oneStep, expected, kTolerance);
    EXPECT_NEAR(twoSteps, expected * expected, kTolerance);
}

TEST(TressfxSourceDamping, PerSecondRetentionFollowsStepCountNotFrameRate)
{
    constexpr float damping = 0.035f;
    // 60 Hz：每帧 1 步 × 60 帧 = 60 步/秒；30 Hz：2 步 × 30 = 60 步/秒。
    // 两者每秒保留比例相同，这正是“按 60 Hz 参考写”的效果。
    const float at60 = VL::Hair::TressfxSourceDampingRetentionPerRealSecond(
        damping, 1.0f / 60.0f);
    const float at30 = VL::Hair::TressfxSourceDampingRetentionPerRealSecond(
        damping, 1.0f / 30.0f);
    EXPECT_NEAR(at60, 0.207008f, 1.0e-5f);
    EXPECT_NEAR(at30, at60, 1.0e-5f);
    // 更高帧率不再是 60 步/秒：120 Hz 是 120 步/秒，衰减明显更快。
    const float at120 = VL::Hair::TressfxSourceDampingRetentionPerRealSecond(
        damping, 1.0f / 120.0f);
    EXPECT_NEAR(at120, 0.042852f, 1.0e-5f);
    EXPECT_LT(at120, at60);
    // 20 Hz 只有 40 步/秒，每秒保留更多速度。
    const float at20 = VL::Hair::TressfxSourceDampingRetentionPerRealSecond(
        damping, 1.0f / 20.0f);
    EXPECT_NEAR(at20, 0.349938f, 1.0e-5f);
    EXPECT_GT(at20, at60);
}

TEST(TressfxSourceDamping, VlVariableStepRetainsLessPerSecondAt60Hz)
{
    constexpr float damping = 0.035f;
    constexpr float maxDt = 0.016f;
    // VL：60 Hz 的 0.016667 被 clamp 到 0.016，每帧一步。
    EXPECT_FLOAT_EQ(VL::Hair::TressfxVlStepSeconds(1.0f / 60.0f, maxDt), maxDt);
    const float vlAt60 = VL::Hair::TressfxVlDampingRetentionPerRealSecond(
        damping, 1.0f / 60.0f, maxDt);
    const float sourceAt60 = VL::Hair::TressfxSourceDampingRetentionPerRealSecond(
        damping, 1.0f / 60.0f);
    // 源的指数是 0.035*0.0125*60*60 = 1.575；VL 是 0.035*0.016*60*60 = 2.016。
    EXPECT_NEAR(sourceAt60, 0.207008f, 1.0e-5f);
    EXPECT_NEAR(vlAt60, 0.133187f, 1.0e-5f);
    EXPECT_LT(vlAt60, sourceAt60);
    // VL 在 dt 被 clamp 的区段（<= 62.5 Hz）每秒衰减随帧率变化，源也随
    // 步数变化，但两者的映射不同：20/30/48 Hz 的 VL 每秒保留分别是
    // 0.5107 / 0.3649 / 0.1993，而源是 0.3499 / 0.2070 / 0.0805。
    EXPECT_NEAR(VL::Hair::TressfxVlDampingRetentionPerRealSecond(
                    damping, 1.0f / 20.0f, maxDt),
        0.510686f, 1.0e-5f);
    EXPECT_NEAR(VL::Hair::TressfxVlDampingRetentionPerRealSecond(
                    damping, 1.0f / 30.0f, maxDt),
        0.364948f, 1.0e-5f);
    EXPECT_NEAR(VL::Hair::TressfxVlDampingRetentionPerRealSecond(
                    damping, 1.0f / 48.0f, maxDt),
        0.199329f, 1.0e-5f);
}

TEST(TressfxSourceVsp, ThresholdIsALengthNotAnAcceleration)
{
    // 源默认 1.4 world m；VL 全部厘米，同量纲换算是 140，不是 1.4。
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceVspThresholdDefaultMeters, 1.4f);
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceVspThresholdDefaultMeters * 100.0f,
        VL::Hair::TressfxDefaultVspAccelThresholdMaxCm);
    EXPECT_FLOAT_EQ(VL::Hair::TressfxDefaultVspAccelThresholdMaxCm, 140.0f);
    // 旧记录里的 5000 cm 不是 1.4 m 的换算，两者相差 35.7 倍；
    // 5000 与 140 都是“二阶位置差幅值”（长度），不是 cm/s²。
    constexpr float legacyThresholdCm = 5000.0f;
    EXPECT_NE(legacyThresholdCm, 140.0f);
    EXPECT_NEAR(legacyThresholdCm / 140.0f, 35.714f, 1.0e-3f);
    // 同一个 140 cm 在不同 solver step 下对应不同的等效加速度：
    // 0.0125 s -> 896000 cm/s²，0.016 s -> 546875 cm/s²。
    EXPECT_NEAR(VL::Hair::TressfxVspThresholdEquivalentAccelerationCmPerSecondSquared(
                    140.0f, 0.0125f),
        896000.0f, 1.0f);
    EXPECT_NEAR(VL::Hair::TressfxVspThresholdEquivalentAccelerationCmPerSecondSquared(
                    140.0f, 0.016f),
        546875.0f, 1.0f);
    // 若把 5000 当阈值，等效加速度是 140 的 35.7 倍，VSP 几乎不会触发。
    EXPECT_NEAR(VL::Hair::TressfxVspThresholdEquivalentAccelerationCmPerSecondSquared(
                    5000.0f, 0.0125f),
        896000.0f * 35.714f, 1.0e3f);
}

TEST(TressfxSourceVsp, PseudoAccelerationScalesWithStepSquared)
{
    // 解析序列：x(t) = 0.5*a*t²（常量加速度），二阶位置差 = a*dt²，与 dt² 成正比。
    constexpr float acceleration = 980.0f; // cm/s²
    const auto secondDifference = [](float dt) {
        constexpr float a = 980.0f;
        const float x0 = 0.0f;
        const float x1 = 0.5f * a * dt * dt;
        const float x2 = 0.5f * a * (2.0f * dt) * (2.0f * dt);
        return VL::Hair::TressfxSourcePseudoAccelerationCm(
            Eigen::Vector3f(0.0f, x2, 0.0f), Eigen::Vector3f(0.0f, x1, 0.0f),
            Eigen::Vector3f(0.0f, x0, 0.0f));
    };
    // dt = 0.1 s：a*dt² = 9.8 cm；dt = 0.2 s：39.2 cm（4 倍）。
    EXPECT_NEAR(secondDifference(0.1f), acceleration * 0.01f, 1.0e-3f);
    EXPECT_NEAR(secondDifference(0.2f), acceleration * 0.04f, 1.0e-3f);
    EXPECT_NEAR(secondDifference(0.2f) / secondDifference(0.1f), 4.0f, 1.0e-4f);

    // 匀速直线运动二阶差分为 0：不触发 VSP。
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourcePseudoAccelerationCm(
                        Eigen::Vector3f(0, 10, 0), Eigen::Vector3f(0, 5, 0),
                        Eigen::Vector3f(0, 0, 0)),
        0.0f);
    // 严格大于：恰好等于阈值不强制。
    EXPECT_FALSE(VL::Hair::TressfxSourceVspForcesFullFollow(140.0f, 140.0f));
    EXPECT_TRUE(VL::Hair::TressfxSourceVspForcesFullFollow(140.001f, 140.0f));
    // 同一段真实运动在 0.0125/0.025 s 下二阶差分是 1:4；140 cm 阈值在
    // 0.025 s 下等效加速度只剩 1/4。
    EXPECT_NEAR(VL::Hair::TressfxVspThresholdEquivalentAccelerationCmPerSecondSquared(
                    140.0f, 0.025f),
        VL::Hair::TressfxVspThresholdEquivalentAccelerationCmPerSecondSquared(
            140.0f, 0.0125f) / 4.0f,
        1.0e-2f);
}

TEST(TressfxSourceVsp, CurveSamplingUsesIOverNNotEndpoints)
{
    // 源按 i/NumVerticesPerStrand 采样（TressFXComponent.cpp:691/710），i = 0..N-1。
    // 根是 0，梢部是 (N-1)/N，永远到不了 1：曲线在 x = 1 的值采不到。
    constexpr uint32_t count = 8;
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceCurveCoordinate(0, count), 0.0f);
    EXPECT_NEAR(VL::Hair::TressfxSourceCurveCoordinate(1, count), 0.125f, kTolerance);
    EXPECT_NEAR(VL::Hair::TressfxSourceCurveCoordinate(count - 1, count),
        0.875f, kTolerance);
    EXPECT_LT(VL::Hair::TressfxSourceCurveCoordinate(count - 1, count), 1.0f);
    // 若作者曲线在 x=1 才回到 0，源梢部仍取 0.875 处的值；VL 的线性近似在
    // 梢部直接给 0，这是形状与自变量的双重差异。
    constexpr uint32_t rootFixedPoints = 2;
    const float vlAtRootFixed = VL::Hair::TressfxVspStiffness(
        rootFixedPoints, count, rootFixedPoints);
    const float vlAtTip = VL::Hair::TressfxVspStiffness(
        count - 1, count, rootFixedPoints);
    EXPECT_NEAR(vlAtRootFixed, 1.0f, kTolerance);
    EXPECT_NEAR(vlAtTip, 0.0f, kTolerance);
    // 作者曲线 f(x) = 1 - x 与 VL 线性近似在同一个顶点上不同：
    // local = 2 时源是 1 - 2/8 = 0.75，VL 是 1.0。
    EXPECT_NEAR(1.0f - VL::Hair::TressfxSourceCurveCoordinate(2, count), 0.75f,
        kTolerance);
    EXPECT_NE(VL::Hair::TressfxVspStiffness(2, count, rootFixedPoints), 0.75f);
}

TEST(TressfxSourceVsp, LocalShapeCurveIsClampedToPointNineFive)
{
    // 源 LocalShape 采样后 min(value, 0.95)（TressFXComponent.cpp:710），
    // 因此 stiffness 永远不会达到 1；VL 的单一标量可以取 1。
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceLocalShapeStiffness(1.0f), 0.95f);
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceLocalShapeStiffness(0.4f), 0.4f);
    EXPECT_FLOAT_EQ(VL::Hair::TressfxSourceLocalShapeStiffness(0.0f), 0.0f);
    VL::Hair::SimulationConfig config;
    config.bendStiffness = 1.0f;
    EXPECT_FLOAT_EQ(config.bendStiffness, 1.0f);
}

TEST(TressfxCapsuleCollision, ProjectionDoesNotRewriteVelocityHistory)
{
    using VL::Hair::GuideSimulation;
    using VL::Hair::SimulationConfig;

    SimulationConfig config;
    config.gravity = Eigen::Vector3f::Zero();
    config.damping = 0.0f;
    config.bendStiffness = 0.0f;
    config.vspCoefficient = 0.0f;
    config.iterations = 1;
    config.rootFixedPoints = 2;
    TressfxCollider sphere;
    sphere.a = Eigen::Vector3f(0, 2, 0);
    sphere.b = sphere.a;
    sphere.radius = 0.5f;
    config.capsules.push_back(sphere);

    const std::vector<Eigen::Vector3f> rest{
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0),
        Eigen::Vector3f(0, 2, 0), Eigen::Vector3f(0, 3, 0)};
    GuideSimulation simulation(rest, 1, 4, {0, 1, 1, 1}, config);
    simulation.Step(0.0125f);

    const auto& positions = simulation.Positions();
    const auto& previous = simulation.PreviousPositions();
    const auto& previousPrevious = simulation.PreviousPreviousPositions();
    // 中间点被推到球面上（退化胶囊法线 +Y）。
    EXPECT_NEAR((positions[2] - Eigen::Vector3f(0, 2.5f, 0)).norm(), 0.0f, 1.0e-5f);
    // 碰撞只改当前位置：previous / previousPrevious 仍是积分前的 rest。
    EXPECT_NEAR((previous[2] - Eigen::Vector3f(0, 2, 0)).norm(), 0.0f, 1.0e-5f);
    EXPECT_NEAR((previousPrevious[2] - Eigen::Vector3f(0, 2, 0)).norm(), 0.0f,
        1.0e-5f);
    // 由此产生的“速度”是碰撞位移，而不是被阻尼或摩擦消耗掉的冲量。
    EXPECT_GT((positions[2] - previous[2]).norm(), 0.0f);
}

TEST(TressfxCapsuleCollision, ProjectionIsIdempotent)
{
    TressfxCollider capsule;
    capsule.a = Eigen::Vector3f(0, 0, 0);
    capsule.b = Eigen::Vector3f(0, 10, 0);
    capsule.radius = 2.0f;
    Eigen::Vector3f point(0.5f, 5.0f, 0.0f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(point, capsule));
    const Eigen::Vector3f projected = point;
    EXPECT_NEAR((projected - Eigen::Vector3f(2, 5, 0)).norm(), 0.0f, 1.0e-5f);
    // 第二次调用已经在表面上：不再移动，也不再返回“发生过投影”。
    EXPECT_FALSE(VL::Hair::ResolveCapsuleCollision(point, capsule));
    EXPECT_NEAR((point - projected).norm(), 0.0f, 1.0e-6f);
    // 外侧点完全不动。
    Eigen::Vector3f outside(5.0f, 5.0f, 0.0f);
    EXPECT_FALSE(VL::Hair::ResolveCapsuleCollision(outside, capsule));
    EXPECT_NEAR((outside - Eigen::Vector3f(5, 5, 0)).norm(), 0.0f, kTolerance);
}

TEST(TressfxCapsuleCollision, OverlapResultDependsOnCapsuleOrder)
{
    // 两个重叠球：先 A 后 B 与先 B 后 A 的最终位置不同。VL 的 shader 与 CPU
    // 都按作者声明顺序逐个投影（tressfxSimCollision.comp:31-79）。
    TressfxCollider sphereA;
    sphereA.a = Eigen::Vector3f(0, 0, 0);
    sphereA.b = sphereA.a;
    sphereA.radius = 1.0f;
    TressfxCollider sphereB;
    sphereB.a = Eigen::Vector3f(1, 0, 0);
    sphereB.b = sphereB.a;
    sphereB.radius = 1.0f;

    Eigen::Vector3f orderAB(0.6f, 0.2f, 0.0f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(orderAB, sphereA));
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(orderAB, sphereB));

    Eigen::Vector3f orderBA(0.6f, 0.2f, 0.0f);
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(orderBA, sphereB));
    EXPECT_TRUE(VL::Hair::ResolveCapsuleCollision(orderBA, sphereA));

    EXPECT_GT((orderAB - orderBA).norm(), 1.0e-3f);
    // 两种顺序的结果都在两个球外（每个球都满足投影条件）。
    EXPECT_GE((orderAB - sphereA.a).norm(), sphereA.radius - 1.0e-5f);
    EXPECT_GE((orderAB - sphereB.a).norm(), sphereB.radius - 1.0e-5f);
    EXPECT_GE((orderBA - sphereA.a).norm(), sphereA.radius - 1.0e-5f);
    EXPECT_GE((orderBA - sphereB.a).norm(), sphereB.radius - 1.0e-5f);
}
