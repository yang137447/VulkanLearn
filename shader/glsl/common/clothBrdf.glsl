#ifndef VL_COMMON_CLOTH_BRDF_GLSL
#define VL_COMMON_CLOTH_BRDF_GLSL

#include "defines.glsl"

const float CLOTH_MIN_SHEEN_ROUGHNESS = 0.02;
const float CLOTH_MAX_SHEEN_ROUGHNESS = 1.0;
const float CLOTH_DIRECTIONAL_ALBEDO_MIN_ALPHA = 0.0004;

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

#endif
