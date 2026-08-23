#ifndef VL_M_CLOTH_SURFACE_GLSL
#define VL_M_CLOTH_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    inputs.modelInputs.cloth.sheenColor = u_clothSheenColor.rgb;
    inputs.modelInputs.cloth.sheenRoughness = u_clothSheenRoughness;
    inputs.metallic = 0.0;
    return inputs;
}

#endif
