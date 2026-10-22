#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <Eigen/Dense>

namespace VL::Hair
{
// 作者解析胶囊上限。GPU shader 里的同名常量必须一起修改；C++ 侧用下面的
// static_assert 固定 GPU 记录布局。8 是当前作者规模的上限（头、颈、肩背），
// 超过后应改用 SDF 而不是继续堆胶囊。
inline constexpr uint32_t TressfxMaxColliders = 8;
// GPU shader 的对应常量必须同步；数量变化会改变 params/descriptor 合同。
static_assert(TressfxMaxColliders == 8,
    "Guide collider limit is mirrored by shader/glsl/hair/tressfxSimCommon.glsl");

// 资产局部空间的解析胶囊；a == b 时退化为球。radius 是厘米。
struct TressfxCollider
{
    Eigen::Vector3f a = Eigen::Vector3f::Zero();
    Eigen::Vector3f b = Eigen::Vector3f::Zero();
    float radius = 0.0f;
};

// GPU std430 镜像：a.xyz = 起点、a.w = radius；b.xyz = 终点、b.w 保留。
// shader/glsl/hair/tressfxSimCollision.comp 的 GuideCollider 与此一一对应。
struct GuideColliderGpu
{
    Eigen::Vector4f a = Eigen::Vector4f::Zero();
    Eigen::Vector4f b = Eigen::Vector4f::Zero();
};
static_assert(sizeof(GuideColliderGpu) == 32,
    "GPU guide collider must stay two vec4 (32 bytes)");
static_assert(alignof(GuideColliderGpu) == 16,
    "GPU guide collider requires vec4 alignment");

inline GuideColliderGpu MakeGuideColliderGpu(const TressfxCollider& collider)
{
    GuideColliderGpu gpu;
    gpu.a = Eigen::Vector4f(collider.a.x(), collider.a.y(), collider.a.z(),
        collider.radius);
    gpu.b = Eigen::Vector4f(collider.b.x(), collider.b.y(), collider.b.z(), 0.0f);
    return gpu;
}

// 胶囊最近点：a==b（denom==0）时退化为球心 a；否则把投影参数 clamp 到 [0,1]，
// 因此线段两端都形成半球帽。CPU 与 GPU 使用同一公式。
inline Eigen::Vector3f ClosestPointOnCapsule(
    const TressfxCollider& collider,
    const Eigen::Vector3f& point)
{
    const Eigen::Vector3f ab = collider.b - collider.a;
    const float denom = ab.squaredNorm();
    float t = 0.0f;
    if (denom > 0.0f)
        t = std::clamp((point - collider.a).dot(ab) / denom, 0.0f, 1.0f);
    return collider.a + ab * t;
}

// 点恰好落在胶囊轴线上时，普通 normalize(delta) 未定义。这里用与轴垂直的确定性
// 方向：axis 非 Y 向时 cross(axis, +Y)，否则 cross(axis, +X)；a==b 的球直接 +Y。
// GPU kernel 使用同一规则。
inline Eigen::Vector3f DegenerateCapsuleNormal(const TressfxCollider& collider)
{
    const Eigen::Vector3f axis = collider.b - collider.a;
    const float axisLength = axis.norm();
    if (axisLength <= 1.0e-6f)
        return Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    const Eigen::Vector3f unitAxis = axis / axisLength;
    const Eigen::Vector3f reference = std::abs(unitAxis.y()) < 0.9f
        ? Eigen::Vector3f(0.0f, 1.0f, 0.0f)
        : Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    return unitAxis.cross(reference).normalized();
}

// 只做位置投影，不修改 previousPositions。返回是否发生投影。
inline bool ResolveCapsuleCollision(
    Eigen::Vector3f& point,
    const TressfxCollider& collider)
{
    const Eigen::Vector3f closest = ClosestPointOnCapsule(collider, point);
    const Eigen::Vector3f delta = point - closest;
    const float length = delta.norm();
    if (length >= collider.radius)
        return false;
    const Eigen::Vector3f normal = length > 1.0e-6f
        ? delta / length
        : DegenerateCapsuleNormal(collider);
    point = closest + normal * collider.radius;
    return true;
}

} // namespace VL::Hair
