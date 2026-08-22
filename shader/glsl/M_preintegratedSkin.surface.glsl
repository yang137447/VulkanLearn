#ifndef VL_M_PREINTEGRATED_SKIN_SURFACE_GLSL
#define VL_M_PREINTEGRATED_SKIN_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// ID 3 将作者参数和 stable skinLutId 写入 GBuffer；response 与 transmission 由 GPU LUT 消费。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    inputs.modelInputs.preintegratedSkin.skinLutId = u_skinLutId;
    inputs.modelInputs.preintegratedSkin.thickness = u_skinSurface.x;
    inputs.modelInputs.preintegratedSkin.thicknessScale = u_skinSurface.y;
    inputs.modelInputs.preintegratedSkin.weight = u_skinSurface.z;
    inputs.modelInputs.preintegratedSkin.curvature = u_skinSurface.w;
    inputs.modelInputs.preintegratedSkin.transmissionWeight =
        u_skinTransmissionWeight;
    return inputs;
}

#endif
