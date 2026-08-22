#ifndef VL_M_SPEEDTREE_SURFACE_GLSL
#define VL_M_SPEEDTREE_SURFACE_GLSL

#include "materialFunction/mf_speedtreeInputs.glsl"

// M_speedtree 的公开 Surface 入口；Base 和 ShadowDepth 共用同一份覆盖率语义。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFSpeedTreeInputs(context);
}

#endif
