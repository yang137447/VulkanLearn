#ifndef VL_MATERIAL_FUNCTION_THIN_TRANSLUCENT_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_THIN_TRANSLUCENT_INPUTS_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "mf_normal.glsl"

// Thin Translucent MF 同时生成基础表面输入和模型专用透射输入，最终光照仍由 ShadingModel 完成。
MaterialInputs EvaluateMFThinTranslucentInputs(
    in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();

    vec3 baseColor = u_baseColorOpacity.rgb;
    #if USE_BASE_COLOR_MAP
        baseColor *= texture(baseColorMap, context.texCoord).rgb;
    #endif
    inputs.baseColor = baseColor * context.vertexColor.rgb;
    inputs.opacity = u_baseColorOpacity.a;
    inputs.opacityMask = u_baseColorOpacity.a;

    inputs.modelInputs.thinTranslucent.transmittanceColor =
        u_transmittanceColorCoverage.rgb;
    #if USE_TRANSMITTANCE_MAP
        inputs.modelInputs.thinTranslucent.transmittanceColor *=
            texture(transmittanceMap, context.texCoord).rgb;
    #endif
    inputs.modelInputs.thinTranslucent.surfaceCoverage =
        u_transmittanceColorCoverage.a;

    // u_surfaceFactors: x=roughness, y=metallic, z=specular, w=AO。
    inputs.roughness = u_surfaceFactors.x;
    inputs.metallic = u_surfaceFactors.y;
    inputs.specular = u_surfaceFactors.z;
    inputs.ambientOcclusion = u_surfaceFactors.w;

    inputs.emissiveColor =
        u_emissiveColorStrength.rgb *
        u_emissiveColorStrength.a;
    #if USE_EMISSION_MAP
        inputs.emissiveColor *=
            texture(emissionMap, context.texCoord).rgb;
    #endif

    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    #if USE_NORMAL_MAP
        vec3 normalTS =
            texture(normalMap, context.texCoord).xyz * 2.0 - 1.0;
        // 这条法线同时参与表面反射、透射 Fresnel 和 NoV 吸收路径长度计算。
        inputs.normal = TransformMaterialNormalToWorld(context, normalTS);
    #endif

    #if MATERIAL_TWO_SIDED
        // 双面材质翻转背面法线，使表面反射与透射都使用当前可见界面的方向。
        inputs.normal = gl_FrontFacing ? inputs.normal : -inputs.normal;
    #endif

    return inputs;
}

#endif
