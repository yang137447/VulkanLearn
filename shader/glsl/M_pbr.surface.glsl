#ifndef VL_M_PBR_SURFACE_GLSL
#define VL_M_PBR_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// M_pbr 的公开 Surface 入口。
// 具体纹理采样和 PBR 输入生成由可复用 MF 完成，ShadingModel 由 M_pbr.json 选择。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFPbrInputs(context);
}

#endif
