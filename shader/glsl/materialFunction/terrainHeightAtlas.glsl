#ifndef VL_TERRAIN_HEIGHT_ATLAS_GLSL
#define VL_TERRAIN_HEIGHT_ATLAS_GLSL

float SampleTerrainHeightAtlas(
    sampler2DArray heightAtlas, sampler2D layerMap,
    uint globalLayer, vec2 tilingUV)
{
    vec4 mapping = texelFetch(layerMap, ivec2(int(globalLayer), 0), 0);
    vec2 sourceSize = mapping.zw;
    if (any(lessThan(sourceSize, vec2(1.0))))
        return 0.5;
    vec2 sourcePosition = fract(tilingUV) * sourceSize;
    uvec2 page = uvec2(floor(sourcePosition / 128.0));
    uint physicalPage = uint(mapping.x) + page.y * uint(mapping.y) + page.x;
    ivec2 atlasSize = textureSize(heightAtlas, 0).xy;
    uint columns = uint(atlasSize.x) / 136u;
    ivec2 pageOrigin = ivec2(physicalPage % columns, physicalPage / columns) * 136;
    vec2 sourceTexel = sourcePosition - vec2(0.5);
    ivec2 sourceBase = ivec2(floor(sourceTexel));
    vec2 blend = sourceTexel - vec2(sourceBase);
    ivec2 atlasBase = pageOrigin + ivec2(4) + sourceBase - ivec2(page) * 128;
    float lowerLeft = texelFetch(heightAtlas, ivec3(atlasBase, 0), 0).r;
    float lowerRight = texelFetch(heightAtlas, ivec3(atlasBase + ivec2(1, 0), 0), 0).r;
    float upperLeft = texelFetch(heightAtlas, ivec3(atlasBase + ivec2(0, 1), 0), 0).r;
    float upperRight = texelFetch(heightAtlas, ivec3(atlasBase + ivec2(1), 0), 0).r;
    return mix(mix(lowerLeft, lowerRight, blend.x),
        mix(upperLeft, upperRight, blend.x), blend.y);
}

#endif
