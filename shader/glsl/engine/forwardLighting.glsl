#ifndef VL_ENGINE_FORWARD_LIGHTING_GLSL
#define VL_ENGINE_FORWARD_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"
#include "../common/lighting.glsl"

// 前向 pass 如果需要阴影，由具体 pass shader 在 include 前定义
// VL_FORWARD_DECLARE_SHADOWMAP_INPUT。默认 include 不占用 Set 3，只有真正带阴影的
// forward pass 才声明 shadowMap 输入，避免材质布局被无关 pass 污染。
#if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
layout (set = 3, binding = 0) uniform sampler2DArrayShadow shadowMap;
#endif

struct ForwardLightingResult
{
    vec3 directLighting;
    vec3 directDiffuse;
    vec3 directSpecular;
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
    result.directDiffuse = vec3(0.0);
    result.directSpecular = vec3(0.0);
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
        result.shadow = CalculateCsmShadow(
            shadowMap,
            surface.worldPosition,
            surface.worldNormal,
            cascadeIndex);
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

ForwardLightingResult ShadeThinTranslucentForwardSurface(
    in MaterialSurface surface)
{
    // Thin Translucent 复用 Default Lit 的表面反射，但必须保留 diffuse/specular 分量，
    // 因为 RootOpacity 只覆盖 diffuse 与 emissive，表面高光不随根透明度衰减。
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    vec3 viewDir = normalize(
        uboVP.cameraPosition - surface.worldPosition);

    LightingLobes directLobes = CalculateDirectLightingLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic);
    #if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
        int cascadeIndex = 0;
        result.shadow = CalculateCsmShadow(
            shadowMap,
            surface.worldPosition,
            surface.worldNormal,
            cascadeIndex);
        result.shadowCascadeIndex =
            ShadowCascadeDebugValue(cascadeIndex);
    #else
        result.shadow = 1.0;
    #endif
    result.directDiffuse = directLobes.diffuse * result.shadow;
    result.directSpecular = directLobes.specular * result.shadow;
    result.directLighting =
        result.directDiffuse + result.directSpecular;

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
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.finalColor =
        surface.emissiveColor +
        result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
    return result;
}

ForwardLightingResult ShadeClearCoatForwardSurface(in MaterialSurface surface)
{
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);

    // 顶层法线负责清漆高光，底层法线负责底漆响应；customData.xy 分别是
    // 清漆权重和清漆粗糙度，与 deferred 路径保持同一输入合同。
    result.directLighting = CalculateClearCoatDirectLighting(
        surface.worldNormal,
        surface.clearCoatBottomNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.customData.x,
        surface.customData.y);
    #if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
        int cascadeIndex = 0;
        result.shadow = CalculateCsmShadow(
            shadowMap,
            surface.worldPosition,
            surface.worldNormal,
            cascadeIndex);
        result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    #else
        result.shadow = 1.0;
    #endif
    result.directLighting *= result.shadow;

    // Legacy Clear Coat 的间接光也按两层拆分：底层使用底层法线，
    // 顶层环境高光使用顶层法线，最后按 Fresnel 透射关系合成。
    result.indirectDiffuse = CalculateClearCoatDiffuseIbl(
        surface.worldNormal,
        surface.clearCoatBottomNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.customData.x);
    result.indirectSpecular = CalculateClearCoatSpecularIbl(
        surface.worldNormal,
        surface.clearCoatBottomNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.customData.x,
        surface.customData.y);
    result.indirectLighting = result.indirectDiffuse + result.indirectSpecular;
    result.finalColor =
        surface.emissiveColor +
        result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
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
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitForwardSurface(surface);
        case SHADING_MODEL_CLEAR_COAT:
            return ShadeClearCoatForwardSurface(surface);
        case SHADING_MODEL_THIN_TRANSLUCENT:
            return ShadeThinTranslucentForwardSurface(surface);
        default:
            return ShadeDefaultLitForwardSurface(surface);
    }
}

vec3 ShadeForwardSurface(in MaterialSurface surface)
{
    return ShadeForwardSurfaceDetailed(surface).finalColor;
}

#endif
