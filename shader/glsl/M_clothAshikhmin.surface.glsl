#ifndef VL_M_CLOTH_ASHIKHMIN_SURFACE_GLSL
#define VL_M_CLOTH_ASHIKHMIN_SURFACE_GLSL

#include "materialFunction/mf_pbrInputs.glsl"

// Shared surface entry for the three Ashikhmin/Premoze/Shirley 2000 cloth
// materials.  The closure is selected by VL_CLOTH_CLOSURE_* in the M_ macro
// set; the shader-side evaluator lives in common/ashikhminCloth.glsl.
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    // u_baseColor is a final tint for untextured paper probes; textured
    // garments leave it at white and use u_tintColor/baseColorMap instead.
    inputs.baseColor *= u_baseColor.rgb;
    inputs.opacity *= u_baseColor.a;
    inputs.opacityMask *= u_baseColor.a;
#if USE_ALPHA_MAP
    // Authoring may keep coverage in a separate map; match M_pbr's rule so a
    // garment never samples alpha from two different slots.
    float alpha = texture(alphaMap, context.texCoord).r * u_tintColor.a;
    inputs.opacity = alpha;
    inputs.opacityMask = alpha;
#endif
    inputs.metallic = 0.0;
    // The Ashikhmin evaluator owns the specular lobe.  Keep the generic
    // DefaultLit fields neutral so a wrong pass cannot silently fall back to a
    // DefaultLit-looking response.
    inputs.specular = 0.0;

    inputs.modelInputs.cloth.sheenColor = u_clothSheenColor.rgb;
    inputs.modelInputs.cloth.sheenRoughness = u_clothSheenRoughness;
    inputs.modelInputs.cloth.f0 = u_f0;
    inputs.modelInputs.cloth.fiberRotationDegrees = u_fiberRotationDegrees;
    inputs.modelInputs.cloth.anisotropy = u_clothAnisotropy;
    inputs.modelInputs.cloth.anisotropyCross = u_clothAnisotropyCross;
    return inputs;
}

#endif
