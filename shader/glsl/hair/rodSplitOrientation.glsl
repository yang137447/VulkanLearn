#ifndef VL_ROD_SPLIT_ORIENTATION_GLSL
#define VL_ROD_SPLIT_ORIENTATION_GLSL

// ===========================================================================
// V4 spike, Stage A1：把朝向从位置求解里拆出来的闭式更新（只验证，不改解）
//
// 论文：Hsu, Wang, Wu, Yuksel, "Stable Cosserat Rods", SIGGRAPH 2025,
// DOI 10.1145/3721238.3730618，Eq. 15/16/17/18/22。
// 逐项对应离线原型 artifacts/tmp/v4orient/proto.py::Rod._v_seg / _b_seg / _sc_quat
// （该原型的报告 report.md 见 §§2.1–2.3、§4）。
//
//   v = -(2 k_ss,i / l_i) * edge            （纯四元数，world 系）
//   b = sum_{j in angles(i)} k_bt,j * phi_j * (q_other * q_rest_rel)
//   q <- normalize(v * b * e + lambda * b),  lambda = |v| + |b|
//
// 三个必须遵守的细节：
//  1. phi 用**四分量**点积 dot4(qq, q_rest_rel)。作者实现里 Rotor::v 是整个
//     vec4；只用 xyz 会让静止姿态 phi 翻成 -1，b 反号，rest 不再是不动点。
//  2. e 是材料中轴。本仓库 restStrains[i].xyz 由平行移动帧定义，恒为 (0,0,1)
//     （rodKernel.glsl:275-278），对应论文的 e3 / 作者代码的 Rotor(1)。用
//     restStrains 里的实际值而不是硬编码，静止时 e*e = -1 恒成立。
//  3. 范数：只归一化分子；normalize 只去掉正标量，因此解析式 Eq.17 的单位范数
//     误差本来就是 float32 舍入量级，不需要额外的重规范化反馈。
//
// 刚度映射（与论文 Eq.5 的单位一致，不是拍脑袋的比值）：
//   k_ss,i = 1 / shearCompliance_i   （engine 的 elastic energy = 1/2 |C|^2/(c l)，
//                                      论文 k_ss,i = k_ss l，两者相等 ⇒ k_ss,i = 1/c）
//   k_bt,j = 4 / bendCompliance_j     （|c_bt| ~ theta/2，论文 k_bt = 4 k_bt_param/l）
// 本仓库这两个 compliance 是各向异性的：拉伸刚度逐轴取 1/c，弯曲刚度取三轴
// 倒数均值作为论文标量 k_bt 的各向同性等价量（已知近似，见 A1 报告）。
//
// 帧约定：engine 的 storage orientation 是 reference-relative（q_phys = ref * q_local，
// ref = reset 时的 q_root * restOrientation），论文的 q 是 world 系。闭式更新对
// 左乘参考帧的变换是等变的（v 被旋转、b 被左乘），因此这里直接在 world 系算，
// 最后 left-multiply conj(ref) 回到 local 存回。静止姿态两者给出同一个不动点。

#ifndef VL_ROD_SPLIT_SWEEPS
// 离线原型在 32 顶点杆上 4 轮 Gauss-Seidel 后最大修正 2.3e-5 rad；GPU 侧是
// Jacobi 双缓冲（A2 的目标形态），所以默认多跑几轮以便同时量到收敛曲线。
#define VL_ROD_SPLIT_SWEEPS 8
#endif

// A1 的测量结果走 RodGuide 里当前完全空着的 energies / residuals（active 路径
// 既没有 VL_ROD_V3_CONTINUATION 也没有 VL_ROD_KKT10_DEBUG，见 rodV2Kernel.glsl
// 的发布段）。快照侧车 .rod-diagnostics.bin 每 guide 16 float 原样带出。
shared vec4 v2SplitOrientations[31];
shared float v2SplitSweepMax[32];
shared float v2SplitMean;
shared float v2SplitPreNorm;
shared float v2SplitRaw;

vec4 RodSplitFrame(uint segment)
{
    return displacementReferences.guides[gl_WorkGroupID.x].frames[segment];
}

// source == 0：读耦合解（v2Orientations，行搜索后的 high 部分 + 补偿 low 部分）。
// source == 1：读本 pass 自己的 Jacobi 状态。
vec4 RodSplitLocal(uint segment, uint source)
{
    if (source != 0u)
        return v2SplitOrientations[segment];
    vec4 local = v2Orientations[segment];
#ifdef VL_ROD_V3_COMPENSATED_STATE
    local += v3QuaternionLow[segment];
#endif
    return normalize(local);
}

vec4 RodSplitWorld(uint segment, uint source)
{
    return RodMul(RodSplitFrame(segment), RodSplitLocal(segment, source));
}

float RodSplitAngle(vec4 first, vec4 second)
{
    vec4 relative = RodMul(RodConjugate(normalize(first)), normalize(second));
    return length(RodLog(relative));
}

// 单段闭式更新。返回 reference-relative 的 q_local（与 engine 存储约定一致）。
// preNormError = | |num|/(lambda^2-|v|^2) - 1 |，论文 Eq.17 的解析单位范数误差。
// rawError = 归一化后实际存下的 |q|-1（float32 舍入量级）。
vec4 RodSplitClosedForm(uint segment, uint source, out float preNormError, out float rawError)
{
    preNormError = 0.0;
    rawError = 0.0;
    vec4 qWorld = RodSplitWorld(segment, source);
    vec3 edge = v2Positions[segment + 1u].xyz - v2Positions[segment].xyz;
    // 材料系应变：restStrain + C，等价于 R_phys^T * edge / l（见 RodV2StableShear）。
    vec3 materialStrain = restStrains[segment].xyz +
        RodV2StableShear(segment, RodSplitLocal(segment, source), edge);
    vec3 stretchStiffness = 1.0 /
        rodState.guides[gl_WorkGroupID.x].segments[segment].shearCompliance.xyz;
    vec3 vWorld = -2.0 * (RodRotation(qWorld) * (stretchStiffness * materialStrain));

    // b：接在该段两侧关节上的弯曲项之和（论文 "b becomes the sum of all
    // bending constraint contributions"；单链上就是关节 i-1 与 i）。
    vec4 bend = vec4(0);
    for (int offset = -1; offset <= 0; ++offset)
    {
        int jointSigned = int(segment) + offset;
        if (jointSigned < 0 || jointSigned >= 30)
            continue;
        uint joint = uint(jointSigned);
        vec4 left = RodSplitWorld(joint, source);
        vec4 right = RodSplitWorld(joint + 1u, source);
        // qq = q_rest 系里的相对旋转 q_a^-1 q_{a+1}，与 phi 的参考量同帧。
        vec4 relative = RodMul(RodConjugate(left), right);
        // restJoints[j] 就是 q_rest_j^-1 q_rest_{j+1} 的对数（rodKernel.glsl:316-318）。
        vec4 restRelative = RodExp(restJoints[joint].xyz);
        float sign = dot(relative, restRelative) > 0.0 ? 1.0 : -1.0;
        vec4 other = joint == segment ? right : left;
        vec4 restTerm = joint == segment ? RodConjugate(restRelative) : restRelative;
        vec3 compliance = rodState.guides[gl_WorkGroupID.x].segments[joint].bendCompliance.xyz;
        float stiffness = 4.0 * dot(vec3(1.0 / 3.0), 1.0 / compliance);
        bend += (stiffness * sign) * RodMul(other, restTerm);
    }

    vec4 axis = vec4(restStrains[segment].xyz, 0.0);
    vec4 vQuaternion = vec4(vWorld, 0.0);
    float vLength = length(vQuaternion);
    float bLength = length(bend);
    float lambda = vLength + bLength;
    vec4 numerator = RodMul(RodMul(vQuaternion, bend), axis) + lambda * bend;
    float numeratorLength = length(numerator);
    float denominator = lambda * lambda - vLength * vLength;
    rawError = abs(numeratorLength - 1.0);
    if (denominator > 0.0)
        preNormError = abs(numeratorLength / denominator - 1.0);
    if (!(numeratorLength > 0.0) || !RodFinite(numerator.xyz) ||
        isnan(numerator.w) || isinf(numerator.w))
        return RodSplitLocal(segment, source);
    vec4 world = numerator / numeratorLength;
    vec4 result = RodMul(RodConjugate(RodSplitFrame(segment)), world);
    // 归一化后真正落盘的 |q|-1（float32 舍入量级；论文的解析误差见 preNormError）。
    rawError = abs(length(result) - 1.0);
    return result;
}

#endif
