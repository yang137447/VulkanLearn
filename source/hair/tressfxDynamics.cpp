#include "hair/tressfxDynamics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "hair/tressfxSolverMath.h"
#include "hair/tressfxSourceSchedule.h"

namespace VL::Hair
{
namespace
{
    // 作者数组与 .tfx 的 pointsPerStrand 同长；CPU solver 没有 TFX 上下文，
    // 因此用实际构造的 pointsPerGuide 做同样的硬校验，避免曲线与顶点错位。
    void ValidateAuthoredStiffnessCurve(const std::vector<float>& curve,
        size_t pointsPerGuide, const char* field)
    {
        if (curve.empty())
            return;
        if (curve.size() != pointsPerGuide)
            throw std::invalid_argument(std::string("Invalid TressFX guide simulation ")
                + field + " length");
        for (float value : curve)
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                throw std::invalid_argument(std::string("Invalid TressFX guide simulation ")
                    + field + " value");
    }

    void ValidateConfig(const SimulationConfig& config, size_t pointsPerGuide)
    {
        if (!std::isfinite(config.damping) || config.damping < 0.0f ||
            config.damping > 1.0f || !std::isfinite(config.maxDt) ||
            config.maxDt <= 0.0f ||
            config.maxDt > TressfxMaxTimeStepSeconds ||
            config.iterations == 0 || config.iterations > 16 ||
            !std::isfinite(config.bendStiffness) || config.bendStiffness < 0.0f ||
            config.bendStiffness > 1.0f ||
            !std::isfinite(config.globalShapeStiffness) ||
            config.globalShapeStiffness < 0.0f ||
            config.globalShapeStiffness > 1.0f ||
            !std::isfinite(config.globalShapeEffectiveRange) ||
            config.globalShapeEffectiveRange < 0.0f ||
            config.globalShapeEffectiveRange > 1.0f ||
            !std::isfinite(config.vspCoefficient) ||
            config.vspCoefficient < 0.0f ||
            config.vspCoefficient > 1.0f ||
            !std::isfinite(config.vspAccelThresholdMax) ||
            config.vspAccelThresholdMax <= 0.0f ||
            config.rootFixedPoints > 32)
            throw std::invalid_argument("Invalid TressFX guide simulation config");
        if (config.capsules.size() > TressfxMaxColliders)
            throw std::invalid_argument(
                "Invalid TressFX guide simulation collider count");
        for (const TressfxCollider& capsule : config.capsules)
        {
            if (!capsule.a.allFinite() || !capsule.b.allFinite() ||
                !std::isfinite(capsule.radius) || capsule.radius <= 0.0f)
                throw std::invalid_argument(
                    "Invalid TressFX guide simulation collider");
        }
        ValidateAuthoredStiffnessCurve(
            config.vspStiffness, pointsPerGuide, "vspStiffness");
        ValidateAuthoredStiffnessCurve(
            config.localShapeStiffness, pointsPerGuide, "localShapeStiffness");
    }
} // namespace

GuideSimulation::GuideSimulation(
    std::vector<Eigen::Vector3f> rest,
    size_t guides,
    size_t points,
    std::vector<float> masses,
    SimulationConfig simulationConfig)
    : GuideSimulation(std::move(rest), guides, points, std::move(masses),
        std::vector<SimulationConfig>{std::move(simulationConfig)}, {})
{
}

GuideSimulation::GuideSimulation(
    std::vector<Eigen::Vector3f> rest,
    size_t guides,
    size_t points,
    std::vector<float> masses,
    std::vector<SimulationConfig> simulationGroupConfigs,
    std::vector<uint32_t> simulationGuideGroupIds)
    : restPositions(std::move(rest))
    , positions(restPositions)
    , previousPositions(restPositions)
    , previousPreviousPositions(restPositions)
    , inverseMass(std::move(masses))
    , guideCount(guides)
    , pointsPerGuide(points)
    , groupConfigs(std::move(simulationGroupConfigs))
    , guideGroupIds(std::move(simulationGuideGroupIds))
    , config(groupConfigs.empty() ? SimulationConfig{} : groupConfigs.front())
{
    if (guideCount == 0 || pointsPerGuide < 2 ||
        restPositions.size() != guideCount * pointsPerGuide ||
        inverseMass.size() != restPositions.size())
        throw std::invalid_argument("Invalid TressFX guide simulation dimensions");
    if (groupConfigs.empty())
        throw std::invalid_argument("Invalid TressFX guide simulation config");
    for (const SimulationConfig& groupConfig : groupConfigs)
        ValidateConfig(groupConfig, pointsPerGuide);

    movable.resize(restPositions.size());
    for (size_t point = 0; point < restPositions.size(); ++point)
    {
        const size_t local = point % pointsPerGuide;
        movable[point] = TressfxIsImmovable(static_cast<uint32_t>(local),
            config.rootFixedPoints, inverseMass[point]) ? 0u : 1u;
    }
    restLengths.assign(restPositions.size(), 0.0f);
    for (size_t guide = 0; guide < guideCount; ++guide)
    {
        const size_t base = guide * pointsPerGuide;
        for (size_t local = 0; local + 1 < pointsPerGuide; ++local)
            restLengths[base + local] =
                (restPositions[base + local + 1] - restPositions[base + local]).norm();
    }
}

void GuideSimulation::Reset()
{
    positions = restPositions;
    previousPositions = restPositions;
    previousPreviousPositions = restPositions;
    simulationTime = 0.0f;
    simulationFrame = 0;
}

void GuideSimulation::RebindGroups(std::vector<SimulationConfig> configs,
    std::vector<uint32_t> simulationGuideGroupIds)
{
    if (configs.empty())
        throw std::invalid_argument("Invalid TressFX guide simulation config");
    if (simulationGuideGroupIds.size() != guideCount)
        throw std::invalid_argument("TressFX guide group ids must match the guide count");
    for (const SimulationConfig& groupConfig : configs)
        ValidateConfig(groupConfig, pointsPerGuide);
    groupConfigs = std::move(configs);
    guideGroupIds = std::move(simulationGuideGroupIds);
    config = groupConfigs.front();
}

const SimulationConfig& GuideSimulation::GuideConfig(size_t guide) const
{
    uint32_t groupId = guide < guideGroupIds.size() ? guideGroupIds[guide] : 0u;
    if (groupId >= groupConfigs.size())
        groupId = 0u;
    return groupConfigs[groupId];
}

void GuideSimulation::IntegratePass(size_t guide, float timeStep, float decay)
{
    const size_t base = guide * pointsPerGuide;
    const float timeStepSquared = timeStep * timeStep;
    const SimulationConfig& guideConfig = GuideConfig(guide);
    for (size_t local = 0; local < pointsPerGuide; ++local)
    {
        const size_t point = base + local;
        // 与 GPU TressFXSimIntegration 的 UpdateFinalVertexPositions 同顺序：
        // prevPrev <- prev, prev <- current, current <- integrated/initial。
        const Eigen::Vector3f current = positions[point];
        const Eigen::Vector3f old = previousPositions[point];
        previousPreviousPositions[point] = old;
        previousPositions[point] = current;
        if (!movable[point])
        {
            // CPU 没有骨骼矩阵；不可移动点等于蒙皮前 rest（GPU 为 M * rest）。
            positions[point] = restPositions[point];
            continue;
        }
        const Eigen::Vector3f integrated = current + decay * (current - old) +
            guideConfig.gravity * timeStepSquared;
        positions[point] = TressfxGlobalShapeApplies(
                                 static_cast<uint32_t>(local),
                                 static_cast<uint32_t>(pointsPerGuide),
                                 guideConfig.globalShapeStiffness,
                                 guideConfig.globalShapeEffectiveRange)
            ? TressfxGlobalShapePull(restPositions[point], integrated,
                  guideConfig.globalShapeStiffness)
            : integrated;
    }
}

GuideSimulation::StrandTransform GuideSimulation::StrandLevelPass(size_t guide) const
{
    const size_t base = guide * pointsPerGuide;
    const SimulationConfig& guideConfig = GuideConfig(guide);
    StrandTransform transform;
    transform.coefficient = guideConfig.vspCoefficient;
    const auto computed = TressfxComputeStrandTransform(
        previousPositions[base], previousPositions[base + 1],
        positions[base], positions[base + 1],
        previousPreviousPositions[base + 1], guideConfig.vspCoefficient,
        guideConfig.vspAccelThresholdMax);
    transform.quat = computed.quat;
    transform.translation = computed.translation;
    transform.coefficient = computed.coefficient;
    return transform;
}

void GuideSimulation::VspPass(size_t guide, const StrandTransform& transform)
{
    // vspCoefficient == 0 时 Host 不录制 VSP dispatch，CPU 侧同样早退。
    const SimulationConfig& guideConfig = GuideConfig(guide);
    if (guideConfig.vspCoefficient <= 0.0f)
        return;
    const size_t base = guide * pointsPerGuide;
    const std::vector<float>* authored =
        guideConfig.vspStiffness.empty() ? nullptr : &guideConfig.vspStiffness;
    for (size_t local = guideConfig.rootFixedPoints; local < pointsPerGuide; ++local)
    {
        if (!movable[base + local])
            continue;
        const float coefficient = TressfxVspCoefficient(transform.coefficient,
            static_cast<uint32_t>(local), static_cast<uint32_t>(pointsPerGuide),
            guideConfig.rootFixedPoints, authored);
        const size_t point = base + local;
        positions[point] = TressfxVspBlend(positions[point], transform.quat,
            transform.translation, coefficient);
        previousPositions[point] = TressfxVspBlend(previousPositions[point],
            transform.quat, transform.translation, coefficient);
    }
}

void GuideSimulation::LocalShapePass(size_t guide, const StrandTransform& transform)
{
    // 标量为 0 但作者数组非空时仍要逐点求解；只有两者都缺席才整体关闭。
    const SimulationConfig& guideConfig = GuideConfig(guide);
    if (guideConfig.bendStiffness <= 0.0f &&
        guideConfig.localShapeStiffness.empty())
        return;
    const size_t base = guide * pointsPerGuide;
    // TressFX 用 `NumVerticesFromRootNoSimulation - 1` 作为起点；该值是 0 时无符号
    // 下溢使循环一次都不执行。这里显式保留同一行为。
    if (guideConfig.rootFixedPoints == 0)
        return;
    // rootFixedPoints == 1 时 TressFX 会从局部点 0 开始并读取 `GlobalVertexIndex - 1`
    // （即上一根 strand 的末点）；VL 从 1 开始，避免首根 strand 越界读。作者资产
    // 使用 rootFixedPoints >= 2，两者数值一致。
    const std::vector<float>* authored = guideConfig.localShapeStiffness.empty()
        ? nullptr : &guideConfig.localShapeStiffness;
    const size_t start = std::max<size_t>(guideConfig.rootFixedPoints - 1, 1);
    for (size_t local = start; local + 1 < pointsPerGuide; ++local)
    {
        const float stiffness = TressfxLocalShapeStiffness(
            static_cast<uint32_t>(local), guideConfig.bendStiffness, authored);
        // 作者曲线上的 0 表示该点不受局部形状约束；跳过与计算零 delta 等价，
        // 但避免无谓的三点旋转与四元数归一化。
        if (stiffness <= 0.0f)
            continue;
        const size_t point = base + local;
        const Eigen::Vector3f delta = TressfxLocalShapeDelta(transform.quat,
            restPositions[point - 1], restPositions[point],
            restPositions[point + 1], positions[point - 1], positions[point],
            positions[point + 1], stiffness);
        // 与源实现相同：del 只加到可移动点上；不可移动点既不收也不给。
        if (movable[point])
            positions[point] -= delta;
        if (movable[point + 1])
            positions[point + 1] += delta;
    }
}

void GuideSimulation::LengthAndWindPass(size_t guide, float timeStep)
{
    const size_t base = guide * pointsPerGuide;
    const SimulationConfig& guideConfig = GuideConfig(guide);
    if (guideConfig.wind.squaredNorm() > 0.0f)
    {
        // 四路锥形风逐帧生成一次；TressfxCombineWind 已把权重和 2 与 cos40
        // 归一化，混合结果就是场景空气速度（cm/s）。
        const TressfxWindCorners corners = TressfxBuildWindCorners(
            guideConfig.wind, guideConfig.wind.norm());
        const Eigen::Vector3f wind = TressfxCombineWind(corners.wind0,
            corners.wind1, corners.wind2, corners.wind3,
            static_cast<uint32_t>(guide));
        const float dragRatePerSpeed = TressfxHairDragRatePerSpeed(
            guideConfig.windGain, guideConfig.strandRadius);
        for (size_t local = guideConfig.rootFixedPoints;
             local + 1 < pointsPerGuide; ++local)
        {
            const size_t point = base + local;
            positions[point] += TressfxDragDisplacement(
                positions[point], previousPositions[point], positions[point + 1],
                wind, timeStep, dragRatePerSpeed);
        }
    }

    // TressFX 的偶数/奇数边分组：每一组内部边互不共享顶点，因此组内串行与
    // groupshared 并行结果相同；组间必须按偶数边 -> 奇数边的顺序执行。
    const size_t evenEdgeCount = pointsPerGuide / 2;
    const size_t oddEdgeCount = (pointsPerGuide - 1) / 2;
    for (uint32_t iteration = 0; iteration < guideConfig.iterations; ++iteration)
    {
        for (size_t edge = 0; edge < evenEdgeCount; ++edge)
        {
            const size_t point = base + 2 * edge;
            TressfxApplyDistanceConstraint(positions[point], movable[point] != 0,
                positions[point + 1], movable[point + 1] != 0,
                restLengths[point]);
        }
        for (size_t edge = 0; edge < oddEdgeCount; ++edge)
        {
            const size_t point = base + 2 * edge + 1;
            TressfxApplyDistanceConstraint(positions[point], movable[point] != 0,
                positions[point + 1], movable[point + 1] != 0,
                restLengths[point]);
        }
    }

    // 速度 clamp：超限时重写 previous 位置，而不是直接限制当前位置。
    for (size_t local = 0; local < pointsPerGuide; ++local)
    {
        const size_t point = base + local;
        TressfxClampPositionDelta(previousPositions[point], positions[point]);
    }
}

void GuideSimulation::CollisionPass(size_t guide)
{
    const SimulationConfig& guideConfig = GuideConfig(guide);
    if (guideConfig.capsules.empty())
        return;
    const size_t base = guide * pointsPerGuide;
    // 胶囊按作者顺序逐个应用；重叠区域的最终位置取决于这个顺序。
    for (const TressfxCollider& capsule : guideConfig.capsules)
    {
        for (size_t local = 0; local < pointsPerGuide; ++local)
        {
            const size_t point = base + local;
            // 不可移动点保持在 InitialPos，不参与碰撞。
            if (!movable[point])
                continue;
            Eigen::Vector3f position = positions[point];
            if (ResolveCapsuleCollision(position, capsule))
                positions[point] = position;
        }
    }
}

void GuideSimulation::SolveStrand(size_t guide, float timeStep)
{
    // 作者 damping 直接就是 TressFX 的 DampingCoeff，不做 retention 变换。
    const float decay = TressfxDampingDecay(GuideConfig(guide).damping, timeStep);
    IntegratePass(guide, timeStep, decay);
    const StrandTransform transform = StrandLevelPass(guide);
    VspPass(guide, transform);
    LocalShapePass(guide, transform);
    LengthAndWindPass(guide, timeStep);
    CollisionPass(guide);
}

void GuideSimulation::Step(float dt)
{
    if (paused)
        return;
    // 旧的可变步长路径：每帧一个 step，直接用 clamp 后帧增量；不等价于源上层循环。
    const float timeStep = std::clamp(dt, 0.0f, config.maxDt);
    if (timeStep <= 0.0f)
        return;
    for (size_t guide = 0; guide < guideCount; ++guide)
        SolveStrand(guide, timeStep);
    simulationTime += timeStep;
    ++simulationFrame;
}

void GuideSimulation::StepFrame(float deltaSeconds)
{
    if (paused)
        return;
    // 与 GPU 路径共用同一份源调度：n 由帧增量决定，每个 substep 恒为 0.0125 s。
    // 这里不读 config.maxDt，否则会把源固定步长重新变回“每帧唯一一步的上限”。
    const TressfxSourceSchedule schedule =
        TressfxSourceScheduleForDelta(deltaSeconds);
    if (schedule.substeps == 0)
        return;
    for (uint32_t substep = 0; substep < schedule.substeps; ++substep)
    {
        // 同一帧的所有 substep 用同一个 simulationFrame（风相位），只在整帧
        // 结束后 +1，与源 ++Guides.Frame 每渲染帧一次一致。
        for (size_t guide = 0; guide < guideCount; ++guide)
            SolveStrand(guide, TressfxSourceSolverStepSeconds);
        simulationTime += TressfxSourceSolverStepSeconds;
    }
    ++simulationFrame;
}

} // namespace VL::Hair
