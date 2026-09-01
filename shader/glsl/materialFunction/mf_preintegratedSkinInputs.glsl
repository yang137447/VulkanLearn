#ifndef VL_MATERIAL_FUNCTION_PREINTEGRATED_SKIN_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_PREINTEGRATED_SKIN_INPUTS_GLSL

#include "mf_pbrInputs.glsl"

// 通用 PreintegratedSkin 只负责标准 PBR 输入和 LUT 参数；NeoX 的贴图语义由
// mf_neoxSkinTextures / mf_neoxSkinInputs 独立承载，避免角色分支污染公共母材质。
MaterialInputs EvaluateMFPreintegratedSkinInputs(
    in MaterialFunctionContext context)
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