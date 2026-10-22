#include "hair/tressfxDynamics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Hair
{

namespace
{
constexpr float TwoPi = 6.28318530717958647692f;
// 黄金分割共轭：相邻 guide 的阵风相位不重合，避免整束发丝同步摆动。
constexpr float GoldenRatioConjugate = 0.6180339f;
}

float WindGustFactor(const SimulationConfig& config, size_t guideIndex, float simulationTime)
{
    const float phase = static_cast<float>(guideIndex) * GoldenRatioConjugate;
    return 1.0f + config.windGust *
        std::sin(TwoPi * config.windFrequency * simulationTime + phase);
}

Eigen::Vector3f GuideAcceleration(
    const SimulationConfig& config,
    size_t guideIndex,
    float simulationTime)
{
    return config.gravity + config.wind * WindGustFactor(config, guideIndex, simulationTime);
}

GuideSimulation::GuideSimulation(
    std::vector<Eigen::Vector3f> rest,
    size_t guides,
    size_t points,
    std::vector<float> masses,
    SimulationConfig simulationConfig)
    : restPositions(std::move(rest))
    , positions(restPositions)
    , previousPositions(restPositions)
    , inverseMass(std::move(masses))
    , guideCount(guides)
    , pointsPerGuide(points)
    , config(simulationConfig)
{
    if (guideCount == 0 || pointsPerGuide < 2 ||
        restPositions.size() != guideCount * pointsPerGuide ||
        inverseMass.size() != restPositions.size())
        throw std::invalid_argument("Invalid TressFX guide simulation dimensions");
    if (!std::isfinite(config.damping) || config.damping < 0.0f ||
        config.damping > 1.0f || !std::isfinite(config.maxDt) || config.maxDt <= 0.0f ||
        config.iterations == 0 ||
        !std::isfinite(config.bendStiffness) || config.bendStiffness < 0.0f ||
        config.bendStiffness > 1.0f ||
        !std::isfinite(config.windGust) || config.windGust < 0.0f ||
        config.windGust > 1.0f ||
        !std::isfinite(config.windFrequency) || config.windFrequency < 0.0f ||
        config.windFrequency > 10.0f ||
        !std::isfinite(config.fixedStep) || config.fixedStep <= 0.0f ||
        config.fixedStep > 0.1f ||
        config.maxSubsteps == 0 || config.maxSubsteps > 8)
        throw std::invalid_argument("Invalid TressFX guide simulation config");
    if (config.capsules.size() > TressfxMaxColliders)
        throw std::invalid_argument("Invalid TressFX guide simulation collider count");
    for (const TressfxCollider& capsule : config.capsules)
    {
        if (!capsule.a.allFinite() || !capsule.b.allFinite() ||
            !std::isfinite(capsule.radius) || capsule.radius <= 0.0f)
            throw std::invalid_argument("Invalid TressFX guide simulation collider");
    }
}

bool GuideSimulation::IsFixed(size_t point) const
{
    const size_t local = point % pointsPerGuide;
    return local < config.rootFixedPoints || inverseMass[point] <= 0.0f;
}

void GuideSimulation::Reset()
{
    positions = restPositions;
    previousPositions = restPositions;
    accumulator = 0.0f;
    simulationTime = 0.0f;
}

void GuideSimulation::ProjectLengthsForGuide(size_t guide)
{
    const size_t base = guide * pointsPerGuide;
    for (size_t local = 0; local + 1 < pointsPerGuide; ++local)
    {
        const size_t a = base + local;
        const size_t b = a + 1;
        const Eigen::Vector3f delta = positions[b] - positions[a];
        const float length = delta.norm();
        const float restLength = (restPositions[b] - restPositions[a]).norm();
        if (length <= 1.0e-6f || restLength <= 1.0e-6f)
            continue;
        const float wa = IsFixed(a) ? 0.0f : inverseMass[a];
        const float wb = IsFixed(b) ? 0.0f : inverseMass[b];
        const float weight = wa + wb;
        if (weight <= 0.0f)
            continue;
        const Eigen::Vector3f correction = delta * ((length - restLength) / length);
        if (wa > 0.0f)
            positions[a] += correction * (wa / weight);
        if (wb > 0.0f)
            positions[b] -= correction * (wb / weight);
    }
}

void GuideSimulation::ProjectBendForGuide(size_t guide)
{
    const float bendStiffness = config.bendStiffness;
    if (bendStiffness <= 0.0f)
        return;
    const size_t base = guide * pointsPerGuide;
    for (size_t local = 1; local + 1 < pointsPerGuide; ++local)
    {
        const size_t a = base + local - 1;
        const size_t b = base + local;
        const size_t c = base + local + 1;
        if (IsFixed(b))
            continue;
        // 保存 rest 局部曲率，而不是把三点拉成直线：target 把作者曲率偏移
        // 平移到当前邻居中点上，弯曲约束只抵抗形状变化，不引入绝对位置。
        const Eigen::Vector3f restOffset =
            restPositions[b] - 0.5f * (restPositions[a] + restPositions[c]);
        const Eigen::Vector3f target =
            0.5f * (positions[a] + positions[c]) + restOffset;
        positions[b] += (target - positions[b]) * bendStiffness;
    }
}

void GuideSimulation::ProjectCollisionsForGuide(size_t guide)
{
    if (config.capsules.empty())
        return;
    const size_t base = guide * pointsPerGuide;
    // 胶囊按作者顺序逐个应用；重叠区域的最终位置取决于这个顺序。
    for (const TressfxCollider& capsule : config.capsules)
    {
        for (size_t local = 0; local < pointsPerGuide; ++local)
        {
            const size_t point = base + local;
            // 固定根点保持在 rest，不参与碰撞；否则 pin 与投影会互相拉扯。
            if (IsFixed(point))
                continue;
            Eigen::Vector3f position = positions[point];
            if (ResolveCapsuleCollision(position, capsule))
                positions[point] = position;
        }
    }
}

void GuideSimulation::Substep()
{
    const float dt = config.fixedStep;
    const float dt2 = dt * dt;
    for (size_t guide = 0; guide < guideCount; ++guide)
    {
        const size_t base = guide * pointsPerGuide;
        const Eigen::Vector3f acceleration =
            GuideAcceleration(config, guide, simulationTime);
        for (size_t local = 0; local < pointsPerGuide; ++local)
        {
            const size_t point = base + local;
            if (IsFixed(point))
            {
                positions[point] = restPositions[point];
                previousPositions[point] = restPositions[point];
                continue;
            }
            const Eigen::Vector3f current = positions[point];
            const Eigen::Vector3f velocity =
                (positions[point] - previousPositions[point]) * config.damping;
            previousPositions[point] = current;
            positions[point] = current + velocity + acceleration * dt2;
        }
        // 与 GPU kernel 一致：每次 iteration 先长度投影，再弯曲约束，最后碰撞。
        for (uint32_t iteration = 0; iteration < config.iterations; ++iteration)
        {
            ProjectLengthsForGuide(guide);
            ProjectBendForGuide(guide);
            ProjectCollisionsForGuide(guide);
        }
    }
}

void GuideSimulation::Step(float dt)
{
    if (paused)
        return;
    accumulator += std::clamp(dt, 0.0f, config.maxDt);
    uint32_t steps = 0;
    while (accumulator >= config.fixedStep && steps < config.maxSubsteps)
    {
        Substep();
        accumulator -= config.fixedStep;
        ++steps;
    }
    // 达到上限说明本帧的累计时间无法在预算内消费完；丢弃 backlog，避免越积越慢。
    if (steps == config.maxSubsteps)
        accumulator = 0.0f;
    simulationTime += static_cast<float>(steps) * config.fixedStep;
}

} // namespace VL::Hair
