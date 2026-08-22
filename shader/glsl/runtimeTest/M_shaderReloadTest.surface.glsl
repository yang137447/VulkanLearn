#ifndef VL_MATERIAL_FUNCTION_SHADER_RELOAD_TEST_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_SHADER_RELOAD_TEST_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"
#include "shaderReloadTestShared.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext pixel)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    
    vec3 primaryTexture =
        texture(u_reloadTexture, pixel.texCoord).rgb;
    vec3 alternateTexture =
        texture(u_reloadAlternateTexture, pixel.texCoord).rgb;
    inputs.baseColor =
        ShaderReloadTestColor(pixel) *
        mix(primaryTexture, alternateTexture, 0.05);
#if USE_RELOAD_REQUIRED_TEXTURE
    inputs.baseColor *=
        texture(u_zReloadRequiredTexture, pixel.texCoord).rgb;
#endif
    inputs.opacity = u_reloadTestColor.a;
    return inputs;
}

#endif
