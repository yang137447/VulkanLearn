#ifndef VL_TERRAIN_QUALITY_VISIBLE
#define VL_TERRAIN_QUALITY_VISIBLE

#include "../materialFunction/terrainCompositedFootprint.glsl"

#ifndef VL_QUALITY_SET
#define VL_QUALITY_SET 3
#endif
#ifndef VL_QUALITY_BUFFER_BINDING
#define VL_QUALITY_BUFFER_BINDING 5
#endif
#ifndef VL_QUALITY_DEPTH_BINDING
#define VL_QUALITY_DEPTH_BINDING 6
#endif
#ifndef VL_QUALITY_PRIMITIVE_ID
#define VL_QUALITY_PRIMITIVE_ID 0
#endif

layout(set = VL_QUALITY_SET, binding = VL_QUALITY_BUFFER_BINDING, std430)
buffer TerrainQualityCapture { uint terrainQualityWords[]; };
layout(set = VL_QUALITY_SET, binding = VL_QUALITY_DEPTH_BINDING)
uniform sampler2D terrainQualityFinalDepth;

void TerrainQualityVisibleSample(
    vec2 uv, TerrainCompositedFootprint footprint, uint surfaceIndex, float bias)
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(128))))
    {
        atomicAdd(terrainQualityWords[5], 1u);
        atomicAdd(terrainQualityWords[21], 1u);
        atomicAdd(terrainQualityWords[22], 1u);
        return;
    }
    if (gl_FragCoord.z != texelFetch(terrainQualityFinalDepth, pixel, 0).r)
        return;
    if (terrainQualityWords[0] != 0x31515654u || terrainQualityWords[1] != 2u ||
        terrainQualityWords[2] != 16384u)
        return;
    uint address = 64u + uint(pixel.y * 128 + pixel.x) * 8u;
    uint prior = atomicAdd(terrainQualityWords[address + 7u], 1u);
    if (prior != 0u)
    {
        atomicAdd(terrainQualityWords[4], 1u);
        uint witness = atomicAdd(terrainQualityWords[23], 1u);
        if (witness < 256u)
        {
            uint duplicateAddress = 401728u + witness * 12u;
            terrainQualityWords[duplicateAddress] = uint(pixel.y * 128 + pixel.x);
            terrainQualityWords[duplicateAddress + 1u] = prior;
            terrainQualityWords[duplicateAddress + 2u] = floatBitsToUint(uv.x);
            terrainQualityWords[duplicateAddress + 3u] = floatBitsToUint(uv.y);
            terrainQualityWords[duplicateAddress + 4u] = floatBitsToUint(footprint.majorUv.x);
            terrainQualityWords[duplicateAddress + 5u] = floatBitsToUint(footprint.majorUv.y);
            terrainQualityWords[duplicateAddress + 6u] = floatBitsToUint(footprint.targetMip);
            terrainQualityWords[duplicateAddress + 7u] = floatBitsToUint(bias);
            terrainQualityWords[duplicateAddress + 8u] =
                surfaceIndex | (uint(footprint.tapCount) << 16);
#if VL_QUALITY_PRIMITIVE_ID
            terrainQualityWords[duplicateAddress + 9u] = uint(gl_PrimitiveID);
#else
            terrainQualityWords[duplicateAddress + 9u] = 0xffffffffu;
#endif
            terrainQualityWords[duplicateAddress + 10u] = floatBitsToUint(gl_FragCoord.z);
            terrainQualityWords[duplicateAddress + 11u] =
                floatBitsToUint(texelFetch(terrainQualityFinalDepth, pixel, 0).r);
        }
        else
            atomicAdd(terrainQualityWords[24], 1u);
        return;
    }
    atomicAdd(terrainQualityWords[3], 1u);
    terrainQualityWords[address] = floatBitsToUint(uv.x);
    terrainQualityWords[address + 1u] = floatBitsToUint(uv.y);
    terrainQualityWords[address + 2u] = floatBitsToUint(footprint.majorUv.x);
    terrainQualityWords[address + 3u] = floatBitsToUint(footprint.majorUv.y);
    terrainQualityWords[address + 4u] = floatBitsToUint(footprint.targetMip);
    terrainQualityWords[address + 5u] = floatBitsToUint(bias);
    terrainQualityWords[address + 6u] = surfaceIndex | (uint(footprint.tapCount) << 16);
}
#endif
