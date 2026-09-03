#ifndef VL_M_PREINTEGRATED_SKIN_SURFACE_GLSL
#define VL_M_PREINTEGRATED_SKIN_SURFACE_GLSL

#include "materialFunction/mf_preintegratedSkinInputs.glsl"

// ID 3 将作者参数和 stable skinLutId 写入 GBuffer；response 与 transmission 由 GPU LUT 消费。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPreintegratedSkinInputs(context);
    inputs.modelInputs.preintegratedSkin.characterLighting =
        u_skinCharacterLighting;
    return inputs;
}

#endif
