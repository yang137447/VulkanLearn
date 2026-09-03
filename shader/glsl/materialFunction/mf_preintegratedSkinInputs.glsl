#ifndef VL_MATERIAL_FUNCTION_PREINTEGRATED_SKIN_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_PREINTEGRATED_SKIN_INPUTS_GLSL

#include "mf_pbrInputs.glsl"

// 通用 PreintegratedSkin 只负责标准 PBR 输入和 LUT 参数；NeoX 的贴图语义由
// mf_neoxSkinTextures / mf_neoxSkinInputs 独立承载，避免角色分支污染公共母材质。
MaterialInputs EvaluateMFPreintegratedSkinInputs(
    in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    float skinMask = 1.0;
    float skinCurvature = u_skinSurface.w;
    float skinThickness = u_skinSurface.x;
    float skinWeight = u_skinSurface.z;
    float skinTransmissionWeight = u_skinTransmissionWeight;
    // Bottom normal 只负责 Skin LUT 的散射方向；顶层法线继续负责高光。
    inputs.modelInputs.preintegratedSkin.bottomNormal = inputs.normal;
#if USE_SKIN_PARAM_MAP
    vec4 skinParam = texture(skinParamMap, context.texCoord);
    inputs.roughness = skinParam.r;
    inputs.metallic = skinParam.g;
    inputs.ambientOcclusion = skinParam.a;
    skinMask = skinParam.b;
#endif
#if USE_SKIN_AUX_MAP
    vec4 skinAux = texture(skinAuxMap, context.texCoord);
    skinCurvature = skinAux.r;
#endif
#if USE_SKIN_DETAIL_MAP
    vec4 skinDetail = texture(skinDetailMap, context.texCoord);
    vec3 detailNormal = TransformMaterialNormalToWorld(
        context,
        skinDetail.rgb * 2.0 - 1.0);
    float detailMask = 1.0;
#if USE_SKIN_AUX_MAP
    detailMask = texture(skinAuxMap, context.texCoord).g;
#endif
    inputs.modelInputs.preintegratedSkin.bottomNormal = normalize(mix(
        inputs.normal,
        detailNormal,
        u_skinDetailStrength * detailMask));
    inputs.roughness = mix(
        inputs.roughness,
        inputs.roughness * skinDetail.a,
        u_skinPoreStrength);
#endif
    inputs.modelInputs.preintegratedSkin.skinLutId = u_skinLutId;
    inputs.modelInputs.preintegratedSkin.thickness = u_skinSurface.x;
    inputs.modelInputs.preintegratedSkin.thicknessScale = u_skinSurface.y;
    inputs.modelInputs.preintegratedSkin.weight =
        u_skinSurface.z * skinMask;
    inputs.modelInputs.preintegratedSkin.curvature = skinCurvature;
#if USE_SKIN_THICKNESS_MAP
    skinThickness *= texture(skinThicknessMap, context.texCoord).r;
#endif
#if USE_SKIN_WEIGHT_MAP
    skinWeight *= texture(skinWeightMap, context.texCoord).r;
#endif
#if USE_SKIN_TRANSMISSION_MAP
    skinTransmissionWeight *=
        texture(skinTransmissionMap, context.texCoord).r;
#endif
    inputs.modelInputs.preintegratedSkin.thickness = skinThickness;
    inputs.modelInputs.preintegratedSkin.weight = skinWeight * skinMask;
    inputs.modelInputs.preintegratedSkin.transmissionWeight =
        skinTransmissionWeight;
    return inputs;
}

#endif
