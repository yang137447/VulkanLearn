#ifndef VL_M_THIN_TRANSLUCENT_SURFACE_GLSL
#define VL_M_THIN_TRANSLUCENT_SURFACE_GLSL

#include "materialFunction/mf_thinTranslucentInputs.glsl"

// M_thinTranslucent 的公开 Surface 入口只填充薄介质输入。
// 最终 Add/Mul 输出和透射光照由 ThinTranslucent ShadingModel/Forward Output 负责。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFThinTranslucentInputs(context);
}

#endif
