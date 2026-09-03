#ifndef VL_M_TWO_SIDED_FOLIAGE_SURFACE_GLSL
#define VL_M_TWO_SIDED_FOLIAGE_SURFACE_GLSL

#include "materialFunction/mf_twoSidedFoliageInputs.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFTwoSidedFoliageInputs(context);
}

#endif
