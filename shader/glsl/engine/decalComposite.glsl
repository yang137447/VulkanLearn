#ifndef VL_ENGINE_DECAL_COMPOSITE_GLSL
#define VL_ENGINE_DECAL_COMPOSITE_GLSL

#include "materialSurface.glsl"

vec3 DecodeDecalWorldNormal(vec3 encodedNormal)
{
    return normalize(encodedNormal * 2.0 - 1.0);
}

// DBuffer A: baseColor.rgb + color coverage
// DBuffer B: encoded world normal + normal coverage
// DBuffer C: roughness/ambientOcclusion + roughness coverage
// DBuffer D: metallic + metallic coverage
// DBufferMask: r = color, g = normal, b = roughness, a = metallic
// DBuffer 的 RGB 已由 AlphaBlend（SrcAlpha / OneMinusSrcAlpha）预乘覆盖率，
// 所以这里必须用 over 形式累加，不能再乘一次 alpha。
void ApplyDecalDBuffer(
    inout MaterialSurface material,
    vec4 colorBuffer,
    vec4 normalBuffer,
    vec4 surfaceBuffer,
    vec4 metallicBuffer,
    vec4 responseMask)
{
    // 掩码用 One/One 累积，重叠 decal 可能让同一通道大于 1；
    // saturate 后只保留“该通道是否允许覆盖”的 0/1 语义。
    vec4 response = clamp(responseMask, 0.0, 1.0);

    float colorCoverage =
        clamp(colorBuffer.a, 0.0, 1.0) * response.r;
    if (colorCoverage > 0.0)
    {
        material.baseColor =
            colorBuffer.rgb +
            material.baseColor * (1.0 - colorCoverage);
    }

    float normalAlpha = clamp(normalBuffer.a, 0.0, 1.0);
    float normalCoverage = normalAlpha * response.g;
    if (normalCoverage > 0.0)
    {
        // 先除掉 AlphaBlend 预乘的覆盖率，再做 normal 合成；单层 decal
        // 可精确恢复。多层 normal decal 仍需 RNM/whiteout 才能真正对齐 UE。
        vec3 encodedNormal =
            normalBuffer.rgb / max(normalAlpha, 1.0e-5);
        vec3 blendedNormal = mix(
            material.worldNormal,
            DecodeDecalWorldNormal(encodedNormal),
            normalCoverage);
        // 相反法线在 coverage≈0.5 时可能互相抵消；退化时保留 decal
        // 法线，避免 normalize(0) 产生 NaN 污染后续光照。
        float blendedLengthSq = dot(blendedNormal, blendedNormal);
        material.worldNormal = blendedLengthSq > 1.0e-8
            ? blendedNormal * inversesqrt(blendedLengthSq)
            : DecodeDecalWorldNormal(encodedNormal);
    }

    float surfaceCoverage = clamp(surfaceBuffer.a, 0.0, 1.0);
    // roughness 和 metallic 分开控位；DecalResponse 没有独立 AO 位，
    // 因此 AO 沿用 roughness 位。
    float roughnessCoverage = surfaceCoverage * response.b;
    if (roughnessCoverage > 0.0)
    {
        material.roughness =
            surfaceBuffer.r +
            material.roughness * (1.0 - roughnessCoverage);
        material.ambientOcclusion =
            surfaceBuffer.g +
            material.ambientOcclusion * (1.0 - roughnessCoverage);
    }

    float metallicCoverage =
        clamp(metallicBuffer.a, 0.0, 1.0) * response.a;
    if (metallicCoverage > 0.0)
    {
        material.metallic =
            metallicBuffer.r +
            material.metallic * (1.0 - metallicCoverage);
    }
}

#endif
