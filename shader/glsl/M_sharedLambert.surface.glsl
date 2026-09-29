#ifndef VL_M_SHARED_LAMBERT_SURFACE_GLSL
#define VL_M_SHARED_LAMBERT_SURFACE_GLSL

#include "engine/materialInputs.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    inputs.baseColor = u_baseColor.rgb;
    inputs.metallic = 0.0;
    inputs.specular = 0.0;
    inputs.roughness = 1.0;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    // 实时 DefaultLit 仅作粗糙漫反射近似；论文离线积分仍由 CPU Lambert evaluator 拥有。
    return inputs;
}

#endif
