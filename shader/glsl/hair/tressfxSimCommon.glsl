#ifndef VL_TRESSFX_SIM_COMMON_GLSL
#define VL_TRESSFX_SIM_COMMON_GLSL

// TressFX 5.0 guide solver 多 pass 管线共享声明。
//
// - std140 params block 与 source/hair/tressfxSimulationParams.h 一一对应；
//   任何字段顺序/大小变化都必须同时改 C++ 的 static_assert。
// - 位置统一为厘米（TressFX 源实现是米，rest 乘 0.01；这里不缩放）。
// - 重力方向由资产给出（Y-up），不再使用 TressFX 的 GravityMagnitude*(0,0,-1)。
// - 约定 q = vec4(x, y, z, w)，w 是标量部分。
// - 可移动标记沿用 TressFX：vec4.w != 0 为 movable；不可移动点在 integration
//   pass 写 w = 0。
layout(std140, set = 0, binding = 0) uniform SimulationParams
{
    vec4 gravity;      // xyz = 重力加速度，厘米/秒²
    vec4 wind0;        // TressFX Wind0
    vec4 wind1;        // TressFX Wind1
    vec4 wind2;        // TressFX Wind2
    vec4 wind3;        // TressFX Wind3
    vec4 controls;     // x = dt, y = DampingCoeff, z = pointsPerGuide, w = rootFixedPoints
    vec4 shape;        // x = globalShapeStiffness, y = effectiveRange,
                       // z = vspCoefficient, w = vspAccelThresholdMax
    vec4 motion;       // x = Stiffness(LocalShape); y/z/w 保留为 0
                       // （阵风已删除，时间调制在 Host 烘进 wind0..3）
    uvec4 counts;      // x = length iterations, y = guideCount,
                       // z = pointCount, w = colliderCount
    vec4 skinningQuat; // (x,y,z,w) = 根骨旋转；w 为标量部分
    mat4 skinning;     // 根点骨骼矩阵
} params;

// 与 source/hair/tressfxCollision.h 的 TressfxMaxColliders / GuideColliderGpu 一致。
const uint TressfxMaxColliders = 8u;
struct GuideCollider
{
    vec4 a; // xyz = 起点，w = radius
    vec4 b; // xyz = 终点，w 保留
};

uint TressfxPointsPerGuide()
{
    return uint(params.controls.z);
}

uint TressfxRootFixedPoints()
{
    return uint(params.controls.w);
}

// TressFX IsMovable(asuint(pos.w))。
bool TressfxIsMovable(float w)
{
    return w != 0.0;
}

// TressFX QuatFromTwoUnitVectors（TressFXSimulationCommon.ush）：
// 最短弧 q = normalize((cross(u,v), dot(u,v)+1))；反向时取确定性垂直轴。
vec4 TressfxQuatFromTwoUnitVectors(vec3 u, vec3 v)
{
    float r = dot(u, v) + 1.0;
    vec4 q;
    if (r < 1.0e-6)
    {
        r = 0.0;
        if (abs(u.x) > abs(u.z))
            q = vec4(-u.y, u.x, 0.0, r);
        else
            q = vec4(0.0, -u.z, u.y, r);
    }
    else
    {
        q = vec4(cross(u, v), r);
    }
    return normalize(q);
}

// TressFX MultQuaternionAndVector。
vec3 TressfxRotateByQuat(vec4 q, vec3 v)
{
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// TressFX LengthConstraintsWind 的四风混合。四路是 Host 按 SetWind 生成的
// 锥形方向（厘米/秒）；权重和为 2，但方向不同，混合结果逐 strand 变化。
vec3 TressfxCombinedWind(uint guide)
{
    float a = float(guide % 20u) / 20.0;
    return a * params.wind0.xyz + (1.0 - a) * params.wind1.xyz
        + a * params.wind2.xyz + (1.0 - a) * params.wind3.xyz;
}

// TressFX ConstraintMultiplier 的四个分支。
vec2 TressfxConstraintMultiplier(bool movable0, bool movable1)
{
    if (movable0)
        return movable1 ? vec2(0.5, 0.5) : vec2(1.0, 0.0);
    return movable1 ? vec2(0.0, 1.0) : vec2(0.0, 0.0);
}

// TressFX ApplyDistanceConstraint（stiffness = 1）。
void TressfxApplyDistanceConstraint(inout vec4 pos0, inout vec4 pos1,
    float targetDistance)
{
    vec3 delta = pos1.xyz - pos0.xyz;
    float distance = max(length(delta), 1.0e-7);
    delta *= 1.0 - targetDistance / distance;
    vec2 multiplier = TressfxConstraintMultiplier(
        TressfxIsMovable(pos0.w), TressfxIsMovable(pos1.w));
    pos0.xyz += multiplier.x * delta;
    pos1.xyz -= multiplier.y * delta;
}

// TressFX 的 VSPStiffness 作者曲线上限 64 项；VL 用根前缀 -> 梢部的线性衰减近似，
// 只在 VspCoeff < 0.9 时参与（与源实现同一条件）。
float TressfxVspStiffness(uint local)
{
    float span = float(TressfxPointsPerGuide()) - 1.0
        - float(TressfxRootFixedPoints());
    if (span <= 0.0)
        return 0.0;
    float t = clamp((float(local) - float(TressfxRootFixedPoints())) / span, 0.0, 1.0);
    return 1.0 - t;
}

#endif // VL_TRESSFX_SIM_COMMON_GLSL
