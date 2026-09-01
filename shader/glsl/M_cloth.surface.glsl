#ifndef VL_M_CLOTH_SURFACE_GLSL
#define VL_M_CLOTH_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"
#include "materialFunction/mf_clothInputs.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    MFClothInputs cloth = EvaluateMFClothInputs(
        u_clothSheenColor.rgb,
        u_clothSheenRoughness,
        u_clothAnisotropy,
        u_clothAnisotropyCross);
    return ApplyMFClothInputs(inputs, cloth);
}

#endif
