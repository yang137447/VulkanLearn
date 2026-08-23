#ifndef VL_M_HAIR_SURFACE_GLSL
#define VL_M_HAIR_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// Hair Material Function 只把作者参数转换为稳定 HairMaterialInputs；R/TT/TRT
// 的界面权重、Beer-Lambert 和 LUT 采样统一由 ShadingModel evaluator 消费。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    inputs.specular = u_pbrFactors.w;
    inputs.modelInputs.hair.scatter = u_hairScattering.x;
    inputs.modelInputs.hair.backlit = u_hairScattering.y;
    inputs.modelInputs.hair.longitudinalRoughness = u_hairScattering.z;
    inputs.modelInputs.hair.azimuthalRoughness = u_hairScattering.w;
    inputs.modelInputs.hair.ior = u_hairOptical.y;
    inputs.modelInputs.hair.fiberRadius = u_hairOptical.z;
    inputs.modelInputs.hair.cuticleTilt = u_hairOptical.w;
    inputs.modelInputs.hair.absorption = max(
        inputs.baseColor * u_hairOptical.x,
        vec3(0.001));
    inputs.modelInputs.hair.coverage = u_hairCoverage.x;
    inputs.modelInputs.hair.density = u_hairCoverage.z;
    inputs.modelInputs.hair.multipleScatteringWeight = u_hairCoverage.y;
    // opacityMask 仍由 BaseColor/alpha map 提供；coverage 只进入 Hair closure，不能
    // 被错误地当作 Beer-Lambert 吸收或在 ShadowDepth 中重新解释。
    return inputs;
}

#endif
