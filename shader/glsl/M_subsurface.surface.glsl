#ifndef VL_M_SUBSURFACE_SURFACE_GLSL
#define VL_M_SUBSURFACE_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// ID 2 只写入局部 wrap/backscatter 与 thickness transmission 输入，不读取邻域或 profile LUT。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    inputs.modelInputs.subsurface.color =
        u_subsurfaceColorWeight.rgb;
    inputs.modelInputs.subsurface.weight =
        u_subsurfaceColorWeight.a;
    inputs.modelInputs.subsurface.wrapWidth =
        u_subsurfaceShape.x;
    inputs.modelInputs.subsurface.backscatterPower =
        u_subsurfaceShape.y;
    inputs.modelInputs.subsurface.backscatterWeight =
        u_subsurfaceShape.z;
    inputs.modelInputs.subsurface.thickness =
        u_subsurfaceShape.w;
    inputs.modelInputs.subsurface.transmissionWeight =
        u_subsurfaceTransmissionWeight;
    return inputs;
}

#endif
