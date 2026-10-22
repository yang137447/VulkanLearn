#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

namespace VL::Hair
{

struct SimulationConfig
{
    Eigen::Vector3f gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    float damping = 0.995f;
    uint32_t iterations = 3;
    uint32_t rootFixedPoints = 2;
    float maxDt = 0.033f;
};

// Deterministic reference solver for the GPU TressFX guide pass. It uses
// Verlet integration and in-place distance projection per strand.
class GuideSimulation
{
public:
    GuideSimulation(
        std::vector<Eigen::Vector3f> restPositions,
        size_t guideCount,
        size_t pointsPerGuide,
        std::vector<float> inverseMass,
        SimulationConfig config);

    void Reset();
    void Step(float dt);
    void SetPaused(bool value) { paused = value; }
    bool IsPaused() const { return paused; }
    const std::vector<Eigen::Vector3f>& Positions() const { return positions; }
    const std::vector<Eigen::Vector3f>& PreviousPositions() const { return previousPositions; }

private:
    void ProjectLengths();
    bool IsFixed(size_t point) const;

    std::vector<Eigen::Vector3f> restPositions;
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> previousPositions;
    std::vector<float> inverseMass;
    size_t guideCount = 0;
    size_t pointsPerGuide = 0;
    SimulationConfig config;
    bool paused = false;
};

} // namespace VL::Hair
