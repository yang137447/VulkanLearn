#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

// TressFX 5.0 guide solver 的纯数学镜像。
//
// 这些函数被 CPU 参考 solver（VL::Hair::GuideSimulation）和模块测试共同使用，
// GPU 侧 shader/glsl/hair/tressfxSim*.comp 必须保持同一公式与同一分支顺序。
// 长度与轴向的主要适配如下；上层调度和作者曲线的差异见资产契约：
// - 单位：TressFX 在米制下工作（rest 位置乘 0.01、速度 clamp 0.3 m）；VL 全链路
//   使用厘米，速度 clamp 为 30 cm，风三叉积另作量纲换算。
// - 坐标系：TressFX 的重力是 GravityMagnitude * (0,0,-1)（Z-up）；VL 直接使用
//   作者资产的 gravity 向量（Y-up）。
namespace VL::Hair
{
// TressFX 的 ClampPositionDelta = 0.3 m；VL 使用厘米，对应 30 cm。
inline constexpr float TressfxVelocityClampCm = 30.0f;

// TressFX 的 DampingCoeff 以 60 Hz 为参考写成 exp(-DampingCoeff * TimeStep * 60)。
inline constexpr float TressfxDampingReferenceHz = 60.0f;

// 源 Integration 参数函数把输入限制到 0.016 s；VL 用它限制自己的变量步长。
// 源上层另有 0.0125 s/max-2 循环，这里不把该 clamp 当成源完整调度策略。
inline constexpr float TressfxMaxTimeStepSeconds = 0.016f;

// TressFX SetWind 的风锥半角：绕 Y/Z 轴各 ±40°。
inline constexpr float TressfxWindConeAngleRadians = 0.69813170079773179f;

// 风的 cm→m 单位适配系数，推导见 TressfxWindForce。
inline constexpr float TressfxWindForceScale = 1.0e-2f;

// TressFX 的 VSP 系数小于 0.9 时才乘以逐顶点 stiffness。
inline constexpr float TressfxVspStiffnessBlendThreshold = 0.9f;

// 源默认 1.4 world m；比较量是 |new - 2*old + oldOld|，没有除 dt²，
// 因此换到 VL 厘米后是 140 cm，而不是厘米/秒²。
inline constexpr float TressfxDefaultVspAccelThresholdMaxCm = 140.0f;

// TressFX 的 rest 位置与 prev/current 位置用 w != 0 表示可移动（IsMovable）。
inline bool TressfxIsMovable(float w)
{
    return w != 0.0f;
}

// TressFX 在 Integration pass 里把 CurrentPos.w 置 0 的点都视为不可移动。
// VL 的不可移动集合是 TressFX 标记的超集：根前缀（rootFixedPoints）或导入逆质量
// <= 0 都写 w = 0；这个判断与 GPU pass 的 movable 计算一一对应。
inline bool TressfxIsImmovable(uint32_t localPoint, uint32_t rootFixedPoints,
    float inverseMass)
{
    return localPoint < rootFixedPoints || inverseMass <= 0.0f;
}

// TressFX Integrate：Decay = exp(-DampingCoeff * TimeStep * 60)。
// 作者字段 damping 直接就是这里的 DampingCoeff，不做 retention 变换：
// TressFXCoreSimulation.cpp 写的是 `Parameters->DampingCoeff = Guides.Damping`，
// 资产默认 0.035（UI 0..0.07），不是“每步保留的速度比例”。
inline float TressfxDampingDecay(float dampingCoeff, float timeStep)
{
    return std::exp(-dampingCoeff * timeStep * TressfxDampingReferenceHz);
}

// TressFX 的 q = (xyz = 旋转轴 * sin(theta/2), w = cos(theta/2))。
// 这里用 Eigen::Vector4f 存 (x, y, z, w)。返回从 u 转到 v 的最短弧四元数，
// 与 TressFXSimulationCommon.ush 的 QuatFromTwoUnitVectors 同式：
//   r = dot(u, v) + 1
//   r < 1e-6（u 与 v 反向）时取与 u 垂直的确定性轴，r 置 0
//   否则 q = normalize((cross(u, v), r))
// 退化（零向量）调用方负责跳过；shader 侧用同样的显式分支保护。
inline Eigen::Vector4f TressfxQuatFromTwoUnitVectors(
    const Eigen::Vector3f& u, const Eigen::Vector3f& v)
{
    float r = u.dot(v) + 1.0f;
    Eigen::Vector4f q;
    if (r < 1.0e-6f)
    {
        r = 0.0f;
        if (std::abs(u.x()) > std::abs(u.z()))
            q = Eigen::Vector4f(-u.y(), u.x(), 0.0f, r);
        else
            q = Eigen::Vector4f(0.0f, -u.z(), u.y(), r);
    }
    else
    {
        const Eigen::Vector3f axis = u.cross(v);
        q = Eigen::Vector4f(axis.x(), axis.y(), axis.z(), r);
    }
    return q.normalized();
}

// TressFX MultQuaternionAndVector：v' = v + 2*q.w*cross(q.xyz, v)
//                                      + 2*cross(q.xyz, cross(q.xyz, v))。
inline Eigen::Vector3f TressfxRotateByQuat(
    const Eigen::Vector4f& q, const Eigen::Vector3f& v)
{
    const Eigen::Vector3f qv(q.x(), q.y(), q.z());
    const Eigen::Vector3f t = 2.0f * qv.cross(v);
    return v + q.w() * t + qv.cross(t);
}

// 从刚体骨骼矩阵的上 3x3 提取旋转四元数（w 在前再换到 (x,y,z,w)）。
// TressFX 的 SkinningQuat 来自骨骼矩阵的 MakeQuaternion；VL 只有单个根骨矩阵，
// 所以 Host 侧提取一次放进 params，shader 不再做矩阵→四元数分解。
inline Eigen::Vector4f TressfxQuaternionFromMatrix(const Eigen::Matrix4f& matrix)
{
    Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    // 缩放/剪切不在蒙皮矩阵合同内；归一化只吸收 float 舍入，不改变刚体旋转。
    if (std::abs(rotation.determinant()) > 1.0e-12f)
        rotation = rotation / std::cbrt(rotation.determinant());
    Eigen::Quaternionf quaternion(rotation);
    quaternion.normalize();
    return Eigen::Vector4f(quaternion.x(), quaternion.y(), quaternion.z(),
        quaternion.w());
}

// TressFX LengthConstraintsWind 的四风混合。a 只取决于 strand 下标：
//   w = a*Wind0 + (1-a)*Wind1 + a*Wind2 + (1-a)*Wind3,  a = (strand % 20) / 20
// 权重和是 2，四路风向不同，所以混合结果既不是单一平均风，也不是把作者幅值
// 原样乘 2；每个 strand 落在一个锥形风向上的插值里。公式逐字保留。
inline Eigen::Vector3f TressfxCombineWind(const Eigen::Vector3f& wind0,
    const Eigen::Vector3f& wind1, const Eigen::Vector3f& wind2,
    const Eigen::Vector3f& wind3, uint32_t guideIndex)
{
    const float a = static_cast<float>(guideIndex % 20u) / 20.0f;
    return a * wind0 + (1.0f - a) * wind1 + a * wind2 + (1.0f - a) * wind3;
}

// TressFX SetWind 的逐帧幅值调制：wM = windMag * (sin(frame * 0.001)² + 0.5)。
// frame 是仿真帧序号（不是秒），在 0.5..1.5 倍之间缓慢起伏。VL 的偏差：运行时的
// 时间调制已交给场景阵风（见 world/worldWindSampling.h），GPU 与 CPU 运行路径都
// 不再调用本函数，否则会与共享阵风叠加成双重调制。只保留给 legacy/SDK 对照入口
// TressfxBuildWindCorners(direction, magnitude, frame) 与离线/单元测试。
inline float TressfxWindFrameModulation(uint32_t frame)
{
    const float phase = std::sin(static_cast<float>(frame) * 0.001f);
    return phase * phase + 0.5f;
}

// 把 X 轴绕给定轴旋转 angle 弧度；对应 SetWindCorner 里的
// `rot(rotAxis, angleToWideWindCone) * XAxis`。
inline Eigen::Vector3f TressfxConeAxis(
    const Eigen::Matrix3f& xAxisToWind, const Eigen::Vector3f& axis, float angle)
{
    const Eigen::Vector3f rotated =
        Eigen::AngleAxisf(angle, axis) * Eigen::Vector3f::UnitX();
    return xAxisToWind * rotated;
}

// TressFX 的四路锥形风。windDirection 只取方向（函数内归一化），windMagnitude 是
// TressFX 的 WindMagnitude 力标量：VL 直接把场景风强度（cm/s）的数值当它用，不是
// 物理风速。四路是风方向分别绕 Y 轴 ±40°、绕 Z 轴 ±40° 的四个角，再统一乘风幅值。
// 本重载不做时间调制：运行时的风幅值已由场景风（含共享阵风）算好。
struct TressfxWindCorners
{
    Eigen::Vector3f wind0 = Eigen::Vector3f::Zero();
    Eigen::Vector3f wind1 = Eigen::Vector3f::Zero();
    Eigen::Vector3f wind2 = Eigen::Vector3f::Zero();
    Eigen::Vector3f wind3 = Eigen::Vector3f::Zero();
};

inline TressfxWindCorners TressfxBuildWindCorners(
    const Eigen::Vector3f& windDirection, float windMagnitude)
{
    TressfxWindCorners corners;
    if (windMagnitude <= 0.0f || windDirection.squaredNorm() <= 1.0e-12f)
        return corners;

    const float wM = windMagnitude;
    const Eigen::Vector3f direction = windDirection.normalized();
    // 参考实现用 asin(|X × dir|) 构造 X→dir 的旋转，只对 dir.x >= 0 正确；
    // 这里用精确最短弧，二者在已校准的 +X 风上完全一致，同时不会把 -X 风翻向。
    const Eigen::Matrix3f xAxisToWind =
        Eigen::Quaternionf::FromTwoVectors(
            Eigen::Vector3f::UnitX(), direction).toRotationMatrix();

    corners.wind0 = wM * TressfxConeAxis(xAxisToWind,
        Eigen::Vector3f::UnitY(), TressfxWindConeAngleRadians);
    corners.wind1 = wM * TressfxConeAxis(xAxisToWind,
        Eigen::Vector3f::UnitY(), -TressfxWindConeAngleRadians);
    corners.wind2 = wM * TressfxConeAxis(xAxisToWind,
        Eigen::Vector3f::UnitZ(), TressfxWindConeAngleRadians);
    corners.wind3 = wM * TressfxConeAxis(xAxisToWind,
        Eigen::Vector3f::UnitZ(), -TressfxWindConeAngleRadians);
    return corners;
}

// legacy/SDK 对照入口：保留 TressFX 5.0 SetWind 的逐帧幅值调制
// （wM = |wind| * (sin(frame*0.001)² + 0.5)）。运行时（HairSimulationPass 与
// CPU 参考 solver）都不调用它：场景阵风已经负责时间调制，两边叠加会双重调制。
inline TressfxWindCorners TressfxBuildWindCorners(
    const Eigen::Vector3f& windDirection, float windMagnitude, uint32_t frame)
{
    return TressfxBuildWindCorners(
        windDirection, windMagnitude * TressfxWindFrameModulation(frame));
}

// TressFX 的风力：Force = -cross(cross(v, w), v)，v 是**未归一化**的段向量
// （pos_i - pos_{i+1}）。结果垂直于 v：只把发丝段往风向上推，不沿段方向拉伸。
//
// 单位适配（合同里记为 deviation）：源实现在米制下积分，VL 的位置和段向量都是
// 厘米。TressFX 的 WindMagnitude 是*力标量*，不是风速，两种单位下取同一个数：
// v_cm = 100·v_m ⇒ |v_cm|² = 1e4·|v_m|²，而厘米位移是米位移的 100 倍，因此
//   cross(cross(v_cm,w),v_cm) 需乘 1e-2 才等于 100·cross(cross(v_m,w),v_m)。
// 乘 1e-2 后作者幅值与 UE 的 WindMagnitude 可直接对照；段长平方量纲必须保留，
// 否则约 1 cm 的段会得到完全不同的风力。
inline Eigen::Vector3f TressfxWindForce(
    const Eigen::Vector3f& segment, const Eigen::Vector3f& wind)
{
    const Eigen::Vector3f crossSegmentWind = segment.cross(wind);
    return -crossSegmentWind.cross(segment) * TressfxWindForceScale;
}

// TressFX ConstraintMultiplier：movable 组合决定 0.5/0.5、1/0、0/1、0/0。
inline Eigen::Vector2f TressfxConstraintMultiplier(bool movable0, bool movable1)
{
    if (movable0)
        return movable1 ? Eigen::Vector2f(0.5f, 0.5f) : Eigen::Vector2f(1.0f, 0.0f);
    return movable1 ? Eigen::Vector2f(0.0f, 1.0f) : Eigen::Vector2f(0.0f, 0.0f);
}

// TressFX ApplyDistanceConstraint（stiffness 固定 1）：
//   delta = pos1 - pos0
//   distance = max(|delta|, 1e-7)
//   delta *= 1 - targetDistance / distance
//   pos0 += m0 * delta;  pos1 -= m1 * delta
inline void TressfxApplyDistanceConstraint(Eigen::Vector3f& pos0, bool movable0,
    Eigen::Vector3f& pos1, bool movable1, float targetDistance)
{
    Eigen::Vector3f delta = pos1 - pos0;
    const float distance = std::max(delta.norm(), 1.0e-7f);
    const float stretching = 1.0f - targetDistance / distance;
    delta *= stretching;
    const Eigen::Vector2f multiplier =
        TressfxConstraintMultiplier(movable0, movable1);
    pos0 += multiplier.x() * delta;
    pos1 -= multiplier.y() * delta;
}

// TressFX Integration 的 global shape gate：
//   GlobalShapeStiffness > 0 && GlobalShapeEffectiveRange > 0 &&
//   localVertex < GlobalShapeEffectiveRange * NumVerticesInTheStrand
inline bool TressfxGlobalShapeApplies(uint32_t localPoint, uint32_t pointCount,
    float stiffness, float effectiveRange)
{
    return stiffness > 0.0f && effectiveRange > 0.0f &&
        static_cast<float>(localPoint) < effectiveRange * static_cast<float>(pointCount);
}

// TressFX global shape：TempPos += GlobalShapeStiffness * (InitialPos - TempPos)。
inline Eigen::Vector3f TressfxGlobalShapePull(const Eigen::Vector3f& initialPos,
    const Eigen::Vector3f& tempPos, float stiffness)
{
    return tempPos + stiffness * (initialPos - tempPos);
}

// TressFX 的 VSPStiffness 是作者曲线（DECLARE_SCALAR_ARRAY 最多 64 项），
// 第 i 项 = 曲线在 i/N 处的值（TressFXComponent.cpp:687-691）。authored 非空且
// 覆盖 localPoint 时直接使用作者值；否则沿用从根前缀（1）到梢部（0）的线性
// 近似（VL 没有曲线作者面时的历史回退）。只有 VspCoeff < 0.9 时才参与混合。
inline float TressfxVspStiffness(uint32_t localPoint, uint32_t pointCount,
    uint32_t rootFixedPoints, const std::vector<float>* authored = nullptr)
{
    if (authored != nullptr && localPoint < authored->size())
        return (*authored)[localPoint];
    const float span = static_cast<float>(pointCount) - 1.0f -
        static_cast<float>(rootFixedPoints);
    if (span <= 0.0f)
        return 0.0f;
    const float t = std::clamp(
        (static_cast<float>(localPoint) - static_cast<float>(rootFixedPoints)) / span,
        0.0f, 1.0f);
    return 1.0f - t;
}

// 源 VSP pass 的系数：VspCoeff < 0.9 时乘逐顶点 VSPStiffness，否则保持原系数
// （patch TressFXSimulationVSP.usf:10153-10160）。CPU 与 shader 共用同一条件。
inline float TressfxVspCoefficient(float coefficient, uint32_t localPoint,
    uint32_t pointCount, uint32_t rootFixedPoints,
    const std::vector<float>* authored = nullptr)
{
    if (coefficient >= TressfxVspStiffnessBlendThreshold)
        return coefficient;
    return coefficient * TressfxVspStiffness(
        localPoint, pointCount, rootFixedPoints, authored);
}

// LocalShape 的逐顶点 Stiffness[i]：作者数组覆盖该点时用数组值（加载期已按源
// min(value, 0.95) 处理），否则回退到整条 strand 的 bendStiffness 标量。
// 调用方（LocalShapeDelta）仍按 TressFX 公式乘 0.5。
inline float TressfxLocalShapeStiffness(uint32_t localPoint, float fallback,
    const std::vector<float>* authored = nullptr)
{
    if (authored != nullptr && localPoint < authored->size())
        return (*authored)[localPoint];
    return fallback;
}

// TressFX VSP 混合：new = (1-coeff)*pos + coeff*(Quat*pos + trans)。
inline Eigen::Vector3f TressfxVspBlend(const Eigen::Vector3f& position,
    const Eigen::Vector4f& quat, const Eigen::Vector3f& translation, float coefficient)
{
    return (1.0f - coefficient) * position +
        coefficient * (TressfxRotateByQuat(quat, position) + translation);
}

// strand-level pass 的输出：VspQuat、VspTranslation、VspCoeff。
struct TressfxStrandTransform
{
    Eigen::Vector4f quat = Eigen::Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    float coefficient = 0.0f;
};

// TressFX CalculateStrandLevelData：用前两个点的 previous/current 位置求刚体变换，
// 再用第二点的伪加速度 length(new - 2*old + old_old) 与阈值决定是否强制 VspCoeff=1。
// 前两点重合时 TressFX 会 normalize(0) 产生 NaN；VL 资产导入已拒绝相邻重合点，
// 这里仍退化为单位旋转，避免异常资产把 NaN 写进 SSBO。
inline TressfxStrandTransform TressfxComputeStrandTransform(
    const Eigen::Vector3f& old0, const Eigen::Vector3f& old1,
    const Eigen::Vector3f& new0, const Eigen::Vector3f& new1,
    const Eigen::Vector3f& oldOld1, float baseCoefficient,
    float accelThreshold)
{
    TressfxStrandTransform transform;
    const Eigen::Vector3f u = old1 - old0;
    const Eigen::Vector3f v = new1 - new0;
    if (u.squaredNorm() > 1.0e-12f && v.squaredNorm() > 1.0e-12f)
    {
        transform.quat = TressfxQuatFromTwoUnitVectors(u.normalized(), v.normalized());
        transform.translation = new0 -
            TressfxRotateByQuat(transform.quat, old0);
    }
    transform.coefficient = baseCoefficient;
    // 与源实现相同的伪加速度（不是真实加速度，只是二阶差分幅值）。
    const Eigen::Vector3f pseudoAcceleration = new1 - 2.0f * old1 + oldOld1;
    if (pseudoAcceleration.norm() > accelThreshold)
        transform.coefficient = 1.0f;
    return transform;
}

// TressFX LocalShapeConstraints 的局部旋转：把 bind-pose 段方向 (i-1 -> i) 旋转到
// 当前段方向，再把 bind-pose 段方向 (i -> i+1) 一起旋转，得到 i+1 的目标位置。
// 返回的是 del（尚未乘 0.5*Stiffness 之后的量已包含），调用方按可移动性分配给 i/i+1。
inline Eigen::Vector3f TressfxLocalShapeDelta(const Eigen::Vector4f& skinningQuat,
    const Eigen::Vector3f& restMinusOne, const Eigen::Vector3f& rest,
    const Eigen::Vector3f& restPlusOne, const Eigen::Vector3f& posMinusOne,
    const Eigen::Vector3f& pos, const Eigen::Vector3f& posPlusOne, float stiffness)
{
    const Eigen::Vector3f bindPos = TressfxRotateByQuat(skinningQuat, rest);
    const Eigen::Vector3f bindPosPlusOne =
        TressfxRotateByQuat(skinningQuat, restPlusOne);
    const Eigen::Vector3f bindPosMinusOne =
        TressfxRotateByQuat(skinningQuat, restMinusOne);

    const Eigen::Vector3f lastVec = pos - posMinusOne;
    const Eigen::Vector3f vecBindPose = bindPosPlusOne - bindPos;
    const Eigen::Vector3f lastVecBindPose = bindPos - bindPosMinusOne;
    if (lastVec.squaredNorm() <= 1.0e-12f || lastVecBindPose.squaredNorm() <= 1.0e-12f)
        return Eigen::Vector3f::Zero();

    const Eigen::Vector4f rotGlobal = TressfxQuatFromTwoUnitVectors(
        lastVecBindPose.normalized(), lastVec.normalized());
    const Eigen::Vector3f target = TressfxRotateByQuat(rotGlobal, vecBindPose) + pos;
    // TressFX 的 StiffnessForLocalShapeMatching = 0.5 * Stiffness[i]。
    return 0.5f * stiffness * (target - posPlusOne);
}

// TressFX 的速度 clamp：不直接限制当前位置，而是重写 previous 位置。源实现的
// 缩放是 `PositionDelta *= Clamp² / SpeedSqr`（注意分母是速度平方），因此结果
// 速度是 Clamp² / |delta|，而不是严格等于 Clamp。这是源实现的既有 quirk，VL
// 逐字保留；Clamp 只作为平方幅值的上界使用。返回是否发生重写。
inline bool TressfxClampPositionDelta(
    Eigen::Vector3f& previous, const Eigen::Vector3f& position,
    float maxDelta = TressfxVelocityClampCm)
{
    Eigen::Vector3f delta = position - previous;
    const float speedSquared = delta.squaredNorm();
    if (speedSquared <= maxDelta * maxDelta)
        return false;
    delta *= (maxDelta * maxDelta) / speedSquared;
    previous = position - delta;
    return true;
}

} // namespace VL::Hair
