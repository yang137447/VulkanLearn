#ifndef VL_M_SUBSURFACE_SURFACE_GLSL
#define VL_M_SUBSURFACE_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"
#include "materialFunction/mf_subsurfaceInputs.glsl"

// ID 2 只写入局部 wrap/backscatter 与 thickness transmission 输入，不读取邻域或 profile LUT。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    MFSubsurfaceInput subsurfaceInput;
    subsurfaceInput.color = u_subsurfaceColorWeight.rgb;
    subsurfaceInput.weight = u_subsurfaceColorWeight.a;
    subsurfaceInput.wrapWidth = u_subsurfaceShape.x;
    subsurfaceInput.backscatterPower = u_subsurfaceShape.y;
    subsurfaceInput.backscatterWeight = u_subsurfaceShape.z;
    subsurfaceInput.thickness = u_subsurfaceShape.w;
    subsurfaceInput.transmissionWeight = u_subsurfaceTransmissionWeight;
    inputs.modelInputs.subsurface =
        EvaluateMFSubsurfaceInputs(subsurfaceInput);
    return inputs;
}

#endif
