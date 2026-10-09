#ifndef VL_COMMON_CLOTH_BRDF_GLSL
#define VL_COMMON_CLOTH_BRDF_GLSL

#include "defines.glsl"

// Cloth / Sheen 的论文公式层。
//
// 2026-09-16 路线定案为方案 C（两个独立 ShadingModelID）之后，本文件只保留**有出处**的公式：
//   · Estévez & Kulla 2017 式(2)：Charlie 分布（指数化正弦）
//   · Neubelt & Pettineo 2013 式(23) 的分母：布料高光里替换传统 microfacet 分母的那一项
//
// 旧 v2 各向异性路径里**论文没有的五项**已按计划 §0.3.1 的阶梯清算删除，不要复活：
//   `aspect = 2^anisotropy`、椭圆 Charlie、warp visibility 的掠射衰减、cross 双轴混合。
// 各向异性改由 B 臂 `ClothFiber`（Sadeghi et al. 2013 microcylinder，thread tangent）承担，
// 那条路线的每一项都能落到论文式号上。
//
// 求值器合同（2026-09-22，知识库 A 臂 / 计划 §2.3⑪）：Cloth 用 ClothSheenG（式 3 + 式 4），
// r 不平方。Neubelt 分母和 ClothSheenRoughnessToAlpha 只留给曲线探针 M_brdfPlot，不要接进求值器。

const float CLOTH_MIN_SHEEN_ROUGHNESS = 0.02;
const float CLOTH_MAX_SHEEN_ROUGHNESS = 1.0;
const float CLOTH_DIRECTIONAL_ALBEDO_MIN_ALPHA = 0.0004;

// Mapping version 1：作者 roughness 用平方映射到 Charlie 的分布参数。
// 注意论文（Estévez & Kulla 2017 式(2)）的 r 直接就是 sin 的指数，**没有** r→α 映射；
// 这里的平方是 Disney（Karis 2013）约定被平移到 Charlie 上，是否沿用见计划 `S-26`。
float ClothSheenRoughnessToAlpha(float sheenRoughness)
{
    return sheenRoughness * sheenRoughness;
}

// Estévez & Kulla 2017 式(2)：D(m) = (2 + 1/r) · sin^(1/r)θ_h / 2π。
// 与论文逐项一致（审计 `S-22`），改动前先看那条记录。
float ClothCharlieDistribution(float alpha, float NdotH)
{
    if (NdotH <= 0.0)
    {
        return 0.0;
    }
    float sinSquaredThetaH = 1.0 - NdotH * NdotH;
    if (sinSquaredThetaH <= 0.0)
    {
        return 0.0;
    }
    float inverseAlpha = 1.0 / alpha;
    return (2.0 + inverseAlpha) *
        pow(sinSquaredThetaH, 0.5 * inverseAlpha) /
        (2.0 * PI);
}

// Neubelt & Pettineo 2013 式(23) 的分母：1 / (4(N·L + N·V − N·L·N·V))。
// 原文是"我们丢掉了 geometry 项"之后**替换传统 microfacet 分母**的那一项，不是独立的
// visibility 模型；函数名是否改成 `ClothNeubeltDenominator` 由审计 `S-24` 决定。
float ClothNeubeltVisibility(float NdotL, float NdotV)
{
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return 0.0;
    }
    return 1.0 /
        (4.0 * (NdotL + NdotV - NdotL * NdotV));
}

// v1 各向同性组合：Charlie 分布（式 2）× 式(23) 分母。
// 计划 `S-25` 要核对的就是"各向同性时它严格等于这两项之积、不含任何额外项"。
float ClothSheenUnitResponse(
    float alpha,
    float NdotL,
    float NdotV,
    float NdotH)
{
    if (NdotL <= 0.0 || NdotV <= 0.0 || NdotH <= 0.0)
    {
        return 0.0;
    }
    return ClothCharlieDistribution(alpha, NdotH) *
        ClothNeubeltVisibility(NdotL, NdotV);
}

// ---------------------------------------------------------------------------
// A 臂 `Cloth` = 8：Estévez & Kulla 2017 的几何项（式 3 与式 4）
// ---------------------------------------------------------------------------
// 论文按 Heitz 2014 的 projected-area normalization 数值求 G，再对 Λ 拟合（式 3）：
//   Λ(θ) = e^{L(cosθ)}                  ，cosθ < 0.5
//          e^{2L(0.5) − L(1−cosθ)}       ，否则
//   L(x) = a/(1 + b·x^c) + d·x + e      （Table 1 给出 r=0 与 r=1 两组系数）
// 中间粗糙度按论文的插值：P = (1−r)²·P_0.0 + (1 − (1−r)²)·P_1.0。
//
// 注意两件事：
//   1. 这里的 `r` 就是作者的感知粗糙度，**不经过 α = r²** —— 论文的 r 同时是式(2) 的指数
//      和式(3) 的插值参数（计划 `S-26` 的结论按此执行）；
//   2. 式(4) 的 terminator softening 只作用于**受光侧**，它破坏互易性，是论文唯一的
//      非物理修改（论文原话：the only point where we break the physical model）。
float ClothSheenLambdaFitL(float a, float b, float c, float d, float e, float x)
{
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

// 式(3)：单个方向的 Λ。cosTheta 是该方向与法线的点积；掠射端 cosTheta→0 时 Λ 会变大，
// 这正是 masking 的效果，调用方负责在 cosTheta ≤ 0 时把整个 lobe 归零。
float ClothSheenLambda(float sheenRoughness, float cosTheta)
{
    // 论文的插值权重：(1−r)² 压给 r = 0 那一组
    float oneMinusRSquared = (1.0 - sheenRoughness) * (1.0 - sheenRoughness);
    // Table 1：r = 1.0 与 r = 0.0 两组 (a, b, c, d, e)
    float a = mix(21.5473, 25.3245, oneMinusRSquared);
    float b = mix(3.82987, 3.32435, oneMinusRSquared);
    float c = mix(0.19823, 0.16801, oneMinusRSquared);
    float d = mix(-1.97760, -1.27393, oneMinusRSquared);
    float e = mix(-4.32054, -4.85967, oneMinusRSquared);

    if (cosTheta < 0.5)
    {
        return exp(ClothSheenLambdaFitL(a, b, c, d, e, cosTheta));
    }
    // cosθ ≥ 0.5 的分支在 cosθ = 0.5 处与上一支连续（2L(0.5) − L(0.5) = L(0.5)）
    return exp(2.0 * ClothSheenLambdaFitL(a, b, c, d, e, 0.5) -
        ClothSheenLambdaFitL(a, b, c, d, e, 1.0 - cosTheta));
}

// 式(4)：受光侧的 Λ′ = Λ^(1 + 2(1−cosθi)^8)。正入射时指数为 1（退化为式 3），
// 越靠近 terminator 指数越大 → 过渡越软。这一步使 BRDF 非互易。
float ClothSheenTerminatorSoftenedLambda(float sheenRoughness, float cosTheta)
{
    float lambda = ClothSheenLambda(sheenRoughness, cosTheta);
    float softenExponent = 1.0 + 2.0 * pow(1.0 - cosTheta, 8.0);
    return pow(lambda, softenExponent);
}

// 式(1) 里的 G：论文用 correlated 形式 1/(1 + Λ(ωo) + Λ(ωi))，
// 其中出射侧用式(3)、受光侧用式(4) 软化后的 Λ′。
float ClothSheenG(float sheenRoughness, float NdotL, float NdotV)
{
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return 0.0;
    }
    float lambdaV = ClothSheenLambda(sheenRoughness, NdotV);
    float lambdaL = ClothSheenTerminatorSoftenedLambda(sheenRoughness, NdotL);
    return 1.0 / (1.0 + lambdaV + lambdaL);
}

#endif
