#ifndef VL_M_SUBSURFACE_PROFILE_SURFACE_GLSL
#define VL_M_SUBSURFACE_PROFILE_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// ID 5 只保存 profile identity、权重、厚度和 transmission；邻域扩散留给独立 post-process pass。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    inputs.modelInputs.subsurfaceProfile.profileId =
        u_subsurfaceProfileId;
    inputs.modelInputs.subsurfaceProfile.weight =
        u_subsurfaceProfileSurface.x;
    inputs.modelInputs.subsurfaceProfile.thickness =
        u_subsurfaceProfileSurface.y;
    inputs.modelInputs.subsurfaceProfile.transmissionWeight =
        u_subsurfaceProfileSurface.z;
    return inputs;
}

#endif
