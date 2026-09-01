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
    // ShadowDepth 与主 Pass 使用同一原始 coverage mask；Hair coverage 只属于
    // lighting visibility，不能让投影边界和 Core Pass 再发生一次乘法。
    ApplyAlphaClip(inputs.opacityMask, u_alphaClipThreshold);
}
#else
void main()
{
}
#endif

#endif
