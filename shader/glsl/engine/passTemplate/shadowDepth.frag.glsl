#ifndef VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL
#define VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL

#if MATERIAL_USES_OPACITY_MASK
#include "../../materialFunction/mf_alphaClip.glsl"

layout(location = 0) in MaterialVaryings v2f;

void main()
{
    MaterialPixelContext pixel = CreateMaterialPixelContext(v2f);
    MaterialSurface surface = EvaluateMaterialSurface(pixel);
    ApplyAlphaClip(surface.opacity, u_alphaClipThreshold);
}
#else
void main()
{
}
#endif

#endif
