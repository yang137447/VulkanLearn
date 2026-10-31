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
    // 场景风向量（厘米/秒）：direction * strength，由 RenderableObject 每帧在
    // StepFrame 之前刷新。CPU 参考 backend 只消费方向/强度：游戏线程不拥有渲染
    // 线程的阵风时钟，所以这里不复制场景阵风，也不另造一个时钟；阵风相位由
    // GPU 路径在 renderScene.windTimeSeconds 处施加。
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    // 物理阻力需要的作者参数：场景风只提供空气速度，gain/半径决定阻力率。
    float windGain = 0.01f;
    float strandRadius = 0.012f;
    // TressFX 的 DampingCoeff，直接用于 exp(-damping*dt*60)，典型 0..0.07。
    float damping = 0.035f;
    uint32_t iterations = 3;
    uint32_t rootFixedPoints = 2;
    // 每帧一个变量步长；0.016 s 上限取自源 Integration 参数函数。
    // GPU 原生调度与 StepFrame 已不再使用它（固定 0.0125 s/substep），
    // 只为兼容既有资产与旧 Step 入口保留。
    float maxDt = TressfxMaxTimeStepSeconds;
    // 映射到 LocalShape pass 的 Stiffness 标量；pass 内再乘 0.5。
    float bendStiffness = 0.3f;
    // TressFX Integration 的 global shape，两者都 > 0 才启用。
    float globalShapeStiffness = 0.0f;
    float globalShapeEffectiveRange = 0.0f;
    // TressFX VSP；vspCoefficient == 0 时 CPU/GPU 都跳过 VSP pass。
    float vspCoefficient = 0.0f;
    // 二阶位置差幅值阈值，厘米；名字沿用源 Accel，但没有除 dt²。
    float vspAccelThresholdMax = TressfxDefaultVspAccelThresholdMaxCm;
    // 可选作者逐顶点曲线。空 = 未作者：VSP 用根前缀→梢部线性近似，LocalShape
    // 用 bendStiffness 标量。非空时长度必须等于 pointsPerGuide（加载期已按
    // .tfx 的 pointsPerStrand 校验）；LocalShape 的 >0.95 值已在加载期 clamp。
    std::vector<float> vspStiffness;
    std::vector<float> localShapeStiffness;
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
    // 分组求解入口：groupConfigs[i] 已按 base -> group i -> placement 合成，
    // guideGroupIds[guide] 选择配置；缺项/越界回退索引 0。
    GuideSimulation(
        std::vector<Eigen::Vector3f> restPositions,
        size_t guideCount,
        size_t pointsPerGuide,
        std::vector<float> inverseMass,
        std::vector<SimulationConfig> groupConfigs,
        std::vector<uint32_t> guideGroupIds);

    void Reset();
    // 组编辑后复用对象：只替换配置表与 guide->group 映射，不重置求解历史、
    // 当前/上一帧位置或模拟帧计数。configs 必须非空，ids 必须与 guide 数一致。
    void RebindGroups(std::vector<SimulationConfig> configs,
        std::vector<uint32_t> guideGroupIds);
    // 诊断/测试读取：返回该 guide 实际使用的组配置（越界回退索引 0）。
    const SimulationConfig& ConfigForGuide(size_t guide) const
    {
        return GuideConfig(guide);
    }
    // 每帧在 StepFrame 之前刷新场景风向量（厘米/秒）。只改方向/强度，不触碰
    // 历史位置或仿真帧计数；调用方是 RenderableObject 的游戏线程步进入口。
    void SetSceneWind(const Eigen::Vector3f& windVelocityCmPerSec)
    {
        config.wind = windVelocityCmPerSec;
        // 风是对象级输入，但每个组配置都保存一份标量副本；只改 base 会让
        // 分组求解读到旧风向量。
        for (SimulationConfig& groupConfig : groupConfigs)
            groupConfig.wind = windVelocityCmPerSec;
    }
    // 旧的可变步长入口：dt 被 clamp 到 [0, maxDt]，一帧只推进一次。
    // 保留给现有测试/逐变量步长调用；运行时改走 StepFrame。
    void Step(float dt);
    // TressFX 5.0 源上层调度：按帧增量得到 0/1/2 个 substep，每个 substep
    // 固定 0.0125 s；simulationFrame 每帧只 +1（同一帧所有 substep 共用风相位）。
    void StepFrame(float deltaSeconds);
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
    const SimulationConfig& GuideConfig(size_t guide) const;

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
    // index 0 同时是对象级参数（maxDt 等）的基准；组差异只允许 6 个组字段。
    std::vector<SimulationConfig> groupConfigs;
    std::vector<uint32_t> guideGroupIds;
    SimulationConfig config;
    bool paused = false;
    // 每个 substep 结束后累加 solver step；StepFrame 一帧可累加 0/1/2 次。
    float simulationTime = 0.0f;
    // TressFX Guides.Frame 的计数镜像：StepFrame 每帧 +1、Step 每次调用 +1。
    // 运行时风幅值不再按它调制（场景阵风接管时间调制）；保留计数是为了与源
    // 调度的相位/帧序列保持可对照，并供测试检查“每帧只 +1”。
    uint32_t simulationFrame = 0;
};

} // namespace VL::Hair
