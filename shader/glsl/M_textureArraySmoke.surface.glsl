#ifndef VL_M_TEXTURE_ARRAY_SMOKE_SURFACE_GLSL
#define VL_M_TEXTURE_ARRAY_SMOKE_SURFACE_GLSL

#include "materialFunction/mf_normal.glsl"
#include "engine/materialInputs.glsl"
#include "common/function.glsl"

// 冒烟材质直接按 u_layer 选择数组层；两支球应分别显示第一层红色和第二层绿色。
// 这里不引入光照，任何层选择/上传错误都会直接表现为两球同色或采样失败。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    const int layer = int(u_layer + 0.5);
    const vec4 albedo = texture(layerMap, vec3(context.texCoord, float(layer)));
    inputs.baseColor = albedo.rgb;
    inputs.emissiveColor = albedo.rgb;
    inputs.opacity = albedo.a;
    inputs.opacityMask = albedo.a;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif
