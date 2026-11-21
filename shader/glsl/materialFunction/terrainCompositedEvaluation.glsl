#ifndef VL_TERRAIN_COMPOSITED_EVALUATION_GLSL
#define VL_TERRAIN_COMPOSITED_EVALUATION_GLSL

// fragment 与 compute 共享基础层公式；光照、全局图与视距后处理不属于此评价器。
vec2 TerrainCompositedLayerUv(vec2 surfaceUv, float uvScale, vec4 extra)
{
    const float angle = extra.w;
    return mat2(cos(angle), sin(angle), -sin(angle), cos(angle)) *
        (surfaceUv * uvScale * extra.z);
}

void TerrainCompositedAccumulate(
    inout vec4 albedo,
    inout vec4 orm,
    inout vec3 normalSum,
    vec4 sourceAlbedo,
    vec4 sourceOrm,
    vec3 sourceNormalTS,
    vec3 surfaceNormal,
    vec4 surfaceTangent,
    float weight)
{
    // 与原 MF 一致：保留插值 N/T，B=cross(N,T)*handedness，仅归一化结果。
    const vec3 bitangent =
        cross(surfaceNormal, surfaceTangent.xyz) * surfaceTangent.w;
    const vec3 transformed =
        sourceNormalTS.x * surfaceTangent.xyz +
        sourceNormalTS.y * bitangent +
        sourceNormalTS.z * surfaceNormal;
    const vec3 layerNormal = dot(transformed, transformed) > 1.0e-8
        ? normalize(transformed) : normalize(surfaceNormal);
    albedo += sourceAlbedo * weight;
    orm += sourceOrm * weight;
    normalSum += layerNormal * weight;
}

vec3 TerrainCompositedNormal(vec3 normalSum, vec3 fallbackNormal)
{
    return dot(normalSum, normalSum) > 1.0e-8
        ? normalize(normalSum) : normalize(fallbackNormal);
}

float TerrainCompositedHeightScore(
    float height, float displacementM, float strength, float fade, float weight)
{
    const float heightMeters = (height - 0.5) * displacementM;
    return clamp(heightMeters * strength, -0.25, 0.25) * fade + (1.0 - weight);
}

void TerrainCompositedHeightWeights(in float scores[4], inout float weights[4])
{
    float maximum = -1.0e9;
    for (int slot = 0; slot < 4; ++slot)
        maximum = max(maximum, scores[slot]);
    float total = 0.0;
    for (int slot = 0; slot < 4; ++slot)
    {
        if (weights[slot] > 1.0e-5)
            weights[slot] *= clamp(scores[slot] - maximum + 1.0, 0.0, 1.0);
        total += weights[slot];
    }
    if (total > 1.0e-5)
    {
        for (int slot = 0; slot < 4; ++slot)
            weights[slot] /= total;
    }
    else
    {
        weights[0] = 1.0;
        weights[1] = 0.0;
        weights[2] = 0.0;
        weights[3] = 0.0;
    }
}

vec3 TerrainCompositedLinearToSrgb(vec3 linearColour)
{
    const vec3 colour = clamp(linearColour, 0.0, 1.0);
    return mix(
        1.055 * pow(colour, vec3(1.0 / 2.4)) - 0.055,
        colour * 12.92,
        lessThanEqual(colour, vec3(0.0031308)));
}

#endif
