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
    // 先保留阶段 6 的线性分发风格：当前支持 DefaultLit / Unlit，
    // 未来 shading model 变多后可以把这里升级成更完整的 dispatch 表。
    DeferredLightingResult defaultLit = ShadeDefaultLitDeferredSurfaceDetailed(surface, inputShadowMap);
    DeferredLightingResult unlit = ShadeUnlitDeferredSurfaceDetailed(surface);
    float unlitMask = ShadingModelMask(surface.shadingModel, SHADING_MODEL_UNLIT);

    DeferredLightingResult result = CreateDefaultDeferredLightingResult();
    result.directLighting = mix(defaultLit.directLighting, unlit.directLighting, unlitMask);
    result.shadow = mix(defaultLit.shadow, unlit.shadow, unlitMask);
    result.shadowCascadeIndex = mix(defaultLit.shadowCascadeIndex, unlit.shadowCascadeIndex, unlitMask);
    result.indirectDiffuse = mix(defaultLit.indirectDiffuse, unlit.indirectDiffuse, unlitMask);
    result.indirectSpecular = mix(defaultLit.indirectSpecular, unlit.indirectSpecular, unlitMask);
    result.indirectLighting = mix(defaultLit.indirectLighting, unlit.indirectLighting, unlitMask);
    result.finalColor = mix(defaultLit.finalColor, unlit.finalColor, unlitMask);
    return result;
}

vec3 ShadeDeferredSurface(in MaterialSurface surface, in sampler2DArrayShadow inputShadowMap)
{
    return ShadeDeferredSurfaceDetailed(surface, inputShadowMap).finalColor;
}

#endif
