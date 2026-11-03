#ifndef VL_ENGINE_DECAL_COMPOSITE_GLSL
#define VL_ENGINE_DECAL_COMPOSITE_GLSL

#include "materialSurface.glsl"

vec3 DecodeDecalWorldNormal(vec3 encodedNormal)
{
    return normalize(encodedNormal * 2.0 - 1.0);
}

// DBuffer A: baseColor.rgb + color coverage
// DBuffer B: encoded world normal + normal coverage
// DBuffer C: roughness/metallic/ambientOcclusion + surface coverage
// DBuffer 的 RGB 已由 AlphaBlend（SrcAlpha / OneMinusSrcAlpha）预乘覆盖率，
// 所以这里必须用 over 形式累加，不能再乘一次 alpha。
void ApplyDecalDBuffer(
    inout MaterialSurface material,
    vec4 colorBuffer,
    vec4 normalBuffer,
    vec4 surfaceBuffer)
{
    float colorCoverage = clamp(colorBuffer.a, 0.0, 1.0);
    if (colorCoverage > 0.0)
    {
        material.baseColor =
            colorBuffer.rgb +
            material.baseColor * (1.0 - colorCoverage);
    }

    float normalCoverage = clamp(normalBuffer.a, 0.0, 1.0);
    if (normalCoverage > 0.0)
    {
        // 先除掉 AlphaBlend 预乘的覆盖率，再做 normal 合成；单层 decal
        // 可精确恢复。多层 normal decal 仍需 RNM/whiteout 才能真正对齐 UE。
        vec3 encodedNormal =
            normalBuffer.rgb / max(normalCoverage, 1.0e-5);
        material.worldNormal = normalize(mix(
            material.worldNormal,
            DecodeDecalWorldNormal(encodedNormal),
            normalCoverage));
    }

    float surfaceCoverage = clamp(surfaceBuffer.a, 0.0, 1.0);
    if (surfaceCoverage > 0.0)
    {
        material.roughness =
            surfaceBuffer.r +
            material.roughness * (1.0 - surfaceCoverage);
        material.metallic =
            surfaceBuffer.g +
            material.metallic * (1.0 - surfaceCoverage);
        material.ambientOcclusion =
            surfaceBuffer.b +
            material.ambientOcclusion * (1.0 - surfaceCoverage);
    }
}

#endif
