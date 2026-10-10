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
    // 非金属 Specular=0 在实时直接光和 IBL 中关闭镜面 lobe，得到 Lambert；
    // 实时可见性仍用 CSM/SH，论文离线积分与光线可见性由 CPU evaluator 拥有。
    return inputs;
}

#endif
