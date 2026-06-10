#ifndef VL_ENGINE_MATERIAL_FORWARD_OUTPUT_GLSL
#define VL_ENGINE_MATERIAL_FORWARD_OUTPUT_GLSL

#include "materialSurface.glsl"
#include "forwardLighting.glsl"
#include "materialDebugView.glsl"

vec4 BuildMaterialForwardOutput(in MaterialSurface surface)
{
    ForwardLightingResult lighting = ShadeForwardSurfaceDetailed(surface);
    vec4 color = vec4(lighting.finalColor, surface.opacity);
    MaterialDebugLightingData debugLighting = CreateMaterialDebugLightingData(
        lighting.shadow,
        lighting.shadowCascadeIndex,
        lighting.directLighting,
        lighting.indirectDiffuse,
        lighting.indirectSpecular);
    return ResolveMaterialDebugView(surface, debugLighting, color);
}

#endif
