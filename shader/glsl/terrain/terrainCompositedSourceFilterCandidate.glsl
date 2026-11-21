#ifndef VL_TERRAIN_SOURCE_FILTER_CANDIDATE
#define VL_TERRAIN_SOURCE_FILTER_CANDIDATE

vec4 TerrainBakeSourceBilinear(sampler2DArray source, vec2 uv, int layer, int level)
{
    const ivec2 extent = textureSize(source, level).xy;
    precise vec2 position = fract(uv) * vec2(extent) - 0.5;
    const ivec2 base = ivec2(floor(position));
    precise vec2 fraction = position - vec2(base);
    vec4 result = vec4(0);
    for (int vertical = 0; vertical < 2; ++vertical)
        for (int horizontal = 0; horizontal < 2; ++horizontal)
        {
            const ivec2 address = ((base + ivec2(horizontal, vertical)) % extent + extent) % extent;
            const vec4 value = texelFetch(source, ivec3(address, layer), level);
            precise float weight = (horizontal != 0 ? fraction.x : 1.0 - fraction.x) *
                (vertical != 0 ? fraction.y : 1.0 - fraction.y);
            precise vec4 contribution = value * weight;
            result += contribution;
        }
    return result;
}

vec4 TerrainBakeSourceTrilinear(sampler2DArray source, vec3 uvLayer, float requestedLod)
{
    const int finalLevel = textureQueryLevels(source) - 1;
    const float lod = clamp(requestedLod, 0.0, float(finalLevel));
    const int first = int(floor(lod));
    const int second = min(first + 1, finalLevel);
    const float fraction = lod - float(first);
    const int layer = int(uvLayer.z);
    const vec4 low = TerrainBakeSourceBilinear(source, uvLayer.xy, layer, first);
    if (first == second || fraction == 0.0)
        return low;
    const vec4 high = TerrainBakeSourceBilinear(source, uvLayer.xy, layer, second);
    precise vec4 lowContribution = low * (1.0 - fraction);
    precise vec4 highContribution = high * fraction;
    return lowContribution + highContribution;
}

#endif
