#pragma once

#include <cstddef>
#include <cstdint>

#include <Eigen/Dense>

#include "hair/tressfxSolverMath.h"
#include "mesh/meshAssetTypes.h"

namespace VL::Hair
{
// 与 shader/glsl/hair/tressfxSimCommon.glsl 的 std140 uniform block 一一对应。
// 每个 tressfx 对象每帧写一次；dt 由 Host 按 clamp(frameDelta, 0, maxDt) 计算，
// 一帧只有一个 solver step（TressFX 的变量步长），不再有 fixedStep accumulator。
struct GuideSimulationParams
{
    // xyz = 重力加速度，w 未使用（厘米/秒²）。
    Eigen::Vector4f gravity = Eigen::Vector4f::Zero();
    // TressFX 的四路风；VL 资产只有一个平均风向量，Host 四路都填同一个值，
    // shader 仍按 a/(1-a) 权重混合（见 TressfxCombineWind）。
    Eigen::Vector4f wind0 = Eigen::Vector4f::Zero();
    Eigen::Vector4f wind1 = Eigen::Vector4f::Zero();
    Eigen::Vector4f wind2 = Eigen::Vector4f::Zero();
    Eigen::Vector4f wind3 = Eigen::Vector4f::Zero();
    // x = dt（秒）, y = DampingCoeff, z = pointsPerGuide, w = rootFixedPoints
    Eigen::Vector4f controls = Eigen::Vector4f::Zero();
    // x = globalShapeStiffness, y = globalShapeEffectiveRange,
    // z = vspCoefficient, w = vspAccelThresholdMax（厘米/秒²）
    Eigen::Vector4f shape = Eigen::Vector4f::Zero();
    // x = bendStiffness（映射到 LocalShape 的 Stiffness 标量）,
    // y = windGust, z = windFrequency, w = frame-start simulationTime
    Eigen::Vector4f motion = Eigen::Vector4f::Zero();
    // x = 长度约束迭代次数, y = guideCount, z = pointCount, w = colliderCount
    uint32_t iterations = 0;
    uint32_t guideCount = 0;
    uint32_t pointCount = 0;
    uint32_t colliderCount = 0;
    // 根骨旋转：(x,y,z) = 四元数向量部分, w = 标量部分。TressFX 的 SkinningQuat
    // 由每根 strand 的骨骼矩阵分解而来；VL 只有单个根骨，Host 预先分解一次。
    Eigen::Vector4f skinningQuat = Eigen::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
    // 根点骨骼矩阵：所有不可移动点写 InitialPos = M * rest（无蒙皮时为单位矩阵）。
    Eigen::Matrix4f skinning = Eigen::Matrix4f::Identity();
};
static_assert(sizeof(GuideSimulationParams) == 224,
    "std140 guide simulation params must stay 224 bytes");
static_assert(alignof(GuideSimulationParams) == 16,
    "std140 guide simulation params require vec4 alignment");
// std140 中 mat4 必须落在 16-byte 边界；Host 与 shader 的偏移都从这里推导。
static_assert(offsetof(GuideSimulationParams, skinning) == 160,
    "std140 skinning matrix must start at offset 160");

// 每个顶点 pass 的 invocation 处理一个点；每根 strand 的 pass 处理一根 guide。
// 两者与 shader 的 local_size_x 必须一致。
inline constexpr uint32_t GuideSimulationWorkgroupSize = 64;

// 参数 UBO 按 frame-in-flight 槽分开。数量必须等于 MAX_FRAMES_IN_FLIGHT。
inline constexpr uint32_t GuideSimulationParamBufferCount = 2;

// 一帧一次 dispatch 所需的参数。dt 已由调用方 clamp；simulationTime 是 frame-start t，
// 用于阵风相位（所有 pass 共用同一值）。
inline GuideSimulationParams MakeGuideSimulationParams(
    const TressfxSimulationConfig& config,
    uint32_t guideCount,
    uint32_t pointsPerGuide,
    uint32_t pointCount,
    float timeStep,
    float simulationTime,
    Eigen::Matrix4f skinningMatrix = Eigen::Matrix4f::Identity())
{
    GuideSimulationParams params;
    params.gravity = Eigen::Vector4f(
        config.gravity.x(), config.gravity.y(), config.gravity.z(), 0.0f);
    // 单一作者风向喂给 TressFX 的四路组合；四路相同则混合结果仍是平均值。
    const Eigen::Vector4f wind(
        config.wind.x(), config.wind.y(), config.wind.z(), 0.0f);
    params.wind0 = wind;
    params.wind1 = wind;
    params.wind2 = wind;
    params.wind3 = wind;
    params.controls = Eigen::Vector4f(
        timeStep,
        TressfxDampingCoeffFromRetention(config.damping),
        static_cast<float>(pointsPerGuide),
        static_cast<float>(config.rootFixedPoints));
    params.shape = Eigen::Vector4f(
        config.globalShapeStiffness,
        config.globalShapeEffectiveRange,
        config.vspCoefficient,
        config.vspAccelThresholdMax);
    params.motion = Eigen::Vector4f(
        config.bendStiffness,
        config.windGust,
        config.windFrequency,
        simulationTime);
    params.iterations = config.iterations;
    params.guideCount = guideCount;
    params.pointCount = pointCount;
    params.colliderCount = static_cast<uint32_t>(config.capsules.size());
    params.skinningQuat = TressfxQuaternionFromMatrix(skinningMatrix);
    params.skinning = skinningMatrix;
    return params;
}

} // namespace VL::Hair
