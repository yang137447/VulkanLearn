#ifndef VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL
#define VL_PASS_TEMPLATE_SHADOW_DEPTH_FRAG_GLSL

#ifndef VL_LOCAL_SHADOW
#define VL_LOCAL_SHADOW 0
#endif

#if MATERIAL_USES_OPACITY_MASK
#include "../../materialFunction/mf_alphaClip.glsl"
#include "../materialSurface.glsl"

layout(location = 0) in MaterialVaryings v2f;
#endif

#if VL_LOCAL_SHADOW
#include "../../common/localShadowDepth.glsl"
layout(location = 8) in vec3 vLocalShadowWorldPos;
#endif

void main()
{
#if MATERIAL_USES_OPACITY_MASK
    MaterialFunctionContext context = CreateMaterialFunctionContext(v2f);
    MaterialInputs inputs = EvaluateMaterialInputs(context);
    MaterialSurface surface = ResolveMaterialSurface(inputs, context);
    // ShadowDepth 与主 Pass 使用同一原始 coverage mask；Hair coverage 只属于
    // lighting visibility，不能让投影边界和 Core Pass 再发生一次乘法。
    ApplyAlphaClip(inputs.opacityMask, u_alphaClipThreshold);
#endif
#if VL_LOCAL_SHADOW
    // CSM 变体保持空片元；LocalShadow 必须先完成材质 alpha clip，再写
    // 八面体/透视深度，两个 pass 的覆盖边界才不会错位。
    WriteLocalShadowDepth(vLocalShadowWorldPos);
#endif
}

#endif
