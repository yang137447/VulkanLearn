#ifndef VL_MATERIAL_FUNCTION_SPEEDTREE_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_SPEEDTREE_INPUTS_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "mf_normal.glsl"

// SpeedTree MF 只生成材质输入；Base 与 ShadowDepth 由同一个公开母材质入口消费覆盖率语义。
MaterialInputs EvaluateMFSpeedTreeInputs(
    in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();

    #if USE_ALBEDO_MAP
        vec4 albedoColor = texture(albedoMap, context.texCoord) * u_tintColor;
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    vec4 baseColor = albedoColor * vec4(context.vertexColor.rgb, 1.0);
    inputs.baseColor = baseColor.rgb;
    inputs.opacity = albedoColor.a;
    inputs.opacityMask = albedoColor.a;

    // pbrParamMap.r/g/b: roughness/metallic/ambient occlusion。
    #if USE_PBR_MAP
        vec4 pbrParam = texture(pbrParamMap, context.texCoord);
        inputs.roughness = pbrParam.x;
        inputs.metallic = pbrParam.y;
    #else
        inputs.roughness = u_pbrFactors.x;
        inputs.metallic = u_pbrFactors.y;
    #endif
    // 当前树资产把 AO 压在 vertexColor.a，统一以该通道作为最终 AO。
    inputs.ambientOcclusion = context.vertexColor.a;

    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    #if USE_NORMAL_MAP
        vec4 normalSample = texture(normalMap, context.texCoord);
        vec3 normalTS = normalSample.xyz * 2.0 - 1.0;
        inputs.normal = TransformMaterialNormalToWorld(context, normalTS);
        #if !USE_PBR_MAP
            inputs.roughness = 1.0 - normalSample.a;
        #endif
    #endif
    #if MATERIAL_TWO_SIDED
        inputs.normal = gl_FrontFacing ? inputs.normal : -inputs.normal;
    #endif

    return inputs;
}

#endif
