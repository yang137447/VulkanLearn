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
#if USE_SUBSURFACE_PROFILE_WEIGHT_MAP
    inputs.modelInputs.subsurfaceProfile.weight *=
        texture(subsurfaceProfileWeightMap, context.texCoord).r;
#endif
#if USE_SUBSURFACE_PROFILE_THICKNESS_MAP
    inputs.modelInputs.subsurfaceProfile.thickness *=
        texture(subsurfaceProfileThicknessMap, context.texCoord).r;
#endif
#if USE_SUBSURFACE_PROFILE_TRANSMISSION_MAP
    inputs.modelInputs.subsurfaceProfile.transmissionWeight *=
        texture(subsurfaceProfileTransmissionMap, context.texCoord).r;
#endif
    return inputs;
}

#endif
