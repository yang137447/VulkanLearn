#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hair/tressfxSolverMath.h"

// TressFX 5.0 **源上层调度**的纯函数镜像，只服务于推导、记录与模块测试。
//
// 源路径（UE 检出 61f8dd9，插件 TressFXCore）：
//   TressFXComponent.cpp:946-952        每 Tick 写 Instance->TimeStep = DeltaTime
//   TressFXCoreSimulation.cpp:933-941   ++Guides.Frame；float TimeStep = Instance->TimeStep；
//                                       Instance->TimeStep = 0.0125f
//   TressFXCoreSimulation.cpp:1013      while (TimeStep > 0.f && NumLoops < 2)
//   TressFXCoreSimulation.cpp:1026/1096/1177  各 pass 收到的是 Instance->TimeStep
//   TressFXCoreSimulation.cpp:1217      TimeStep -= 0.0167f（只扣局部变量）
//
// 由此得到两个**不是同一个量**的常数：
//   - 0.0125 s 是每个 substep 真正传给 shader 的 dt（solver step）；
//   - 0.0167 s 只决定这一帧 dispatch 几次（循环扣除量）。
// 把 0.0167 当成 dt 会同时算错次数与每帧物理时间。循环每一轮的 dt 恒为
// 0.0125 s，不随帧率变化，也不随剩余时间变化。
//
// VL 自 2026-10-24 起按本文件的源调度推进运行时（GPU 与 CPU 的 StepFrame）：
//   - 每帧先按 frozenDelta 求 n，再执行 n 个固定 0.0125 s 的 substep；
//   - 源 Integration 参数函数里的 0.016 clamp（TressFXCoreSimulation.cpp:152/271）
//     作用在已经是 0.0125 的值上，因此在这条路径上永远不生效。它只说明
//     “如果有人把帧增量直接传进 Integration，0.016 会截断它”，不是源调度策略；
//   - 资产的 maxDt 仅为兼容旧资产保留，GPU 原生路径与 CPU StepFrame 都不再使用它；
//     只有旧的可变步长入口 GuideSimulation::Step 仍按 maxDt clamp。
namespace VL::Hair
{
// 每个 substep 实际 dispatch 使用的 dt（TressFXCoreSimulation.cpp:941）。
inline constexpr float TressfxSourceSolverStepSeconds = 0.0125f;
// 循环扣除量，只参与 while 条件（TressFXCoreSimulation.cpp:1217）。
inline constexpr float TressfxSourceLoopDeductionSeconds = 0.0167f;
// NumLoops < 2 的硬上限（TressFXCoreSimulation.cpp:1013）。
inline constexpr uint32_t TressfxSourceMaxSubsteps = 2;
// 只作用于 Integration pass 的输入 clamp（TressFXCoreSimulation.cpp:152/271）。
inline constexpr float TressfxSourceIntegrationTimeStepClampSeconds = 0.016f;
// Guides.Frame 每个渲染帧只 +1（TressFXCoreSimulation.cpp:933），不随 substep 增加；
// 同一帧的所有 substep 共用同一个风相位。
inline constexpr uint32_t TressfxSourceWindPhaseAdvancePerRenderFrame = 1;
// 源 SetWind 的全局默认阈值是 1.4 world m（TressFXAssetSimulation.h:79）。
inline constexpr float TressfxSourceVspThresholdDefaultMeters = 1.4f;
// LocalShape 作者曲线在采样后还会被限制到 0.95（TressFXComponent.cpp:710）。
inline constexpr float TressfxSourceLocalShapeStiffnessLimit = 0.95f;

// 源默认 1.4 m 与 VL 厘米默认值必须是同一次换算，防止两处常量漂移。
static_assert(TressfxDefaultVspAccelThresholdMaxCm / 100.0f ==
        TressfxSourceVspThresholdDefaultMeters,
    "source 1.4 m must map to the VL 140 cm default");

// 一次渲染帧的源调度结果。
struct TressfxSourceSchedule
{
    // 本帧 dispatch 的 substep 次数：0、1 或 2。
    uint32_t substeps = 0;
    // 每个 substep 的 dt；源路径上恒为 0.0125 s。
    float stepSeconds = TressfxSourceSolverStepSeconds;
    // 本帧推进的物理时间 = substeps * stepSeconds。
    float framePhysicalSeconds = 0.0f;
    // 物理时间 / 真实时间。deltaSeconds <= 0 时无定义，这里记 0。
    float realtimeMultiplier = 0.0f;
};

// 复刻源的 float 减法循环，保证边界值与源一致：
//   delta <= 0        -> 0 次
//   0 < delta <= 0.0167 -> 1 次
//   delta > 0.0167    -> 2 次（含 delta > 0.0334，被 cap 截断）
// NaN 与负值一样不进入循环。
inline TressfxSourceSchedule TressfxSourceScheduleForDelta(float deltaSeconds)
{
    float remaining = deltaSeconds;
    uint32_t loops = 0;
    while (remaining > 0.0f && loops < TressfxSourceMaxSubsteps)
    {
        remaining -= TressfxSourceLoopDeductionSeconds;
        ++loops;
    }

    TressfxSourceSchedule schedule;
    schedule.substeps = loops;
    schedule.framePhysicalSeconds =
        static_cast<float>(loops) * TressfxSourceSolverStepSeconds;
    if (deltaSeconds > 0.0f)
        schedule.realtimeMultiplier =
            schedule.framePhysicalSeconds / deltaSeconds;
    return schedule;
}

// 一帧的 VL 原生调度计划 = 源 substep 次数 + reset/换基落在哪个 substep。
// 源的 bResetPositions 在循环体第一轮 Integration 生效，同一轮后面的
// VSP/LocalShape/LengthWind/SDF collision 仍会录制（TressFXCoreSimulation.cpp:1135-1214），
// 第二轮则是正常步进；`TressfxSourceScheduleForDelta` 为 0 时循环不执行，
// reset 事件因此必须留到下一个正 delta 帧（host 不能提前 Commit 历史）。
struct GuideSubstepPlan
{
    TressfxSourceSchedule schedule;
    // 本帧 substep 0 的 Integration 携带 ResetPositions（motion.y = 1）。
    bool resetFirstSubstep = false;
    // 本帧 substep 0 的 Integration 额外携带历史换基矩阵。这是 VL 为角色整体
    // 位移追加的语义，源没有；它只在非 reset 帧发生，且只作用于首个 substep
    // （后续 substep 的三份历史已经在当前局部空间）。
    bool rebaseFirstSubstep = false;

    bool HasWork() const { return schedule.substeps > 0; }

    // reset 与正常步进不能共用同一份 UBO：同一命令缓冲里的多个 dispatch 之间
    // 主机无法再改写参数，所以 substep 0 需要一份独立参数（见
    // hairSimulationPass.cpp 的 resetParams）。reset 与换基互斥，共用这份参数。
    bool FirstSubstepUsesSeparateParams() const
    {
        return HasWork() && (resetFirstSubstep || rebaseFirstSubstep);
    }

    // 源在 reset 首轮也不会跳过后续约束/风/碰撞；本函数用于固定这个顺序。
    bool RunsConstraintChain(uint32_t substep) const
    {
        return substep < schedule.substeps;
    }
};

// 由冻结帧增量与 VL 的 reset/placement 事件生成原生调度计划。
// resetRequested 来自 GuideSimulationHistory::Plan（非消费查询）；
// delta <= 0 时既不消费 reset 也不做换基，调用方必须保持历史/时钟未提交。
inline GuideSubstepPlan PlanGuideSubsteps(float deltaSeconds, bool resetRequested,
    bool placementChanged)
{
    GuideSubstepPlan plan;
    plan.schedule = TressfxSourceScheduleForDelta(deltaSeconds);
    if (!plan.HasWork())
        return plan;
    plan.resetFirstSubstep = resetRequested;
    // reset 会把三份历史直接写回 InitialPos，此时换基矩阵没有意义。
    plan.rebaseFirstSubstep = !resetRequested && placementChanged;
    return plan;
}

// 每步阻尼保留比例：exp(-DampingCoeff * dt * 60)，dt 是 solver step 0.0125。
// 与 tressfxSolverMath.h 的 TressfxDampingDecay 同式，这里固定源步长。
inline float TressfxSourceDampingDecayPerStep(float dampingCoeff)
{
    return TressfxDampingDecay(dampingCoeff, TressfxSourceSolverStepSeconds);
}

// 每渲染帧的阻尼保留比例 = decay^substeps（1 或 2 步）。
inline float TressfxSourceDampingRetentionPerFrame(float dampingCoeff,
    float deltaSeconds)
{
    const TressfxSourceSchedule schedule =
        TressfxSourceScheduleForDelta(deltaSeconds);
    return std::pow(TressfxSourceDampingDecayPerStep(dampingCoeff),
        static_cast<float>(schedule.substeps));
}

// 每真实秒的阻尼保留比例 = decay^(substeps / deltaSeconds)。
// deltaSeconds <= 0 时没有可定义的推进，返回 1（不衰减）。
inline float TressfxSourceDampingRetentionPerRealSecond(float dampingCoeff,
    float deltaSeconds)
{
    if (!(deltaSeconds > 0.0f))
        return 1.0f;
    const TressfxSourceSchedule schedule =
        TressfxSourceScheduleForDelta(deltaSeconds);
    const float stepsPerSecond =
        static_cast<float>(schedule.substeps) / deltaSeconds;
    return std::pow(TressfxSourceDampingDecayPerStep(dampingCoeff),
        stepsPerSecond);
}

// 旧的可变步长入口（GuideSimulation::Step；新运行时不再调用）：
// dt = clamp(frameDelta, 0, maxDt)，一帧一个 step。
inline float TressfxVlStepSeconds(float frameDelta, float maxDt)
{
    return std::clamp(frameDelta, 0.0f, maxDt);
}

// 旧可变步长每真实秒的阻尼保留比例（只用于对照，原生调度不调用）。
// 60 Hz 时 frameDelta 0.016667 被截到 0.016，与源 0.0125 相差约 28% 的指数。
inline float TressfxVlDampingRetentionPerRealSecond(float dampingCoeff,
    float frameDelta, float maxDt)
{
    if (!(frameDelta > 0.0f))
        return 1.0f;
    const float step = TressfxVlStepSeconds(frameDelta, maxDt);
    return std::pow(TressfxDampingDecay(dampingCoeff, step),
        1.0f / frameDelta);
}

// 源风相位：frame 是仿真帧序号，不是秒（TressFXCoreSimulation.cpp:933 + 763）。
// 同一渲染帧的两个 substep 用同一个 frame，因此相位相同；但源在每次
// SetWind 调用里还会乘一个 (0.3 + FRand()) 的随机幅值（TressFXCoreSimulation.cpp:763），
// 所以两轮 substep 的风幅值其实不同，且不可复现。VL 没有这个随机项。
inline float TressfxSourceWindModulationForFrame(uint32_t frame)
{
    return TressfxWindFrameModulation(frame);
}

// ---------------------------------------------------------------------------
// VSP / LocalShape 参数语义（纯推导，不参与运行时）
// ---------------------------------------------------------------------------

// 源 StrandLevel 的“伪加速度”：length(new - 2*old + oldOld)，单位与位置相同。
// 它没有除 dt²，所以**不是加速度**；把它当加速度会误判量纲。
inline float TressfxSourcePseudoAccelerationCm(const Eigen::Vector3f& newPosition,
    const Eigen::Vector3f& oldPosition, const Eigen::Vector3f& oldOldPosition)
{
    return (newPosition - 2.0f * oldPosition + oldOldPosition).norm();
}

// 源比较是严格大于（patch TressFXSimulationStrandLevel.usf:9927）。
inline bool TressfxSourceVspForcesFullFollow(float pseudoAcceleration,
    float threshold)
{
    return pseudoAcceleration > threshold;
}

// 把“二阶位置差阈值”换算成等效加速度（只用于说明量纲，不是源字段）：
// a_equiv = threshold / dt²。同一个 140 cm 在 0.0125 s 与 0.016 s 下不是同一个
// 加速度阈值。
inline float TressfxVspThresholdEquivalentAccelerationCmPerSecondSquared(
    float thresholdCm, float stepSeconds)
{
    if (!(stepSeconds > 0.0f))
        return 0.0f;
    return thresholdCm / (stepSeconds * stepSeconds);
}

// 源作者曲线采样坐标：i / NumVerticesPerStrand（TressFXComponent.cpp:691/710），
// i = 0..N-1。注意梢部的 x 是 (N-1)/N < 1，曲线在 x = 1 的取值永远采不到。
inline float TressfxSourceCurveCoordinate(uint32_t localPoint,
    uint32_t pointCount)
{
    if (pointCount == 0)
        return 0.0f;
    return static_cast<float>(localPoint) / static_cast<float>(pointCount);
}

// 源 LocalShape 在采样之后再做 min(value, 0.95)（TressFXComponent.cpp:710）。
inline float TressfxSourceLocalShapeStiffness(float curveValue)
{
    return std::min(curveValue, TressfxSourceLocalShapeStiffnessLimit);
}

} // namespace VL::Hair
