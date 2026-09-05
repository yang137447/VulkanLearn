#ifndef VL_MATERIAL_FUNCTION_TWO_SIDED_FOLIAGE_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_TWO_SIDED_FOLIAGE_INPUTS_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "mf_normal.glsl"

MaterialInputs EvaluateMFTwoSidedFoliageInputs(
    in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    #if USE_ALBEDO_MAP
        vec4 albedoColor = texture(albedoMap, context.texCoord) * u_tintColor;
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    // 叶片覆盖率固定来自 BaseColor 的 A 通道；不再引入第二份 Alpha 贴图语义。
    vec4 baseColor = albedoColor * vec4(context.vertexColor.rgb, 1.0);
    inputs.baseColor = baseColor.rgb;
    inputs.opacity = baseColor.a;
    inputs.opacityMask = inputs.opacity;
    #if USE_OPACITY_MASK_MAP
        // UE 的 Opacity Mask 是独立材质输入；BaseColor Alpha 只作为未绑定时的兼容回退。
        inputs.opacityMask = texture(
            opacityMaskMap,
            context.texCoord).r;
    #endif
    inputs.modelInputs.twoSidedFoliage.subsurfaceColor = u_subsurfaceColor.rgb;
    #if USE_SUBSURFACE_COLOR_MAP
        // 当前植物用 BaseColor 作为逐像素 Subsurface Color，再乘作者 tint，避免整片叶片共享常量透光色。
        inputs.modelInputs.twoSidedFoliage.subsurfaceColor *= texture(
            subsurfaceColorMap,
            context.texCoord).rgb;
    #endif
    #if USE_ROUGHNESS_MAP
        inputs.roughness = texture(roughnessMap, context.texCoord).r;
    #else
        inputs.roughness = u_pbrFactors.x;
    #endif
    inputs.metallic = u_pbrFactors.y;
    inputs.ambientOcclusion = u_pbrFactors.z;
    inputs.specular = u_specular;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = OrthonormalizeMaterialTangent(
        inputs.normal,
        context.worldTangent);
#if USE_NORMAL_MAP
    vec3 normalTS = texture(normalMap, context.texCoord).xyz * 2.0 - 1.0;
    inputs.normal = TransformMaterialNormalToWorld(context, normalTS);
    inputs.tangent = OrthonormalizeMaterialTangent(
        inputs.normal,
        context.worldTangent);
#endif
#if MATERIAL_TWO_SIDED
    // 双面路径先把法线翻到当前可见面的半球，再由 foliage lobe 独立计算背光。
    inputs.normal = gl_FrontFacing ? inputs.normal : -inputs.normal;
#endif
    return inputs;
}

#endif
