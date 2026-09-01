#ifndef VL_COMMON_CLOTH_BRDF_GLSL
#define VL_COMMON_CLOTH_BRDF_GLSL

#include "defines.glsl"

const float CLOTH_MIN_SHEEN_ROUGHNESS = 0.02;
const float CLOTH_MAX_SHEEN_ROUGHNESS = 1.0;
const float CLOTH_DIRECTIONAL_ALBEDO_MIN_ALPHA = 0.0004;
const float CLOTH_ANISOTROPY_MIN = -1.0;
const float CLOTH_ANISOTROPY_MAX = 1.0;
const float CLOTH_ANISOTROPY_CROSS_MIN = 0.0;
const float CLOTH_ANISOTROPY_CROSS_MAX = 1.0;

// Mapping version 1：作者 roughness 使用平方映射到 Charlie alpha。
float ClothSheenRoughnessToAlpha(float sheenRoughness)
{
    return sheenRoughness * sheenRoughness;
}

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

float ClothNeubeltVisibility(float NdotL, float NdotV)
{
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return 0.0;
    }
    return 1.0 /
        (4.0 * (NdotL + NdotV - NdotL * NdotV));
}

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

float ClothAnisotropyToAspect(float anisotropy)
{
    // Mapping version 1：aspect=2^anisotropy；正值使 fiber tangent 轴更宽，
    // 负值交换 T/B 的宽窄，因而正负号具有稳定的轴向含义。
    return exp2(anisotropy);
}

float ClothAnisotropicCharlieDistribution(
    float alpha,
    float anisotropy,
    vec3 localHalfDirection)
{
    float halfNormal = localHalfDirection.z;
    float halfTangent = localHalfDirection.x;
    float halfBitangent = localHalfDirection.y;
    if (halfNormal <= 0.0)
    {
        return 0.0;
    }

    float sinSquaredThetaH =
        halfTangent * halfTangent + halfBitangent * halfBitangent;
    if (sinSquaredThetaH <= 0.0)
    {
        return 0.0;
    }

    float aspect = ClothAnisotropyToAspect(anisotropy);
    float alphaTangent = alpha * aspect;
    float alphaBitangent = alpha / aspect;
    float ellipseDenominator =
        alphaBitangent * alphaBitangent * halfTangent * halfTangent +
        alphaTangent * alphaTangent * halfBitangent * halfBitangent;
    float isotropicDistribution = ClothCharlieDistribution(alpha, halfNormal);
    // 椭圆切平面因子保持 projected-area 归一；anisotropy=0 时严格退化
    // 为各向同性 Charlie，而不是把 NdotH 替换成单个 tangent dot。
    return isotropicDistribution *
        (alphaTangent * alphaBitangent * sinSquaredThetaH) /
        ellipseDenominator;
}

float ClothAnisotropicWarpedNdot(
    float anisotropy,
    vec3 localDirection)
{
    float aspect = ClothAnisotropyToAspect(anisotropy);
    float tangent = localDirection.x / aspect;
    float bitangent = localDirection.y * aspect;
    float normal = localDirection.z;
    return normal / sqrt(
        normal * normal + tangent * tangent + bitangent * bitangent);
}

float ClothAnisotropicVisibility(
    float alpha,
    float anisotropy,
    vec3 localLightDirection,
    vec3 localViewDirection)
{
    float warpedNdotL = ClothAnisotropicWarpedNdot(
        anisotropy,
        localLightDirection);
    float warpedNdotV = ClothAnisotropicWarpedNdot(
        anisotropy,
        localViewDirection);
    if (warpedNdotL <= 0.0 || warpedNdotV <= 0.0)
    {
        return 0.0;
    }

    // v2 visibility 在同一椭圆坐标域中评估，并在 grazing 域加入与 alpha
    // 相关的连续衰减；这不是旧 Neubelt 的 NdotL/NdotV 直接复用。
    float baseDenominator = 4.0 * (
        warpedNdotL + warpedNdotV - warpedNdotL * warpedNdotV);
    float grazingDenominator = 4.0 * (1.0 - alpha) *
        (1.0 - warpedNdotL) * (1.0 - warpedNdotV);
    return 1.0 / (baseDenominator + grazingDenominator);
}

float ClothAnisotropicLobeResponse(
    float alpha,
    float anisotropy,
    vec3 localLightDirection,
    vec3 localViewDirection)
{
    float normalLight = localLightDirection.z;
    float normalView = localViewDirection.z;
    if (normalLight <= 0.0 || normalView <= 0.0)
    {
        return 0.0;
    }
    vec3 localHalfDirection = normalize(
        localLightDirection + localViewDirection);
    return ClothAnisotropicCharlieDistribution(
        alpha,
        anisotropy,
        localHalfDirection) *
        ClothAnisotropicVisibility(
            alpha,
            anisotropy,
            localLightDirection,
            localViewDirection);
}

float ClothAnisotropicSheenUnitResponse(
    float alpha,
    float anisotropy,
    float anisotropyCross,
    vec3 localLightDirection,
    vec3 localViewDirection)
{
    float primaryResponse = ClothAnisotropicLobeResponse(
        alpha,
        anisotropy,
        localLightDirection,
        localViewDirection);
    float crossResponse = ClothAnisotropicLobeResponse(
        alpha,
        -anisotropy,
        localLightDirection,
        localViewDirection);
    // Cross 参数只混合两个明确的正交轴向 closure，不改变 sheenColor 的
    // 能量语义；cross=0 且 anisotropy=0 时与 v1 完全同构。
    return mix(
        primaryResponse,
        0.5 * (primaryResponse + crossResponse),
        anisotropyCross);
}

#endif
