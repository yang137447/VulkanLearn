#ifndef VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL
#define VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL

#if MATERIAL_USES_OPACITY_MASK
#include "../../materialFunction/mf_alphaClip.glsl"

layout(location = 0) in MaterialVaryings v2f;

void main()
{
    MaterialFunctionContext context = CreateMaterialFunctionContext(v2f);
    MaterialInputs inputs = EvaluateMaterialInputs(context);
    ApplyAlphaClip(inputs.opacityMask, u_alphaClipThreshold);
}
#else
void main()
{
}
#endif

#endif
