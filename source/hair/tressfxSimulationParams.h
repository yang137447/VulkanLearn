#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <Eigen/Dense>

#include "hair/tressfxSolverMath.h"
#include "mesh/meshAssetTypes.h"
#include "world/sceneWindRuntime.h"

namespace VL::Hair
{
// 与 shader/glsl/hair/tressfxSimCommon.glsl 的 std140 uniform block 一一对应。
// GPU 原生调度每帧按源 n = 0/1/2 个 substep 录制，所有 substep 共用同一份正常
// 参数（dt 恒为 0.0125）；reset/换基的首个 Integration 用独立的 resetParams 副本。
// 资产的 maxDt 不再参与 GPU 路径。
struct GuideSimulationParams
{
    // xyz = 世界空间重力换算到当前 hair 局部后的加速度，w 未使用（厘米/秒²）。
    Eigen::Vector4f gravity = Eigen::Vector4f::Zero();
    // 场景空气速度的四路锥形分量（cm/s）。Host 在物理阻力路径中把场景风展开成
    // 锥形方向；TressfxCombineWind 负责归一化，shader 用法向相对速度算阻力。
    Eigen::Vector4f wind0 = Eigen::Vector4f::Zero();
    Eigen::Vector4f wind1 = Eigen::Vector4f::Zero();
    Eigen::Vector4f wind2 = Eigen::Vector4f::Zero();
    Eigen::Vector4f wind3 = Eigen::Vector4f::Zero();
    // x = dt（秒，源调度恒为 0.0125）, y = base DampingCoeff（仅供诊断；
    // 分组模拟以 binding 17 的 per-guide 配置为准）, z = pointsPerGuide,
    // w = rootFixedPoints
    Eigen::Vector4f controls = Eigen::Vector4f::Zero();
    // x = base globalShapeStiffness, y = base globalShapeEffectiveRange,
    // z = base vspCoefficient, w = base vspAccelThresholdMax（二阶位置差阈值，
    // 厘米，未除 dt²）。Phase 4 起这 4 项由 binding 17 的组配置驱动；此处保留
    // 816-byte 布局与 base 镜像，便于 ABI 对照和非分组诊断。
    Eigen::Vector4f shape = Eigen::Vector4f::Zero();
    // x = base bendStiffness（分组时以 binding 17 的组配置为准），y = ResetPositions；
    // z = 只换基三份历史（历史 VL 路径保留，GPU 原生调度已不再使用：n == 0 时
    // 不录 pass，换基并入首个正 delta substep 的 Integration）；w 的 bit0/bit1
    // = VSP / LocalShape 逐顶点作者数组存在，其余位保留 0。
    // wind0..3 只复制已经过场景阵风调制的本帧风向量，不含额外时间调制。
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
    // inverse(currentHairModel) * lastRecordedHairModel；只在 Integration 使用，
    // 位置的 w 是固定点标记，shader 换基时必须显式用齐次坐标 1。
    Eigen::Matrix4f historyToCurrentLocal = Eigen::Matrix4f::Identity();
    // TressFX 作者逐顶点 stiffness 曲线（最多 64 项），按 4 个 float 一组打包成
    // std140 的 vec4 数组；第 local 项在 [local / 4][local % 4]。motion.w 的
    // bit0/bit1 分别表示这两个数组存在；未作者时 shader 走各自的回退路径。
    std::array<Eigen::Vector4f, 16> vspStiffness = {};
    std::array<Eigen::Vector4f, 16> localShapeStiffness = {};
    // x = 每单位法向相对速度的阻力率（1/cm），由
    // TressfxHairDragRatePerSpeed(windGain, strandRadius) 预先算好；yzw 保留。
    Eigen::Vector4f windParams = Eigen::Vector4f::Zero();
};
static_assert(sizeof(GuideSimulationParams) == 816,
    "std140 guide simulation params must stay 816 bytes");
static_assert(alignof(GuideSimulationParams) == 16,
    "std140 guide simulation params require vec4 alignment");
// std140 中 mat4 必须落在 16-byte 边界；Host 与 shader 的偏移都从这里推导。
static_assert(offsetof(GuideSimulationParams, skinning) == 160,
    "std140 skinning matrix must start at offset 160");
static_assert(offsetof(GuideSimulationParams, historyToCurrentLocal) == 224,
    "std140 history matrix must start at offset 224");
// 两个 vec4 数组各 16 项（256 bytes），追加在 mat4 之后。
static_assert(offsetof(GuideSimulationParams, vspStiffness) == 288,
    "std140 VSP stiffness array must start at offset 288");
static_assert(offsetof(GuideSimulationParams, localShapeStiffness) == 544,
    "std140 LocalShape stiffness array must start at offset 544");

// 每个顶点 pass 的 invocation 处理一个点；每根 strand 的 pass 处理一根 guide。
// 两者与 shader 的 local_size_x 必须一致。
inline constexpr uint32_t GuideSimulationWorkgroupSize = 64;

// 参数 UBO 按 frame-in-flight 槽分开。数量必须等于 MAX_FRAMES_IN_FLIGHT。
inline constexpr uint32_t GuideSimulationParamBufferCount = 2;

// 关闭动力学的骨骼挂载只上传姿态与几何计数；不把风、约束、碰撞或 reset
// 带入蒙皮路径。整根发丝每帧由 rest 直接求值，因此不需要模拟时间和历史。
inline GuideSimulationParams MakeGuideSkinningParams(
    uint32_t guideCount,
    uint32_t pointsPerGuide,
    const Eigen::Matrix4f& skinningMatrix,
    bool hasStrandWeights)
{
    GuideSimulationParams params;
    params.controls.z() = static_cast<float>(pointsPerGuide);
    params.guideCount = guideCount;
    params.pointCount = guideCount * pointsPerGuide;
    params.skinning = skinningMatrix;
    params.motion.w() = hasStrandWeights ? 4.0f : 0.0f;
    return params;
}

// 把作者逐顶点 stiffness 曲线按 4 个 float 一组打包进 std140 的 vec4 数组。
// 非空数组长度由资产校验保证 == pointsPerGuide，这里仍按容量截断。
inline void PackGuideStiffnessCurve(const std::vector<float>& curve,
    std::array<Eigen::Vector4f, 16>& packed)
{
    for (Eigen::Vector4f& group : packed)
        group = Eigen::Vector4f::Zero();
    // 资产校验保证非空数组长度 == pointsPerStrand <= 64；这里仍按容量截断，
    // 防止外部直接调用把越界数据写进 UBO。
    const size_t count = std::min(curve.size(), size_t{64});
    for (size_t index = 0; index < count; ++index)
    {
        const Eigen::Index group = static_cast<Eigen::Index>(index / 4);
        const Eigen::Index component = static_cast<Eigen::Index>(index % 4);
        packed[static_cast<size_t>(group)][component] = curve[index];
    }
}

// 运行时场景风 -> TressFX 风向量（厘米/秒）。方向由 SceneWindRuntime 保证已归一化，
// 强度已含场景缓动与阵风；这里只做方向/幅值搬运，不再二次调制。零强度直接返回
// 零向量，既不归一化零向量，也让 shader 走零风早退。
inline Eigen::Vector3f TressfxSceneWindVelocityCmPerSec(
    const ResolvedSceneWind& wind)
{
    if (!(wind.strength > 0.0f))
        return Eigen::Vector3f::Zero();
    return wind.direction * wind.strength;
}

// 一份 pass 参数。dt 由调用方按源 substep 固定为 0.0125；同一帧的所有 substep
// 共用这一份参数。windVelocityCmPerSec 是本帧场景风向量（方向 * 幅值，厘米/秒，
// 已含场景阵风），Host 只负责 TressFX 的四角展开与换基，不再做时间调制。
inline GuideSimulationParams MakeGuideSimulationParams(
    const TressfxSimulationConfig& config,
    const Eigen::Vector3f& windVelocityCmPerSec,
    uint32_t guideCount,
    uint32_t pointsPerGuide,
    uint32_t pointCount,
    float timeStep,
    Eigen::Matrix4f skinningMatrix = Eigen::Matrix4f::Identity(),
    bool resetPositions = false,
    Eigen::Matrix4f historyToCurrentLocal = Eigen::Matrix4f::Identity(),
    bool rebaseOnly = false,
    Eigen::Matrix4f hairModel = Eigen::Matrix4f::Identity())
{
    GuideSimulationParams params;
    const Eigen::Matrix3f worldToHair = hairModel.block<3, 3>(0, 0).inverse();
    const Eigen::Vector3f localGravity = worldToHair * config.gravity;
    params.gravity = Eigen::Vector4f(
        localGravity.x(), localGravity.y(), localGravity.z(), 0.0f);
    // 四路风由场景风向量展开：方向归一化、长度作为幅值，再套 TressFX 的风锥。
    const float windMagnitude = windVelocityCmPerSec.norm();
    const TressfxWindCorners corners =
        TressfxBuildWindCorners(windVelocityCmPerSec, windMagnitude);
    // 锥角绕世界轴展开后再换基，不能先旋转单风向量再绕局部轴展开。
    // 刚体旋转时三叉积协变；scale 下沿用局部 solver 近似，不声称力/长度度量等价。
    params.wind0.head<3>() = worldToHair * corners.wind0;
    params.wind1.head<3>() = worldToHair * corners.wind1;
    params.wind2.head<3>() = worldToHair * corners.wind2;
    params.wind3.head<3>() = worldToHair * corners.wind3;
    // 作者 damping 就是 TressFX 的 DampingCoeff，直接进 controls.y。
    params.controls = Eigen::Vector4f(
        timeStep,
        config.damping,
        static_cast<float>(pointsPerGuide),
        static_cast<float>(config.rootFixedPoints));
    params.shape = Eigen::Vector4f(
        config.globalShapeStiffness,
        config.globalShapeEffectiveRange,
        config.vspCoefficient,
        config.vspAccelThresholdMax);
    // motion.w 的 bit0/bit1 是作者数组存在标记（见 struct 注释）；其余位保留 0。
    const bool hasVspStiffness = !config.vspStiffness.empty();
    const bool hasLocalShapeStiffness = !config.localShapeStiffness.empty();
    const float stiffnessFlags = (hasVspStiffness ? 1.0f : 0.0f) +
        (hasLocalShapeStiffness ? 2.0f : 0.0f);
    params.motion = Eigen::Vector4f(
        config.bendStiffness, resetPositions ? 1.0f : 0.0f,
        rebaseOnly ? 1.0f : 0.0f, stiffnessFlags);
    PackGuideStiffnessCurve(config.vspStiffness, params.vspStiffness);
    PackGuideStiffnessCurve(config.localShapeStiffness,
        params.localShapeStiffness);
    params.iterations = config.iterations;
    params.guideCount = guideCount;
    params.pointCount = pointCount;
    params.colliderCount = static_cast<uint32_t>(config.capsules.size());
    params.skinningQuat = TressfxQuaternionFromMatrix(skinningMatrix);
    params.skinning = skinningMatrix;
    params.historyToCurrentLocal = historyToCurrentLocal;
    // 物理阻力：Host 只传预计算的 1/cm 系数，shader 用当时的法向相对速度
    // 现算 beta；段长和发丝质量已在系数推导中约掉。
    params.windParams.x() = TressfxHairDragRatePerSpeed(
        config.windGain, config.strandRadius);
    return params;
}

} // namespace VL::Hair
