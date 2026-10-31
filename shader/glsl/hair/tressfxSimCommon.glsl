#ifndef VL_TRESSFX_SIM_COMMON_GLSL
#define VL_TRESSFX_SIM_COMMON_GLSL

// TressFX 5.0 guide solver 多 pass 管线共享声明。
//
// - std140 params block 与 source/hair/tressfxSimulationParams.h 一一对应；
//   任何字段顺序/大小变化都必须同时改 C++ 的 static_assert。
// - 位置统一为厘米（TressFX 源实现是米，rest 乘 0.01；这里不缩放）。
// - 重力/风由资产按世界 Y-up 给出，Host 换算到当前 hair 局部后上传。
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
    vec4 controls;     // x = dt, y = base DampingCoeff（分组模拟读 binding 17）,
                       // z = pointsPerGuide, w = rootFixedPoints
    vec4 shape;        // base globalShapeStiffness/effectiveRange/vspCoefficient/
                       // vspAccelThresholdMax 的镜像；分组模拟读 binding 17
    vec4 motion;       // x = base Stiffness(LocalShape), y = ResetPositions, z = rebaseOnly,
                       // w 的 bit0/bit1 = VSP / LocalShape 作者数组存在，
                       // bit2 = 逐 strand 多骨权重存在
                       // 时间调制在 Host 烘进 wind0..3
    uvec4 counts;      // x = length iterations, y = guideCount,
                       // z = pointCount, w = colliderCount
    vec4 skinningQuat; // (x,y,z,w) = 根骨旋转；w 为标量部分
    mat4 skinning;     // 根点骨骼矩阵
    mat4 historyToCurrentLocal; // inverse(currentHairModel) * lastRecordedHairModel
    // 作者逐顶点 stiffness 曲线（最多 64 项），4 个 float 一组打包为 vec4；
    // 第 local 项在 [local / 4][local % 4]。motion.w 的标志位决定是否使用。
    vec4 vspStiffness[16];
    vec4 localShapeStiffness[16];
    // x = 每单位法向相对速度的阻力率（1/cm）；其余保留。
    vec4 windParams;
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

// 从刚体矩阵上 3x3 提取四元数；与源 MakeQuaternion 同约定（q = (axis*sin,
// cos)）。多骨 StrandLevel 用它把加权后的骨骼矩阵写成 SkinningQuat。
vec4 TressfxQuatFromMatrix(mat4 matrix)
{
    mat3 r = mat3(matrix);
    float trace = r[0][0] + r[1][1] + r[2][2];
    vec4 q;
    if (trace > 0.0)
    {
        float s = sqrt(trace + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (r[1][2] - r[2][1]) / s;
        q.y = (r[2][0] - r[0][2]) / s;
        q.z = (r[0][1] - r[1][0]) / s;
    }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
    {
        float s = sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
        q.w = (r[1][2] - r[2][1]) / s;
        q.x = 0.25 * s;
        q.y = (r[1][0] + r[0][1]) / s;
        q.z = (r[2][0] + r[0][2]) / s;
    }
    else if (r[1][1] > r[2][2])
    {
        float s = sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
        q.w = (r[2][0] - r[0][2]) / s;
        q.x = (r[1][0] + r[0][1]) / s;
        q.y = 0.25 * s;
        q.z = (r[2][1] + r[1][2]) / s;
    }
    else
    {
        float s = sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
        q.w = (r[0][1] - r[1][0]) / s;
        q.x = (r[2][0] + r[0][2]) / s;
        q.y = (r[2][1] + r[1][2]) / s;
        q.z = 0.25 * s;
    }
    return normalize(q);
}

// TressFX LengthConstraintsWind 的四风混合。四路是 Host 按 SetWind 生成的
// 锥形空气速度；权重和为 2，纵向分量为 2*cos(40°)，这里乘回 1/(2cos40°)
// 让混合后的幅值等于场景风速，横向分量仍作为逐 strand 扰动。
const float TressfxWindConeMixScale = 0.6527036446661393;
vec3 TressfxCombinedWind(uint guide)
{
    float a = float(guide % 20u) / 20.0;
    vec3 mixed = a * params.wind0.xyz + (1.0 - a) * params.wind1.xyz
        + a * params.wind2.xyz + (1.0 - a) * params.wind3.xyz;
    return mixed * TressfxWindConeMixScale;
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

// 作者逐顶点曲线数组是否存在（Host 写入 motion.w 的 bit0/bit1）。
bool TressfxHasVspStiffness()
{
    return (uint(params.motion.w) & 1u) != 0u;
}

bool TressfxHasLocalShapeStiffness()
{
    return (uint(params.motion.w) & 2u) != 0u;
}

// TressFX 的 VSPStiffness 作者曲线上限 64 项，第 i 项 = 曲线在 i/N 处的值
// （TressFXComponent.cpp:687-691）。Host 有作者数组时按 4 个一组打包；
// 没有时沿用根前缀 -> 梢部的线性衰减近似。
float TressfxVspStiffness(uint local)
{
    if (TressfxHasVspStiffness())
        return params.vspStiffness[int(local / 4u)][int(local % 4u)];
    float span = float(TressfxPointsPerGuide()) - 1.0
        - float(TressfxRootFixedPoints());
    if (span <= 0.0)
        return 0.0;
    float t = clamp((float(local) - float(TressfxRootFixedPoints())) / span, 0.0, 1.0);
    return 1.0 - t;
}

// 源 VSP pass 只在 VspCoeff < 0.9 时乘逐顶点 stiffness（patch 10153-10160）；
// 否则系数原样保留。CPU 的 TressfxVspCoefficient 保持同一分支。
float TressfxVspCoefficient(float coefficient, uint local)
{
    if (coefficient >= 0.9)
        return coefficient;
    return coefficient * TressfxVspStiffness(local);
}

// LocalShape 的逐顶点 Stiffness[i]：作者数组存在时用数组值（加载期已按源
// min(value, 0.95) 处理），否则回退到 motion.x 的整条 strand 标量。
// 调用方仍按 TressFX 公式乘 0.5。
float TressfxLocalShapeStiffness(uint local, float fallback)
{
    if (TressfxHasLocalShapeStiffness())
        return params.localShapeStiffness[int(local / 4u)][int(local % 4u)];
    return fallback;
}

#endif // VL_TRESSFX_SIM_COMMON_GLSL
