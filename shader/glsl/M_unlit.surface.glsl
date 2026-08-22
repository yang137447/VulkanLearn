#ifndef VL_M_UNLIT_SURFACE_GLSL
#define VL_M_UNLIT_SURFACE_GLSL

#include "materialFunction/mf_normal.glsl"
#include "engine/materialInputs.glsl"

// M_unlit 的公开 Surface 入口；Unlit ShadingModel 只消费材质输入，不执行光照。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    #if USE_ALBEDO_MAP
        vec4 albedo = texture(albedoMap, context.texCoord);
    #else
        vec4 albedo = vec4(1.0);
    #endif
    vec4 color = albedo * u_tintColor * vec4(context.vertexColor.rgb, 1.0);
    inputs.baseColor = color.rgb;
    inputs.opacity = color.a;
    inputs.opacityMask = color.a;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif
