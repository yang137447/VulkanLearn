#ifndef VL_MATERIAL_FUNCTION_CLOTH_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_CLOTH_INPUTS_GLSL

#include "../engine/materialInputs.glsl"

struct MFClothInputs
{
    vec3 sheenColor;
    float sheenRoughness;
    float anisotropy;
    float anisotropyCross;
};

MFClothInputs EvaluateMFClothInputs(
    vec3 sheenColor,
    float sheenRoughness,
    float anisotropy,
    float anisotropyCross)
{
    MFClothInputs cloth;
    cloth.sheenColor = sheenColor;
    cloth.sheenRoughness = sheenRoughness;
    cloth.anisotropy = anisotropy;
    cloth.anisotropyCross = anisotropyCross;
    return cloth;
}

MaterialInputs ApplyMFClothInputs(
    in MaterialInputs inputs,
    in MFClothInputs cloth)
{
    MaterialInputs result = inputs;
    result.modelInputs.cloth.sheenColor = cloth.sheenColor;
    result.modelInputs.cloth.sheenRoughness = cloth.sheenRoughness;
    result.modelInputs.cloth.anisotropy = cloth.anisotropy;
    result.modelInputs.cloth.anisotropyCross = cloth.anisotropyCross;
    result.metallic = 0.0;
    return result;
}

#endif
