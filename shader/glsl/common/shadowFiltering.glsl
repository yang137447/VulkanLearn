#ifndef VL_COMMON_SHADOW_FILTERING_GLSL
#define VL_COMMON_SHADOW_FILTERING_GLSL

// 单次硬件阴影采样入口。
// sampler2DArrayShadow 会在采样时完成深度比较；当前 sampler 使用线性过滤，
// 因此一次 texture() 会对邻近 texel 的比较结果做硬件插值，而不是读取原始深度。
float SampleShadowMapHardwarePcf(
    in sampler2DArrayShadow shadowMap,
    vec2 shadowUv,
    int cascadeIndex,
    float compareDepth)
{
    return texture(
        shadowMap,
        vec4(shadowUv, float(cascadeIndex), compareDepth));
}

// 在四个偏移位置分别执行一次硬件 comparison sampling，再平均可见性。
// 这里的“4 tap PCF”是在硬件过滤之上扩大采样范围，不是 shader 手动读取
// 原始深度并比较。偏移使用 texel 单位，使过滤范围不随阴影图分辨率变化。
float FilterShadowMapPcf4(
    in sampler2DArrayShadow shadowMap,
    vec2 shadowUv,
    int cascadeIndex,
    float compareDepth)
{
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    const vec2 pcfOffsets[4] = vec2[](
        vec2(-0.75, -0.75),
        vec2( 0.75, -0.75),
        vec2(-0.75,  0.75),
        vec2( 0.75,  0.75));

    float visibility = 0.0;
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        visibility += SampleShadowMapHardwarePcf(
            shadowMap,
            shadowUv + pcfOffsets[sampleIndex] * texelSize,
            cascadeIndex,
            compareDepth);
    }
    return visibility * 0.25;
}

// 阴影过滤策略的唯一入口。后续接 Low / Medium / High 质量档时，只在这里
// 选择单次硬件 PCF、4 tap PCF 或更高质量采样核，不要复制 CSM 投影和 bias。
float FilterShadowMap(
    in sampler2DArrayShadow shadowMap,
    vec2 shadowUv,
    int cascadeIndex,
    float compareDepth)
{
    return FilterShadowMapPcf4(
        shadowMap,
        shadowUv,
        cascadeIndex,
        compareDepth);
}

#endif
