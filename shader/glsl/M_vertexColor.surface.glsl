#ifndef VL_M_VERTEX_COLOR_SURFACE_GLSL
#define VL_M_VERTEX_COLOR_SURFACE_GLSL

#include "engine/materialInputs.glsl"
#include "common/function.glsl"

// M_vertexColor 的公开 Surface 入口；颜色空间转换在材质语义层完成。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    inputs.baseColor = mix(
        SRGBtoLinear(context.vertexColor.rgb),
        u_tintColor.rgb,
        u_tintColor.a);
    inputs.opacity = 1.0;
    inputs.opacityMask = 1.0;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif
