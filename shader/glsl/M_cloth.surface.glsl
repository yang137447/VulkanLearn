#ifndef VL_M_CLOTH_SURFACE_GLSL
#define VL_M_CLOTH_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    // Cloth 参数在材质入口冻结到 MaterialInputs；后续 Surface/GBuffer/lighting
    // 只消费这份快照，避免 Forward 与 Deferred 分别读取 UBO 造成语义漂移。
    inputs.modelInputs.cloth.sheenColor = u_clothSheenColor.rgb;
    inputs.modelInputs.cloth.sheenRoughness = u_clothSheenRoughness;
    inputs.modelInputs.cloth.anisotropy = u_clothAnisotropy;
    inputs.modelInputs.cloth.anisotropyCross = u_clothAnisotropyCross;
    inputs.metallic = 0.0;
    return inputs;
}

#endif
