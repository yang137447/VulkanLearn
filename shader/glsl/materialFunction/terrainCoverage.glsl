#ifndef VL_TERRAIN_COVERAGE_GLSL
#define VL_TERRAIN_COVERAGE_GLSL

uint TerrainCoverageBilinear(
    sampler2D pageTable, sampler2D idAtlas, sampler2D weightAtlas,
    vec2 virtualUv, int pageResolution, ivec4 virtualTexelBounds,
    out uvec4 ids, out vec4 weights)
{
    ids = uvec4(0);
    weights = vec4(0);
    const ivec2 tableSize = textureSize(pageTable, 0);
    const ivec2 atlasSize = textureSize(idAtlas, 0);
    if (pageResolution < 1 || any(notEqual(atlasSize, textureSize(weightAtlas, 0))) ||
        any(isnan(virtualUv)) || any(isinf(virtualUv)))
        return 2u;
    const ivec2 virtualSize = tableSize * pageResolution;
    const ivec2 minimum = clamp(virtualTexelBounds.xy, ivec2(0), virtualSize - 1);
    const ivec2 maximum = clamp(virtualTexelBounds.zw, minimum, virtualSize - 1);
    const vec2 position = clamp(
        clamp(virtualUv, 0.0, 1.0) * vec2(virtualSize) - 0.5,
        vec2(minimum), vec2(maximum));
    const ivec2 base = ivec2(floor(position));
    const vec2 fraction = fract(position);
    uint candidateIds[16];
    float candidateWeights[16];
    int candidateCount = 0;
    uint status = 0u;
    for (int corner = 0; corner < 4; ++corner)
    {
        const ivec2 offset = ivec2(corner & 1, corner >> 1);
        const float factor = (offset.x == 0 ? 1.0 - fraction.x : fraction.x) *
            (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        if (factor <= 0.0)
            continue;
        const ivec2 texel = clamp(base + offset, minimum, maximum);
        const ivec2 page = texel / pageResolution;
        const vec4 mapping = texelFetch(pageTable, page, 0);
        if (any(isnan(mapping)) || any(isinf(mapping)))
        {
            status |= 2u;
            continue;
        }
        if (mapping.a < 0.5)
        {
            status |= 1u;
            continue;
        }
        const ivec2 physical = ivec2(round(mapping.xy * 255.0));
        const ivec2 atlasTexel = physical * pageResolution + texel % pageResolution;
        if (any(lessThan(physical, ivec2(0))) ||
            any(greaterThanEqual(atlasTexel, atlasSize)))
        {
            status |= 2u;
            continue;
        }
        const vec4 encodedIds = texelFetch(idAtlas, atlasTexel, 0);
        const vec4 tapWeights = texelFetch(weightAtlas, atlasTexel, 0);
        if (any(isnan(encodedIds)) || any(isinf(encodedIds)) ||
            any(isnan(tapWeights)) || any(isinf(tapWeights)))
        {
            status |= 16u;
            continue;
        }
        const uvec4 tapIds = uvec4(round(clamp(encodedIds, 0.0, 1.0) * 255.0));
        for (int slot = 0; slot < 4; ++slot)
        {
            const float contribution = max(tapWeights[slot], 0.0) * factor;
            if (contribution <= 0.0)
                continue;
            int candidate = 0;
            while (candidate < candidateCount && candidateIds[candidate] != tapIds[slot])
                ++candidate;
            if (candidate == candidateCount)
            {
                candidateIds[candidate] = tapIds[slot];
                candidateWeights[candidate] = 0.0;
                ++candidateCount;
            }
            candidateWeights[candidate] += contribution;
        }
    }
    if (status != 0u)
        return status;
    weights = vec4(0);
    ids = uvec4(0xffffffffu);
    for (int candidate = 0; candidate < candidateCount; ++candidate)
    {
        const float weight = candidateWeights[candidate];
        const uint layer = candidateIds[candidate];
        for (int slot = 0; slot < 4; ++slot)
            if (weight > weights[slot] ||
                (weight == weights[slot] && layer < ids[slot]))
            {
                for (int move = 3; move > slot; --move)
                {
                    weights[move] = weights[move - 1];
                    ids[move] = ids[move - 1];
                }
                weights[slot] = weight;
                ids[slot] = layer;
                break;
            }
    }
    const float total = dot(weights, vec4(1));
    if (total < 1.0e-4)
    {
        ids = uvec4(0);
        weights = vec4(1, 0, 0, 0);
    }
    else
    {
        weights /= total;
        for (int slot = 0; slot < 4; ++slot)
            if (weights[slot] == 0.0)
                ids[slot] = 0u;
    }
    return status;
}

uint TerrainCoverageBilinear(
    sampler2D pageTable, sampler2D idAtlas, sampler2D weightAtlas,
    vec2 virtualUv, int pageResolution, out uvec4 ids, out vec4 weights)
{
    return TerrainCoverageBilinear(pageTable, idAtlas, weightAtlas,
        virtualUv, pageResolution,
        ivec4(ivec2(0), textureSize(pageTable, 0) * pageResolution - 1), ids, weights);
}

#endif
