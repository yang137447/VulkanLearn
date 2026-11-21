#ifndef VL_TERRAIN_BAKE_WORLD_EVALUATOR
#define VL_TERRAIN_BAKE_WORLD_EVALUATOR

#include "../materialFunction/terrainCompositedEvaluation.glsl"
#include "terrainCompositedBakeCoverage.glsl"

#define TERRAIN_BAKE_PROTOTYPE_INPUTS \
    uint targetIndex, \
    sampler2D coveragePageTable, sampler2D coverageIds, sampler2D coverageWeights, \
    sampler2DArray localLayerWeights, sampler2D layerCatalog, \
    sampler2DArray layerAlbedo, sampler2DArray layerNormal, sampler2DArray layerOrm, \
    sampler2D surfaceLocalNormal, sampler2D surfaceLocalTangent, sampler2DArray layerHeight

#define TERRAIN_BAKE_PROTOTYPE_ARGUMENTS \
    targetIndex, coveragePageTable, coverageIds, coverageWeights, localLayerWeights, \
    layerCatalog, layerAlbedo, layerNormal, layerOrm, surfaceLocalNormal, \
    surfaceLocalTangent, layerHeight

#define Fail(flag) terrainDiagnostic |= (flag)
#define bake world.surfaces[targetIndex]

bool FiniteValue(vec4 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

float SourceLod(ivec2 extent, vec2 deltaU, vec2 deltaV)
{
    const float footprint = max(
        length(deltaU * vec2(extent)), length(deltaV * vec2(extent)));
    return max(0.0, log2(max(footprint, 1.0e-8)));
}

void ResolveLayers(
    TERRAIN_BAKE_PROTOTYPE_INPUTS,
    vec2 surfaceUv, vec2 domainUv, out uvec4 ids, out vec4 weights,
    inout uint terrainDiagnostic)
{
    ids = uvec4(0);
    weights = vec4(0);
    if (bake.coverage.w != 0u)
    {
        const ivec3 extent = textureSize(localLayerWeights, 0);
        const vec2 span = vec2(extent.xy - ivec2(1));
        const vec2 weightUv = (vec2(1) + surfaceUv * (span - 2.0)) / span;
        for (uint layer = 0u; layer < bake.counts.x; ++layer)
        {
            const float weight = textureLod(
                localLayerWeights, vec3(weightUv, float(layer)), 0.0).r;
            for (int slot = 0; slot < 4; ++slot)
            {
                if (weight > weights[slot])
                {
                    for (int move = 3; move > slot; --move)
                    {
                        weights[move] = weights[move - 1];
                        ids[move] = ids[move - 1];
                    }
                    weights[slot] = weight;
                    ids[slot] = bake.localToGlobal[layer];
                    break;
                }
            }
        }
    }
    else
    {
        const vec2 globalUv = clamp(
            bake.coverageUv.xy + domainUv * bake.coverageUv.zw, 0.0, 1.0);
        const uint status = TerrainCoverageBilinear(
            coveragePageTable, coverageIds, coverageWeights,
            globalUv, int(bake.coverage.z), ids, weights);
        if (status != 0u)
        {
            Fail(status);
            return;
        }
    }
    if (!FiniteValue(weights))
    {
        Fail(16u);
        weights = vec4(0);
    }
    const float total = dot(weights, vec4(1));
    if (total < 1.0e-4)
    {
        weights = vec4(1, 0, 0, 0);
        if (bake.coverage.w != 0u)
            ids.x = bake.localToGlobal[0];
    }
    else
        weights /= total;
}

void EvaluateBase(
    TERRAIN_BAKE_PROTOTYPE_INPUTS,
    vec2 domainUv, vec2 footprintU, vec2 footprintV,
    out vec3 albedo, out vec3 localNormal, out vec3 orm,
    inout uint terrainDiagnostic)
{
    const vec2 surfaceUv = bake.surfaceUv.xy + domainUv * bake.surfaceUv.zw;
    const vec2 frameUv = clamp(
        bake.frameUv.xy + domainUv * bake.frameUv.zw, 0.0, 1.0);
    const vec3 geometricNormal = textureLod(surfaceLocalNormal, frameUv, 0.0).xyz;
    const vec4 tangent = textureLod(surfaceLocalTangent, frameUv, 0.0);
    if (!FiniteValue(vec4(geometricNormal, 1)) || !FiniteValue(tangent) ||
        dot(geometricNormal, geometricNormal) < 1.0e-8 ||
        dot(tangent.xyz, tangent.xyz) < 1.0e-8 || abs(tangent.w) < 0.5)
    {
        Fail(8u);
        albedo = vec3(0);
        localNormal = vec3(0, 1, 0);
        orm = vec3(1);
        return;
    }
    uvec4 ids;
    vec4 weights;
    ResolveLayers(TERRAIN_BAKE_PROTOTYPE_ARGUMENTS,
        surfaceUv, domainUv, ids, weights, terrainDiagnostic);
    if (bake.material.y > 0.0)
    {
        float scores[4];
        float blendWeights[4];
        for (int slot = 0; slot < 4; ++slot)
        {
            blendWeights[slot] = weights[slot];
            scores[slot] = -1.0e9;
            if (weights[slot] <= 1.0e-5)
                continue;
            if (ids[slot] >= 256u || bake.globalToSlice[ids[slot]] >= bake.counts.y)
            {
                Fail(4u);
                continue;
            }
            const int globalLayer = int(ids[slot]);
            const vec4 extra = texelFetch(layerCatalog, ivec2(globalLayer, 1), 0);
            if (!FiniteValue(extra))
            {
                Fail(16u);
                continue;
            }
            const vec2 uv = TerrainCompositedLayerUv(surfaceUv, bake.material.x, extra);
            const float height = textureLod(layerHeight,
                vec3(uv, float(bake.globalToSlice[ids[slot]])), 3.0).r;
            const float displacementM =
                texelFetch(layerCatalog, ivec2(globalLayer, 3), 0).w;
            if (!FiniteValue(vec4(height, displacementM, extra.zw)))
            {
                Fail(16u);
                continue;
            }
            scores[slot] = TerrainCompositedHeightScore(
                height, displacementM, bake.material.y, bake.material.z, weights[slot]);
        }
        TerrainCompositedHeightWeights(scores, blendWeights);
        weights = vec4(blendWeights[0], blendWeights[1], blendWeights[2], blendWeights[3]);
    }
    vec4 colourSum = vec4(0);
    vec4 ormSum = vec4(0);
    vec3 normalSum = vec3(0);
    for (int slot = 0; slot < 4; ++slot)
    {
        if (weights[slot] <= 0.0)
            continue;
        const uint globalLayer = ids[slot];
        if (globalLayer >= 256u)
        {
            Fail(4u);
            continue;
        }
        const uint slice = bake.globalToSlice[globalLayer];
        if (slice >= bake.counts.y)
        {
            Fail(4u);
            continue;
        }
        const vec4 extra = texelFetch(layerCatalog, ivec2(int(globalLayer), 1), 0);
        if (!FiniteValue(extra))
        {
            Fail(16u);
            continue;
        }
        const vec2 layerUv = TerrainCompositedLayerUv(surfaceUv, bake.material.x, extra);
        const vec2 deltaU = TerrainCompositedLayerUv(
            footprintU * bake.surfaceUv.zw, bake.material.x, extra);
        const vec2 deltaV = TerrainCompositedLayerUv(
            footprintV * bake.surfaceUv.zw, bake.material.x, extra);
        const vec4 colour = textureLod(layerAlbedo, vec3(layerUv, float(slice)),
            SourceLod(textureSize(layerAlbedo, 0).xy, deltaU, deltaV));
        const vec4 packedOrm = textureLod(layerOrm, vec3(layerUv, float(slice)),
            SourceLod(textureSize(layerOrm, 0).xy, deltaU, deltaV));
        const vec3 normalTS = textureLod(layerNormal, vec3(layerUv, float(slice)),
            SourceLod(textureSize(layerNormal, 0).xy, deltaU, deltaV)).xyz * 2.0 - 1.0;
        if (!FiniteValue(colour) || !FiniteValue(packedOrm) ||
            !FiniteValue(vec4(normalTS, 1)))
        {
            Fail(16u);
            continue;
        }
        TerrainCompositedAccumulate(
            colourSum, ormSum, normalSum, colour, packedOrm, normalTS,
            geometricNormal, tangent, weights[slot]);
    }
    albedo = colourSum.rgb;
    orm = ormSum.rgb;
    localNormal = TerrainCompositedNormal(normalSum, geometricNormal);
}

#undef Fail
#undef bake
#endif
