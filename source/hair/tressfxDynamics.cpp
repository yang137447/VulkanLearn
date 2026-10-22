#include "hair/tressfxDynamics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Hair
{

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
        config.iterations == 0)
        throw std::invalid_argument("Invalid TressFX guide simulation config");
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
}

void GuideSimulation::ProjectLengths()
{
    for (uint32_t iteration = 0; iteration < config.iterations; ++iteration)
    {
        for (size_t guide = 0; guide < guideCount; ++guide)
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
    }
}

void GuideSimulation::Step(float dt)
{
    if (paused)
        return;
    dt = std::clamp(dt, 0.0f, config.maxDt);
    if (dt <= 0.0f)
        return;
    const float dt2 = dt * dt;
    const Eigen::Vector3f acceleration = config.gravity + config.wind;
    for (size_t point = 0; point < positions.size(); ++point)
    {
        if (IsFixed(point))
        {
            positions[point] = restPositions[point];
            previousPositions[point] = restPositions[point];
            continue;
        }
        const Eigen::Vector3f current = positions[point];
        const Eigen::Vector3f velocity = (positions[point] - previousPositions[point]) * config.damping;
        previousPositions[point] = current;
        positions[point] = current + velocity + acceleration * dt2;
    }
    ProjectLengths();
}

} // namespace VL::Hair
