#ifndef VL_MATERIAL_FUNCTION_SUBSURFACE_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_SUBSURFACE_INPUTS_GLSL

#include "../engine/materialInputs.glsl"

// 该 MF 只把作者参数整理为 Subsurface 专属输入，不计算灯光、透射或 LUT 响应。
struct MFSubsurfaceInput
{
    vec3 color;
    float weight;
    float wrapWidth;
    float backscatterPower;
    float backscatterWeight;
    float thickness;
    float transmissionWeight;
};

SubsurfaceMaterialInputs EvaluateMFSubsurfaceInputs(
    in MFSubsurfaceInput subsurfaceInput)
{
    SubsurfaceMaterialInputs outputValue;
    outputValue.color = subsurfaceInput.color;
    outputValue.weight = subsurfaceInput.weight;
    outputValue.wrapWidth = subsurfaceInput.wrapWidth;
    outputValue.backscatterPower =
        subsurfaceInput.backscatterPower;
    outputValue.backscatterWeight =
        subsurfaceInput.backscatterWeight;
    outputValue.thickness = subsurfaceInput.thickness;
    outputValue.transmissionWeight =
        subsurfaceInput.transmissionWeight;
    return outputValue;
}

#endif
