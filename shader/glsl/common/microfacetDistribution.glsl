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
    float a      = roughness*roughness;
    float a2     = a*a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float num   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

// GeometrySchlickGGX 从 common/lighting.glsl 原样搬到这里：它是纯数学，不依赖任何
// UBO，而 Material Surface 阶段无法 include lighting.glsl。lighting.glsl 继续
// include 本文件，行为不变。
float GeometrySchlickGGX(float cosTheta, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float num   = cosTheta;
    float denom = cosTheta * (1.0 - k) + k;
	
    return num / denom;
}

// 单方向精确 Smith GGX 遮蔽项 G1，用作 Karis 2013 Fig 2 的参考曲线。
//
// 注意：引擎实际着色路径用的是上面的 GeometrySchlickGGX（k=(r+1)^2/8 的 IBL 变体），
// 论文 Fig 2 画的是 k=alpha/2 的 direct 变体，两者不是同一条曲线。
// 提供精确解是为了让曲线探针量化**本引擎所选近似**的误差，它不参与任何着色路径。
// 形式取自 Smith height-field 闭合：G1 = 2c / (c + sqrt(a^2 + (1-a^2)c^2))，
// 其中 a = roughness^2，与 DistributionGGX 的粗糙度约定一致。
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
