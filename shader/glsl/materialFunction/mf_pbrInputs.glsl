#ifndef VL_MATERIAL_FUNCTION_PBR_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_PBR_INPUTS_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "mf_normal.glsl"

// 共享 PBR MF 只生成 MaterialInputs；具体母材质公开入口位于对应 M_*.json 同目录。
MaterialInputs EvaluateMFPbrInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();

    // 第一版 PBR 复用一张 albedo 贴图，BaseColor = 贴图 * Tint * 顶点色。
    #if USE_ALBEDO_MAP
        vec4 albedoColor = texture(albedoMap, context.texCoord) * u_tintColor;
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    vec4 baseColor = albedoColor * vec4(context.vertexColor.rgb, 1.0);
    inputs.baseColor = baseColor.rgb;
    inputs.opacity = baseColor.a;
    inputs.opacityMask = baseColor.a;

    // u_pbrFactors: x=roughness, y=metallic, z=ao, w=reserved。
    // 数据范围由 M_/MI_ 资产保证，运行时不重复做防御性截断。
    #if USE_PBR_MAP
        vec4 pbrParam = texture(pbrParamMap, context.texCoord);
        inputs.roughness = pbrParam.x;
        inputs.metallic = pbrParam.y;
        inputs.ambientOcclusion = pbrParam.z;
    #else
        inputs.roughness = u_pbrFactors.x;
        inputs.metallic = u_pbrFactors.y;
        inputs.ambientOcclusion = u_pbrFactors.z;
    #endif

    #if USE_EMISSION_MAP
        inputs.emissiveColor =
            texture(emissionMap, context.texCoord).rgb *
            u_emissiveStrength;
    #endif

    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    #if USE_NORMAL_MAP
        vec3 normalTS =
            texture(normalMap, context.texCoord).xyz * 2.0 - 1.0;
        // 保留顶点阶段传来的 T/B/N 方向，避免与烘焙端切线空间产生额外偏差。
        inputs.normal = TransformMaterialNormalToWorld(context, normalTS);
    #endif

    inputs.modelInputs.clearCoat.bottomNormal = inputs.normal;
    #if defined(VL_PBR_USE_CLEAR_COAT_INPUTS)
        vec2 clearCoatInputs = vec2(
            u_clearCoat,
            u_clearCoatRoughness);
        #if USE_CLEAR_COAT_MAP
            // 启用贴图后 RG 直接替代常量输入，不再与常量相乘。
            clearCoatInputs = texture(
                clearCoatMap,
                context.texCoord).rg;
        #endif
        inputs.modelInputs.clearCoat.weight = clearCoatInputs.x;
        inputs.modelInputs.clearCoat.roughness = clearCoatInputs.y;
    #endif

    #if USE_CLEAR_COAT_BOTTOM_NORMAL_MAP
        #if defined(VL_PBR_USE_CLEAR_COAT_BOTTOM_NORMAL_CONTROLS)
            vec2 bottomNormalTexCoord =
                context.texCoord * u_clearCoatBottomNormalTiling;
        #else
            vec2 bottomNormalTexCoord = context.texCoord;
        #endif
        vec3 bottomNormalTS =
            texture(
                clearCoatBottomNormalMap,
                bottomNormalTexCoord).xyz * 2.0 - 1.0;
        #if defined(VL_PBR_USE_CLEAR_COAT_BOTTOM_NORMAL_CONTROLS)
            // 对齐 Blender Normal Map Strength：在切线空间向中性法线混合后再转到世界空间。
            bottomNormalTS = normalize(mix(
                vec3(0.0, 0.0, 1.0),
                bottomNormalTS,
                u_clearCoatBottomNormalStrength));
        #endif
        inputs.modelInputs.clearCoat.bottomNormal =
            TransformMaterialNormalToWorld(context, bottomNormalTS);
    #endif

    #if MATERIAL_TWO_SIDED
        // 双面材质翻面时顶层和底层法线必须同步翻转，避免落在不同半球。
        inputs.normal = gl_FrontFacing ? inputs.normal : -inputs.normal;
        inputs.modelInputs.clearCoat.bottomNormal =
            gl_FrontFacing
                ? inputs.modelInputs.clearCoat.bottomNormal
                : -inputs.modelInputs.clearCoat.bottomNormal;
    #endif

    return inputs;
}

#endif
