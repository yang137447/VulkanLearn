#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxCollision.h"

namespace VL::Hair
{

struct SimulationConfig
{
    Eigen::Vector3f gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    // 平均风；每帧叠加有界阵风后与重力相加。
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    float damping = 0.995f;
    uint32_t iterations = 3;
    uint32_t rootFixedPoints = 2;
    float maxDt = 0.033f;
    // 局部形状/弯曲约束强度；0 关闭。
    float bendStiffness = 0.3f;
    // gust = 1 + windGust * sin(2π * windFrequency * t + guide * 0.6180339)。
    float windGust = 0.5f;
    float windFrequency = 0.5f;
    // 固定子步长；accumulator 按它消耗，剩余不足一步时保留到下一帧。
    float fixedStep = 1.0f / 60.0f;
    uint32_t maxSubsteps = 4;
    // 资产局部空间的解析胶囊；顺序即重叠时的投影顺序。
    std::vector<TressfxCollider> capsules;
};

// 与 GPU kernel 相同的 golden-ratio 相位展开和有界阵风。
// 纯函数，供 CPU Substep 与模块测试共同使用；t 是该帧所有子步共享的 frame-start 时间。
float WindGustFactor(const SimulationConfig& config, size_t guideIndex, float simulationTime);
Eigen::Vector3f GuideAcceleration(
    const SimulationConfig& config,
    size_t guideIndex,
    float simulationTime);

// Deterministic reference solver for the GPU TressFX guide pass. It uses a
// fixed-step accumulator, Verlet integration, distance projection and a
// rest-shape bend constraint per strand.
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
    float SimulationTime() const { return simulationTime; }

private:
    void Substep();
    void ProjectLengthsForGuide(size_t guide);
    void ProjectBendForGuide(size_t guide);
    void ProjectCollisionsForGuide(size_t guide);
    bool IsFixed(size_t point) const;

    std::vector<Eigen::Vector3f> restPositions;
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> previousPositions;
    std::vector<float> inverseMass;
    size_t guideCount = 0;
    size_t pointsPerGuide = 0;
    SimulationConfig config;
    bool paused = false;
    // 帧粒度 accumulator：Step 内按 fixedStep 消费，达到 maxSubsteps 时丢弃 backlog。
    float accumulator = 0.0f;
    // 每帧子步结束后才推进；同一帧所有 Substep 共用同一个 frame-start t。
    float simulationTime = 0.0f;
};

} // namespace VL::Hair
