#ifndef VL_TERRAIN_BAKE_WORLD_TYPES
#define VL_TERRAIN_BAKE_WORLD_TYPES
struct TerrainBakeSurface
{
    uvec4 page;
    uvec4 coverage;
    uvec4 counts;
    vec4 surfaceUv;
    vec4 coverageUv;
    vec4 frameUv;
    vec4 material;
    uvec4 adjacency;
    mat4 normalToSource;
    uint globalToSlice[256];
    uint localToGlobal[256];
};
#endif
