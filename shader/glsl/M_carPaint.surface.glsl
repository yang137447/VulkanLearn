#ifndef VL_M_CAR_PAINT_SURFACE_GLSL
#define VL_M_CAR_PAINT_SURFACE_GLSL

#define VL_PBR_USE_CLEAR_COAT_INPUTS 1
#define VL_PBR_USE_CLEAR_COAT_BOTTOM_NORMAL_CONTROLS 1
#include "materialFunction/mf_pbrInputs.glsl"

// M_carPaint 的公开 Surface 入口只填充底漆和 Clear Coat 模型输入。
// Clear Coat BRDF、GBuffer 编码和 Forward/Deferred 分发属于 Engine。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFPbrInputs(context);
}

#endif
