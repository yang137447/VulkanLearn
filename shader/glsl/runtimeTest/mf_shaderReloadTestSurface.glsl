#ifndef VL_MATERIAL_FUNCTION_SHADER_RELOAD_TEST_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_SHADER_RELOAD_TEST_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"
#include "shaderReloadTestShared.glsl"

MaterialSurface EvaluateMaterialSurface(in MaterialPixelContext pixel)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.shadingModel = SHADING_MODEL_UNLIT;
    vec3 primaryTexture =
        texture(u_reloadTexture, pixel.texCoord).rgb;
    vec3 alternateTexture =
        texture(u_reloadAlternateTexture, pixel.texCoord).rgb;
    surface.baseColor =
        ShaderReloadTestColor(pixel) *
        mix(primaryTexture, alternateTexture, 0.05);
#if USE_RELOAD_REQUIRED_TEXTURE
    surface.baseColor *=
        texture(u_zReloadRequiredTexture, pixel.texCoord).rgb;
#endif
    surface.opacity = u_reloadTestColor.a;
    return surface;
}

#endif
