#ifndef VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL
#define VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL

#if MATERIAL_USES_OPACITY_MASK
#include "../../materialFunction/mf_alphaClip.glsl"
#include "../materialSurface.glsl"

layout(location = 0) in MaterialVaryings v2f;

void main()
{
    MaterialFunctionContext context = CreateMaterialFunctionContext(v2f);
    MaterialInputs inputs = EvaluateMaterialInputs(context);
    MaterialSurface surface = ResolveMaterialSurface(inputs, context);
    float resolvedOpacityMask = inputs.opacityMask;
    if (surface.shadingModel == SHADING_MODEL_HAIR)
    {
        // ShadowDepth 复用与主 Pass 相同的 coverage/opacity 顺序，避免边缘与阴影脱节。
        resolvedOpacityMask *= surface.modelInputs.hair.coverage;
    }
    ApplyAlphaClip(resolvedOpacityMask, u_alphaClipThreshold);
}
#else
void main()
{
}
#endif

#endif
