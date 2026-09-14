#ifndef VL_COMMON_MICROFACET_DISTRIBUTION_GLSL
#define VL_COMMON_MICROFACET_DISTRIBUTION_GLSL

#include "defines.glsl"

// 纯数学的微表面分布函数。
//
// 这些函数不依赖任何 UBO / 光照上下文，因此 Material Surface 阶段也能直接调用。
// 它们原本定义在 common/lighting.glsl 里，但那支文件会引用 uboVP、csmParameters
// 等全局光照 uniform，Material Surface 阶段没有这些声明，无法 include。
// 把它拆到这里是为了让"论文曲线探针"之类的材质复用**同一份**分布实现，
// 而不是在材质里照抄一遍公式。lighting.glsl 继续 include 本文件，行为不变。

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float alpha  = roughness*roughness;
    float alphaSquared = alpha*alpha;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float num   = alphaSquared;
    float denom = (NdotH2 * (alphaSquared - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

// GeometrySchlickGGX 从 common/lighting.glsl 原样搬到这里：它是纯数学，不依赖任何
// UBO，而 Material Surface 阶段无法 include lighting.glsl。lighting.glsl 继续
// include 本文件，行为不变。
//
// 这一支是 Karis 2013 式(4) 的 **analytic light source 变体**：Disney 的粗糙度重映射
// （(Roughness+1)/2 再平方）→ `k = (r+1)²/8`。它**只**服务于直接光（dir / point / spot）
// 与同样按 analytic 处理的 UE Legacy 分支；IBL 域要用下面的 GeometrySchlickGGXIbl。
float GeometrySchlickGGX(float cosTheta, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float num   = cosTheta;
    float denom = cosTheta * (1.0 - k) + k;
	
    return num / denom;
}

// Schlick-GGX G1 的 **IBL 变体**：`k = α/2`（α = roughness²）。
//
// 这支是从 `generator/brfdLut.comp` 原样搬过来的：split-sum 预积分 LUT 用它、
// 曲线探针也要画它，而"数学定义在仓库里只有一份"是硬要求（见本文件开头）。
//
// 变量名按论文写：论文的 **α ≡ roughness²**，这里就叫 `alpha`，不再用 `a`。
// 命名规矩的来历（值得记住）：旧写法是 `float a = roughness; k = (a*a)*0.5`——
// 同一个 `a` 在这个文件里两处含义不同（`DistributionGGX` 里 `a` 曾是 α，这里却是感知
// 粗糙度），审计时被读成 `k = α²/2`，凭空找出一条"论文里没有的公式"（`D-21`）。
// 结论：**α 一律写 alpha**；名字与论文不一致时，先改名字再谈对齐。
//
// 为什么是 α/2 而不是 analytic 路径的 (r+1)²/8：Karis 2013 §Specular G 只把 Disney 的
// 粗糙度重映射（(Roughness+1)/2 再平方）用于 analytic light source，原话是
// "this adjustment is only used for analytic light sources; if applied to image-based
// lighting, the results at glancing angles will be much too dark."；IBL 域用的是未重映射的
// `k = α/2` 拟合（同节：*"we chose to use the Schlick model, but with k = α/2"*）。
// 换句话说：本文件里的两支 Schlick **分别对应论文允许的两条路径**，不是同一支的两个写法。
float GeometrySchlickGGXIbl(float cosTheta, float roughness)
{
    float alpha = roughness * roughness;
    float k = alpha * 0.5;

    return cosTheta / (cosTheta * (1.0 - k) + k);
}

// Beckmann–Spizzichino 法线分布（**参考曲线，不参与任何着色路径**）。
//
// 出处：PBRT 3ed §8.4.1 式(8.10) 的等向形式（αx = αy 时各向异性式退化为它）：
//   D(ωh) = exp( -tan²θh / α² ) / ( π α² cos⁴θh )
// 原始来源是 Beckmann & Spizzichino 1963（"基于微表面斜率高斯分布"）。
//
// 为什么引擎里要留一支"不参与着色"的分布：对齐计划 `D-06` 要在**同一 α** 下对照 GGX 与
// Beckmann 的形状，判据是 PBRT 4ed §9.6.1 + Fig 9.23 的那句结论——
// *"Note that Trowbridge–Reitz has higher tails at larger values of θ"*（α = 0.5）。
// 曲线探针的 mode 5 画的就是这两条；着色路径一行都不用它。
//
// 与 GGX 的约定保持一致：入参是 perceptual roughness，函数内做 α = roughness²；
// 峰值同为 D(n) = 1/(π α²)，所以"按各自峰值归一化"对两条曲线是同一件事。
// tan²θh → ∞ 时按 PBRT 的做法显式返回 0（那边是 0/0 → NaN，这里用 cos⁴ 兜住）。
float DistributionBeckmann(float cosThetaH, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float clampedCos = clamp(cosThetaH, 0.0, 1.0);
    if (clampedCos <= 0.0)
    {
        return 0.0;
    }
    float cosSquared = clampedCos * clampedCos;
    float tanSquared = (1.0 - cosSquared) / cosSquared;
    float cosFourth = cosSquared * cosSquared;
    return exp(-tanSquared / alphaSquared) / (PI * alphaSquared * cosFourth);
}

// 单方向精确 Smith GGX 遮蔽项 G1，用作 Karis 2013 Fig 2 的参考曲线。
//
// 注意：直接光路径用的是上面的 GeometrySchlickGGX（`k=(r+1)²/8`，analytic 变体），
// IBL 路径用的是 GeometrySchlickGGXIbl（`k=α/2`）；论文 Fig 2 画的是后者。
// 提供精确解是为了让曲线探针量化**两支 Schlick 各自**的误差，它不参与任何着色路径。
// 形式取自 Smith height-field 闭合：G1 = 2c / (c + sqrt(α² + (1-α²)c²))，
// 其中 α = roughness²（变量名同上：一律 `alpha`），与 DistributionGGX 的粗糙度约定一致。
float SmithG1Ggx(float cosTheta, float roughness)
{
    float alpha = roughness * roughness;
    float clampedCos = clamp(cosTheta, 0.0, 1.0);
    float alphaSquared = alpha * alpha;
    float root = sqrt(
        alphaSquared + (1.0 - alphaSquared) * clampedCos * clampedCos);
    return 2.0 * clampedCos / (clampedCos + root);
}
#endif
