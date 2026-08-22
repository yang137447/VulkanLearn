#ifndef VL_ENGINE_DEFERRED_LIGHTING_GLSL
#define VL_ENGINE_DEFERRED_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"
#include "../common/lighting.glsl"
#include "subsurfaceLighting.glsl"
#include "preintegratedSkinLighting.glsl"
#include "subsurfaceProfileLighting.glsl"

vec3 ReconstructWorldPositionFromSceneDepth(vec2 uv, float deviceDepth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipPosition = vec4(ndc, deviceDepth, 1.0);
    vec4 worldPosition = uboVP.invViewProjection * clipPosition;
    return worldPosition.xyz / worldPosition.w;
}

struct DeferredLightingResult
{
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 directLighting;
    float shadow;
    float shadowCascadeIndex;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectLighting;
    vec3 diffuseLighting;
    vec3 nonDiffuseLighting;
    vec3 transmissionLighting;
    vec3 localSubsurfaceLighting;
    vec3 defaultDiffuseLighting;
    float subsurfaceWeight;
    float transmissionWeight;
    vec3 finalColor;
};

DeferredLightingResult CreateDefaultDeferredLightingResult()
{
    DeferredLightingResult result;
    result.directDiffuse = vec3(0.0);
    result.directSpecular = vec3(0.0);
    result.directLighting = vec3(0.0);
    result.shadow = 1.0;
    result.shadowCascadeIndex = 0.0;
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.indirectLighting = vec3(0.0);
    result.diffuseLighting = vec3(0.0);
    result.nonDiffuseLighting = vec3(0.0);
    result.transmissionLighting = vec3(0.0);
    result.localSubsurfaceLighting = vec3(0.0);
    result.defaultDiffuseLighting = vec3(0.0);
    result.subsurfaceWeight = 0.0;
    result.transmissionWeight = 0.0;
    result.finalColor = vec3(0.0);
    return result;
}

void ResolveDeferredLightingComposition(
    in MaterialSurface surface,
    inout DeferredLightingResult result)
{
    result.directLighting =
        result.directDiffuse +
        result.directSpecular +
        result.transmissionLighting;
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.diffuseLighting =
        result.directDiffuse +
        result.indirectDiffuse * surface.ambientOcclusion;
    result.nonDiffuseLighting =
        surface.emissiveColor +
        result.directSpecular +
        result.indirectSpecular * surface.ambientOcclusion;
    result.finalColor =
        result.diffuseLighting +
        result.nonDiffuseLighting +
        result.transmissionLighting;
}

DeferredLightingResult ShadeDefaultLitDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    vec3 viewDir = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    LightingLobes directLobes = CalculateDirectLightingLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic);

    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadowCascadeIndex =
        ShadowCascadeDebugValue(cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.directDiffuse = directLobes.diffuse * result.shadow;
    result.directSpecular = directLobes.specular * result.shadow;

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
    ResolveDeferredLightingComposition(surface, result);
    result.defaultDiffuseLighting = result.diffuseLighting;
    return result;
}

DeferredLightingResult ShadeSubsurfaceDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result =
        ShadeDefaultLitDeferredSurfaceDetailed(
            surface,
            inputShadowMap);
    SubsurfaceLocalLighting localLighting =
        CalculateSubsurfaceLocalDirectLighting(surface);
    vec3 localDirectDiffuse =
        localLighting.diffuse * result.shadow;
    vec3 localIndirectDiffuse =
        CalculateSubsurfaceLocalIndirectDiffuse(surface);
    float weight = surface.modelInputs.subsurface.weight;
    float transmissionWeight =
        weight *
        surface.modelInputs.subsurface.transmissionWeight;
    // 先从 diffuse 反射能量中预留 transmission 份额，避免 profile/local response 重复计能。
    float reflectedFraction = 1.0 - transmissionWeight;

    result.localSubsurfaceLighting =
        localDirectDiffuse +
        localIndirectDiffuse * surface.ambientOcclusion;
    result.subsurfaceWeight = weight;
    result.transmissionWeight = transmissionWeight;
    result.directDiffuse = mix(
        result.directDiffuse,
        localDirectDiffuse,
        weight) * reflectedFraction;
    result.indirectDiffuse = mix(
        result.indirectDiffuse,
        localIndirectDiffuse,
        weight) * reflectedFraction;
    result.transmissionLighting =
        localLighting.transmission *
        result.shadow *
        transmissionWeight;
    ResolveDeferredLightingComposition(surface, result);
    return result;
}

DeferredLightingResult ShadePreintegratedSkinDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D skinLutTable)
{
    DeferredLightingResult result =
        ShadeDefaultLitDeferredSurfaceDetailed(
            surface,
            inputShadowMap);
    PreintegratedSkinLighting skinLighting =
        CalculatePreintegratedSkinDirectLighting(
            surface,
            skinLutTable);
    vec3 localDirectDiffuse =
        skinLighting.diffuse * result.shadow;
    vec3 localIndirectDiffuse =
        CalculatePreintegratedSkinIndirectDiffuse(
            surface,
            skinLutTable);
    float weight =
        surface.modelInputs.preintegratedSkin.weight;
    float transmissionWeight =
        weight *
        surface.modelInputs.preintegratedSkin.transmissionWeight;
    // transmission 与 LUT diffuse response 分路输出，最终 composition 只合成一次。
    float reflectedFraction = 1.0 - transmissionWeight;

    result.localSubsurfaceLighting =
        localDirectDiffuse +
        localIndirectDiffuse * surface.ambientOcclusion;
    result.subsurfaceWeight = weight;
    result.transmissionWeight = transmissionWeight;
    result.directDiffuse = mix(
        result.directDiffuse,
        localDirectDiffuse,
        weight) * reflectedFraction;
    result.indirectDiffuse = mix(
        result.indirectDiffuse,
        localIndirectDiffuse,
        weight) * reflectedFraction;
    result.transmissionLighting =
        skinLighting.transmission *
        result.shadow *
        transmissionWeight;
    ResolveDeferredLightingComposition(surface, result);
    return result;
}

DeferredLightingResult ShadeSubsurfaceProfileDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D profileTable)
{
    DeferredLightingResult result =
        ShadeDefaultLitDeferredSurfaceDetailed(
            surface,
            inputShadowMap);
    float weight =
        surface.modelInputs.subsurfaceProfile.weight;
    float transmissionWeight =
        weight *
        surface.modelInputs.subsurfaceProfile.transmissionWeight;
    // profile filter 只处理 diffuse；transmission 先扣除反射份额再独立输出。
    float reflectedFraction = 1.0 - transmissionWeight;
    result.directDiffuse *= reflectedFraction;
    result.indirectDiffuse *= reflectedFraction;
    result.subsurfaceWeight = weight;
    result.transmissionWeight = transmissionWeight;
    result.transmissionLighting =
        CalculateSubsurfaceProfileTransmission(
            surface,
            profileTable) *
        result.shadow *
        transmissionWeight;
    ResolveDeferredLightingComposition(surface, result);
    // ID 5 的空间 response 在后处理 profile filter 中产生；这里不把未过滤 diffuse
    // 冒充为 Debug View 16 的 local response，避免与 ID 2/3 的语义混淆。
    result.localSubsurfaceLighting = vec3(0.0);
    return result;
}

DeferredLightingResult ShadeClearCoatDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    vec3 viewDir = normalize(
        uboVP.cameraPosition - surface.worldPosition);
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
    result.shadowCascadeIndex =
        ShadowCascadeDebugValue(cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.directLighting *= result.shadow;

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
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.nonDiffuseLighting =
        surface.emissiveColor +
        result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
    result.finalColor = result.nonDiffuseLighting;
    return result;
}

DeferredLightingResult ShadeUnlitDeferredSurfaceDetailed(
    in MaterialSurface surface)
{
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    result.shadow = 1.0;
    result.nonDiffuseLighting =
        surface.baseColor + surface.emissiveColor;
    result.finalColor = result.nonDiffuseLighting;
    return result;
}

DeferredLightingResult ShadeDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D profileTable,
    in sampler2D skinLutTable)
{
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitDeferredSurfaceDetailed(surface);
        case SHADING_MODEL_SUBSURFACE:
            return ShadeSubsurfaceDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
        case SHADING_MODEL_PREINTEGRATED_SKIN:
            return ShadePreintegratedSkinDeferredSurfaceDetailed(
                surface,
                inputShadowMap,
                skinLutTable);
        case SHADING_MODEL_CLEAR_COAT:
            return ShadeClearCoatDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
        case SHADING_MODEL_SUBSURFACE_PROFILE:
            return ShadeSubsurfaceProfileDeferredSurfaceDetailed(
                surface,
                inputShadowMap,
                profileTable);
        default:
            return ShadeDefaultLitDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
    }
}

vec3 ShadeDeferredSurface(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D profileTable,
    in sampler2D skinLutTable)
{
    return ShadeDeferredSurfaceDetailed(
        surface,
        inputShadowMap,
        profileTable,
        skinLutTable).finalColor;
}

#endif
