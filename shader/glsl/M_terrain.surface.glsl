#ifndef VL_M_TERRAIN_SURFACE_GLSL
#define VL_M_TERRAIN_SURFACE_GLSL

#include "materialFunction/mf_terrainLayerBlend.glsl"

// M_terrain 的公开 Surface 入口；层权重/top-4/数组采样都在共享 MF 中。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFTerrainInputs(context);
}

#endif
