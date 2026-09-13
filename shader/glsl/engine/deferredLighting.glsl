#ifndef VL_ENGINE_DEFERRED_LIGHTING_GLSL
#define VL_ENGINE_DEFERRED_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
layout(set = 3, binding = 9) uniform sampler2DArrayShadow shadowMap;
// set 3 的 binding 10..13 原为 Hair / Eye / Cloth 的 LUT；这些模型的旧实现已于 2026-09-12
// 清理，采样器声明随之下线。重建某个模型时，它的 LUT 要连 renderGraphConfig 的 pass input
// 与 pass 材质 JSON 一起重新接回来（binding 顺序必须与 graph 的 input 顺序一致）。
#include "materialSurface.glsl"
#include "../common/lighting.glsl"

vec3 ReconstructWorldPositionFromSceneDepth(vec2 uv, float deviceDepth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipPosition = vec4(ndc, deviceDepth, 1.0);
    vec4 worldPosition = uboVP.invViewProjection * clipPosition;
    return worldPosition.xyz / worldPosition.w;
}

struct DeferredLightingResult
{
    // 2026-09-12 清理后只保留与模型无关的光照分解。原先为 Skin / Subsurface / Hair / Eye /
    // Cloth / TwoSidedFoliage 预留的字段（以及各自的 debug 注入）随实现一起删除；重建模型时
    // 由该模型的论文决定往这份快照里加什么，并同时接回自己的 debug setter。
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 directLighting;
    float shadow;
    float shadowCascadeIndex;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectLighting;
    // 三路分解保留：diffuse / nonDiffuse / transmission 是调试视图与后续模型合成的基础量
    // （旧 SSS 合成的输入即来自这里），任何模型重建时都应通过 ResolveDeferredLightingComposition
    // 把结果汇入 finalColor，而不要各自另开一份输出。
    vec3 diffuseLighting;
    vec3 nonDiffuseLighting;
    vec3 transmissionLighting;
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
        surface.metallic,
        0.5);

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
        surface.metallic,
        0.5);
    ResolveDeferredLightingComposition(surface, result);
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
    in sampler2DArrayShadow inputShadowMap)
{
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitDeferredSurfaceDetailed(surface);
        // 旧实现清理（2026-09-12）后只剩 DefaultLit 与 Unlit；Subsurface / PreintegratedSkin /
        // SubsurfaceProfile / ClearCoat / Hair / Eye / Cloth / TwoSidedFoliage 的 case 随各自实现
        // 删除，按论文重建时逐个接回（同时补 forward 路径的 case 与材质校验）。
        default:
            return ShadeDefaultLitDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
    }
}

vec3 ShadeDeferredSurface(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    return ShadeDeferredSurfaceDetailed(
        surface,
        inputShadowMap).finalColor;
}

#endif
