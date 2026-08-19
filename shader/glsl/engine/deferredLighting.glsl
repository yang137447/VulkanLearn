#ifndef VL_ENGINE_DEFERRED_LIGHTING_GLSL
#define VL_ENGINE_DEFERRED_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"
#include "../common/lighting.glsl"

vec3 ReconstructWorldPositionFromSceneDepth(vec2 uv, float deviceDepth)
{
    // Vulkan 的屏幕深度范围是 0..1。这里把 fullscreen uv 还原成 NDC xy，
    // 再配合 sceneDepth 中保存的 deviceDepth，通过 invViewProjection 回到世界空间。
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipPosition = vec4(ndc, deviceDepth, 1.0);
    vec4 worldPosition = uboVP.invViewProjection * clipPosition;
    return worldPosition.xyz / worldPosition.w;
}

struct DeferredLightingResult
{
    vec3 directLighting;
    float shadow;
    float shadowCascadeIndex;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectLighting;
    vec3 finalColor;
};

DeferredLightingResult CreateDefaultDeferredLightingResult()
{
    DeferredLightingResult result;
    result.directLighting = vec3(0.0);
    result.shadow = 1.0;
    result.shadowCascadeIndex = 0.0;
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.indirectLighting = vec3(0.0);
    result.finalColor = vec3(0.0);
    return result;
}

DeferredLightingResult ShadeDefaultLitDeferredSurfaceDetailed(in MaterialSurface surface, in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result = CreateDefaultDeferredLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);

    result.directLighting = CalculateDirectLighting(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic);

    // 阴影贴图是当前 deferredLighting pass 的输入，所以 sampler 由 pass shader 传入。
    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    // GBufferE.r 先作为学习版的预计算直接光可见性项：1 = 不遮蔽，0 = 完全遮蔽。
    // 后续接 lightmap / stationary light 时，可以继续细分 rgba 的具体光源语义。
    result.shadow *= surface.precomputedShadowFactors.r;
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

    // AO 主要遮蔽间接光，不乘到直接光上；这和阶段 6 的约定保持一致。
    result.finalColor = surface.emissiveColor + result.directLighting + result.indirectLighting * surface.ambientOcclusion;
    return result;
}

DeferredLightingResult ShadeClearCoatDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result = CreateDefaultDeferredLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);

    // GBuffer 解码后恢复出顶层/底层两条法线；customData.xy 继续作为
    // 清漆权重和清漆粗糙度，确保 deferred 与 forward 走同一套 BRDF。
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

    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.directLighting *= result.shadow;

    // AO 仍只作用于最终间接光；清漆层内部的 Fresnel 与介质透射已在 IBL 函数中处理。
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

vec3 ShadeDefaultLitDeferredSurface(in MaterialSurface surface, in sampler2DArrayShadow inputShadowMap)
{
    return ShadeDefaultLitDeferredSurfaceDetailed(surface, inputShadowMap).finalColor;
}

DeferredLightingResult ShadeUnlitDeferredSurfaceDetailed(in MaterialSurface surface)
{
    DeferredLightingResult result = CreateDefaultDeferredLightingResult();
    result.shadow = 1.0;
    result.finalColor = surface.baseColor + surface.emissiveColor;
    return result;
}

vec3 ShadeUnlitDeferredSurface(in MaterialSurface surface)
{
    return ShadeUnlitDeferredSurfaceDetailed(surface).finalColor;
}

DeferredLightingResult ShadeDeferredSurfaceDetailed(in MaterialSurface surface, in sampler2DArrayShadow inputShadowMap)
{
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitDeferredSurfaceDetailed(surface);
        case SHADING_MODEL_CLEAR_COAT:
            return ShadeClearCoatDeferredSurfaceDetailed(surface, inputShadowMap);
        default:
            return ShadeDefaultLitDeferredSurfaceDetailed(surface, inputShadowMap);
    }
}

vec3 ShadeDeferredSurface(in MaterialSurface surface, in sampler2DArrayShadow inputShadowMap)
{
    return ShadeDeferredSurfaceDetailed(surface, inputShadowMap).finalColor;
}

#endif
