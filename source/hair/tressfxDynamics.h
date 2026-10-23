#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxCollision.h"
#include "hair/tressfxSolverMath.h"

namespace VL::Hair
{
// CPU 参考 solver 的作者配置。字段与 meshAssetTypes.h 的 TressfxSimulationConfig
// 一一对应（GPU 与 CPU 消费同一份资产），但不含 skinning：CPU backend 没有
// 渲染线程的 SkinPose，加载期已拒绝这个组合。
struct SimulationConfig
{
    Eigen::Vector3f gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    // 单个作者风向量：方向归一化，长度作为幅值（厘米/秒）；CPU/GPU 都按
    // TressFX SetWind 展开成四路锥形风。
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    // TressFX 的 DampingCoeff，直接用于 exp(-damping*dt*60)，典型 0..0.07。
    float damping = 0.035f;
    uint32_t iterations = 3;
    uint32_t rootFixedPoints = 2;
    // 变量步长上限；TressFX 的硬上限是 0.016 s，每帧一个 solver step。
    float maxDt = TressfxMaxTimeStepSeconds;
    // 映射到 LocalShape pass 的 Stiffness 标量；pass 内再乘 0.5。
    float bendStiffness = 0.3f;
    // TressFX Integration 的 global shape，两者都 > 0 才启用。
    float globalShapeStiffness = 0.0f;
    float globalShapeEffectiveRange = 0.0f;
    // TressFX VSP；vspCoefficient == 0 时 CPU/GPU 都跳过 VSP pass。
    float vspCoefficient = 0.0f;
    float vspAccelThresholdMax = 5000.0f;
    // 资产局部空间的解析胶囊；顺序即重叠时的投影顺序。
    std::vector<TressfxCollider> capsules;
};

// GPU 多 pass guide pipeline 的确定性 CPU 镜像：Integration + GlobalShape、
// StrandLevel、VSP、LocalShape、LengthConstraintsWind，最后是 VL 自己的胶囊投影。
// 它按 guide 串行执行各 pass（GPU 的 vertex-level pass 是一点一线程），数学与
// 分支顺序相同；没有蒙皮矩阵，InitialPos 直接取 rest。
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
    // dt 被 clamp 到 [0, maxDt]；一帧只推进一次，不再消费固定步 accumulator。
    void Step(float dt);
    void SetPaused(bool value) { paused = value; }
    bool IsPaused() const { return paused; }
    const std::vector<Eigen::Vector3f>& Positions() const { return positions; }
    const std::vector<Eigen::Vector3f>& PreviousPositions() const { return previousPositions; }
    const std::vector<Eigen::Vector3f>& PreviousPreviousPositions() const
    {
        return previousPreviousPositions;
    }
    float SimulationTime() const { return simulationTime; }
    uint32_t SimulationFrame() const { return simulationFrame; }

private:
    // GPU 的 StrandLevel pass 写成 per-strand buffer；CPU 侧在同一根 guide 的
    // 连续 pass 之间用局部变量传递同样的数据。
    struct StrandTransform
    {
        Eigen::Vector4f quat = Eigen::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
        Eigen::Vector3f translation = Eigen::Vector3f::Zero();
        float coefficient = 0.0f;
    };

    void SolveStrand(size_t guide, float timeStep);
    void IntegratePass(size_t guide, float timeStep, float decay);
    StrandTransform StrandLevelPass(size_t guide) const;
    void VspPass(size_t guide, const StrandTransform& transform);
    void LocalShapePass(size_t guide, const StrandTransform& transform);
    void LengthAndWindPass(size_t guide, float timeStep);
    void CollisionPass(size_t guide);

    std::vector<Eigen::Vector3f> restPositions;
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> previousPositions;
    std::vector<Eigen::Vector3f> previousPreviousPositions;
    std::vector<float> inverseMass;
    // 与 GPU 的 rest length buffer 相同：第 i 项是 i -> i+1 的静止长度，末项 0。
    std::vector<float> restLengths;
    // TressFX 的 IsMovable 标记（w != 0）：根前缀或逆质量 <= 0 为 false。
    std::vector<uint8_t> movable;
    size_t guideCount = 0;
    size_t pointsPerGuide = 0;
    SimulationConfig config;
    bool paused = false;
    // 每帧 solver step 结束后推进一次；同一帧所有 pass 共用 frame-start t。
    float simulationTime = 0.0f;
    // TressFX Guides.Frame：逐帧 wind 幅值调制 sin(frame*0.001)² 的相位来源。
    uint32_t simulationFrame = 0;
};

} // namespace VL::Hair
