#ifndef VL_M_PBR_SURFACE_GLSL
#define VL_M_PBR_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// M_pbr 的公开 Surface 入口。
// 具体纹理采样和 PBR 输入生成由可复用 MF 完成，ShadingModel 由 M_pbr.json 选择。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
#if USE_ALPHA_MAP
    // 发片 alpha 与颜色贴图分离时，PBR 对比项也必须使用同一 coverage 来源。
    float alpha = texture(alphaMap, context.texCoord).r * u_tintColor.a;
    inputs.opacity = alpha;
    inputs.opacityMask = alpha;
#endif
    return inputs;
}

#endif
