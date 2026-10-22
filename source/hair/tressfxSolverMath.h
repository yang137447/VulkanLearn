#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <Eigen/Dense>

// TressFX 5.0 guide solver 的纯数学镜像。
//
// 这些函数被 CPU 参考 solver（VL::Hair::GuideSimulation）和模块测试共同使用，
// GPU 侧 shader/glsl/hair/tressfxSim*.comp 必须保持同一公式与同一分支顺序。
// 约定与 TressFX 源实现一致，只有以下两处刻意不同：
// - 单位：TressFX 在米制下工作（rest 位置乘 0.01、速度 clamp 0.3 m）；VL 全链路
//   使用厘米，因此这里不出现 0.01，速度 clamp 为 30 cm。
// - 坐标系：TressFX 的重力是 GravityMagnitude * (0,0,-1)（Z-up）；VL 直接使用
//   作者资产的 gravity 向量（Y-up）。
namespace VL::Hair
{
// TressFX 的 ClampPositionDelta = 0.3 m；VL 使用厘米，对应 30 cm。
inline constexpr float TressfxVelocityClampCm = 30.0f;

// TressFX 的 DampingCoeff 以 60 Hz 为参考写成 exp(-DampingCoeff * TimeStep * 60)。
inline constexpr float TressfxDampingReferenceHz = 60.0f;

// TressFX 的 VSP 系数小于 0.9 时才乘以逐顶点 stiffness。
inline constexpr float TressfxVspStiffnessBlendThreshold = 0.9f;

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
inline float TressfxDampingDecay(float dampingCoeff, float timeStep)
{
    return std::exp(-dampingCoeff * timeStep * TressfxDampingReferenceHz);
}

// 作者字段 damping 是 60 Hz 下每步保留的速度比例（0..1]，历史语义。
// 映射到 TressFX 的 DampingCoeff：exp(-DampingCoeff) = damping。
// damping=1 表示不衰减；下界取 1e-6 避免 log(0)。
inline float TressfxDampingCoeffFromRetention(float damping)
{
    return -std::log(std::clamp(damping, 1.0e-6f, 1.0f));
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

// TressFX LengthConstraintsWind 的四风混合。a 只取决于全局 strand 下标：
//   w = a*Wind0 + (1-a)*Wind1 + a*Wind2 + (1-a)*Wind3,  a = (strand % 20) / 20
// VL 资产只有一个平均风向，Host 把四路都填成同一个向量，因此混合结果等于平均值；
// 公式保留是为了与源实现逐项对应，也为将来四风作者化留出位置。
inline Eigen::Vector3f TressfxCombineWind(const Eigen::Vector3f& wind0,
    const Eigen::Vector3f& wind1, const Eigen::Vector3f& wind2,
    const Eigen::Vector3f& wind3, uint32_t guideIndex)
{
    const float a = static_cast<float>(guideIndex % 20u) / 20.0f;
    return a * wind0 + (1.0f - a) * wind1 + a * wind2 + (1.0f - a) * wind3;
}

// TressFX 的风力：Force = -cross(cross(v, w), v)，v 是段方向（pos_i - pos_{i+1}）。
// 结果垂直于 v：只把发丝段往风向上推，不沿段方向拉伸。
//
// 单位适配（合同里记为 deviation）：源实现在米制下把**未归一化**的段向量代入
// 三叉积，结果带 |v|² 的长度平方量纲；VL 用厘米后 |v| 大 100 倍，同一段会得到
// 10^6 倍的风力，直接把发丝吹散。这里先把 v 归一化再代入同一公式，得到
// -w 在段法平面上的分量（cm/s²），保持源实现的推动方向与作者风速的物理量级。
inline Eigen::Vector3f TressfxWindForce(
    const Eigen::Vector3f& segment, const Eigen::Vector3f& wind)
{
    if (segment.squaredNorm() <= 1.0e-12f)
        return Eigen::Vector3f::Zero();
    const Eigen::Vector3f direction = segment.normalized();
    return -direction.cross(wind).cross(direction);
}

// VL 既有的有界阵风，保持作者字段 windGust/windFrequency 的历史效果。
// GPU 在 LengthConstraintsWind pass 把它乘到混合后的风向量上；TressFX 源实现没有
// 这一项，这是 VL 刻意保留的作者化扩展（见合同）。
inline float TressfxWindGustFactor(
    float windGust, float windFrequency, uint32_t guideIndex, float simulationTime)
{
    const float phase = static_cast<float>(guideIndex) * 0.6180339f;
    return 1.0f + windGust *
        std::sin(6.283185307179586f * windFrequency * simulationTime + phase);
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

// TressFX 的 VSPStiffness 是作者曲线（DECLARE_SCALAR_ARRAY 最多 64 项）。VL 没有
// 逐顶点曲线作者面，用从根前缀（1）到梢部（0）的线性衰减近似；只有 VspCoeff < 0.9
// 时参与混合。这一段在合同里明确记为近似。
inline float TressfxVspStiffness(uint32_t localPoint, uint32_t pointCount,
    uint32_t rootFixedPoints)
{
    const float span = static_cast<float>(pointCount) - 1.0f -
        static_cast<float>(rootFixedPoints);
    if (span <= 0.0f)
        return 0.0f;
    const float t = std::clamp(
        (static_cast<float>(localPoint) - static_cast<float>(rootFixedPoints)) / span,
        0.0f, 1.0f);
    return 1.0f - t;
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
