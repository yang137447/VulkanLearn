#ifndef VL_MATERIAL_FUNCTION_TWO_SIDED_FOLIAGE_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_TWO_SIDED_FOLIAGE_INPUTS_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialInputs.glsl"
#include "mf_normal.glsl"

MaterialInputs EvaluateMFTwoSidedFoliageInputs(
    in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    vec4 baseColor = u_tintColor * vec4(context.vertexColor.rgb, 1.0);
    inputs.baseColor = baseColor.rgb;
    inputs.opacity = baseColor.a;
    inputs.opacityMask = baseColor.a;
    inputs.modelInputs.twoSidedFoliage.subsurfaceColor = u_subsurfaceColor;
    inputs.modelInputs.twoSidedFoliage.frontFacing = gl_FrontFacing ? 1.0 : 0.0;
    inputs.roughness = u_pbrFactors.x;
    inputs.metallic = u_pbrFactors.y;
    inputs.ambientOcclusion = u_pbrFactors.z;
    inputs.specular = u_specular;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = OrthonormalizeMaterialTangent(
        inputs.normal,
        context.worldTangent);
#if MATERIAL_TWO_SIDED
    // 双面路径先把法线翻到当前可见面的半球，再由 foliage lobe 独立计算背光。
    inputs.normal = gl_FrontFacing ? inputs.normal : -inputs.normal;
#endif
    return inputs;
}

#endif
