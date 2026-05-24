#ifndef VL_ENGINE_FORWARD_LIGHTING_GLSL
#define VL_ENGINE_FORWARD_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"
#include "../common/lighting.glsl"

struct ForwardLightingResult
{
    vec3 directLighting;
    float shadow;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectLighting;
    vec3 finalColor;
};

ForwardLightingResult CreateDefaultForwardLightingResult()
{
    ForwardLightingResult result;
    result.directLighting = vec3(0.0);
    result.shadow = 1.0;
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.indirectLighting = vec3(0.0);
    result.finalColor = vec3(0.0);
    return result;
}

ForwardLightingResult ShadeDefaultLitForwardSurface(in MaterialSurface surface)
{
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);

    result.directLighting = CalculateDirectLighting(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic);
    result.shadow = CalculateShadow(uboVP.lightViewProj, surface.worldPosition, 0.002);
    result.directLighting *= result.shadow;

    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic);
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic);
    result.indirectLighting = result.indirectDiffuse + result.indirectSpecular;
    result.finalColor = surface.emissiveColor + result.directLighting + result.indirectLighting * surface.ambientOcclusion;
    return result;
}

ForwardLightingResult ShadeUnlitForwardSurface(in MaterialSurface surface)
{
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    result.finalColor = surface.baseColor + surface.emissiveColor;
    return result;
}

ForwardLightingResult ShadeForwardSurfaceDetailed(in MaterialSurface surface)
{
    if (surface.shadingModel == SHADING_MODEL_UNLIT)
    {
        return ShadeUnlitForwardSurface(surface);
    }
    return ShadeDefaultLitForwardSurface(surface);
}

vec3 ShadeForwardSurface(in MaterialSurface surface)
{
    return ShadeForwardSurfaceDetailed(surface).finalColor;
}

#endif
