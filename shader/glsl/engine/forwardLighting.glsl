#ifndef VL_ENGINE_FORWARD_LIGHTING_GLSL
#define VL_ENGINE_FORWARD_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"
#include "../common/lighting.glsl"

// Forward pass 如果需要阴影，由具体 pass shader 在 include 前定义 VL_FORWARD_DECLARE_SHADOWMAP_INPUT。
// 这样默认 include 本文件不会占用 Set 3；只有真正的 forward-with-shadow pass 才声明 shadowMap 输入。
#if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
layout (set = 3, binding = 0) uniform sampler2DArrayShadow shadowMap;
#endif

struct ForwardLightingResult
{
    vec3 directLighting;
    float shadow;
    float shadowCascadeIndex;
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
    result.shadowCascadeIndex = 0.0;
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
    #if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
        int cascadeIndex = 0;
        result.shadow = CalculateCsmShadow(shadowMap, surface.worldPosition, cascadeIndex);
        result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    #else
        result.shadow = 1.0;
    #endif
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
    // 与 deferred lighting 保持同一套分发风格：先算当前支持的 shading model，
    // 再用 mask 选择结果，避免在材质 shader 里散落动态分支。
    ForwardLightingResult defaultLit = ShadeDefaultLitForwardSurface(surface);
    ForwardLightingResult unlit = ShadeUnlitForwardSurface(surface);
    float unlitMask = ShadingModelMask(surface.shadingModel, SHADING_MODEL_UNLIT);

    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    result.directLighting = mix(defaultLit.directLighting, unlit.directLighting, unlitMask);
    result.shadow = mix(defaultLit.shadow, unlit.shadow, unlitMask);
    result.shadowCascadeIndex = mix(defaultLit.shadowCascadeIndex, unlit.shadowCascadeIndex, unlitMask);
    result.indirectDiffuse = mix(defaultLit.indirectDiffuse, unlit.indirectDiffuse, unlitMask);
    result.indirectSpecular = mix(defaultLit.indirectSpecular, unlit.indirectSpecular, unlitMask);
    result.indirectLighting = mix(defaultLit.indirectLighting, unlit.indirectLighting, unlitMask);
    result.finalColor = mix(defaultLit.finalColor, unlit.finalColor, unlitMask);
    return result;
}

vec3 ShadeForwardSurface(in MaterialSurface surface)
{
    return ShadeForwardSurfaceDetailed(surface).finalColor;
}

#endif
