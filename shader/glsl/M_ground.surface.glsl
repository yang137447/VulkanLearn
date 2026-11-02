#ifndef VL_M_GROUND_SURFACE_GLSL
#define VL_M_GROUND_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// 地面专用母材质：复用共享 PBR MF，只额外提供 UV 平铺。
// M_pbr 被其它场景共用，因此 tiling 不写进 M_pbr；这里把 texCoord 乘 u_tiling
// 后交给同一个 EvaluateMFPbrInputs，albedo/normal/PBR 通道语义与 M_pbr 完全一致。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialFunctionContext tiled = context;
    tiled.texCoord = context.texCoord * u_tiling;
    return EvaluateMFPbrInputs(tiled);
}

#endif
