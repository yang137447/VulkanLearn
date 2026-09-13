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
    // 2026-09-12 清理后只保留与模型无关的光照分解；Hair / Eye / Cloth / TwoSidedFoliage 的
    // 逐模型 ledger 字段随各自实现删除。重建模型时由该模型的论文决定往这份快照里加什么，
    // 并同时接回 `materialForwardOutput.glsl` 里对应的 debug setter。
    vec3 directLighting;
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 transmissionLighting;
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
    result.transmissionLighting = vec3(0.0);
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
        surface.metallic,
        0.5);
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
        surface.metallic,
        0.5);
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
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitForwardSurface(surface);
        // 旧实现清理（2026-09-12）后只剩 DefaultLit 与 Unlit；ClearCoat / ThinTranslucent /
        // Hair / Cloth / TwoSidedFoliage / Eye 的 case 随各自实现删除，按论文重建时逐个接回。
        // 未实现的 ShadingModelID 会落到 default（= DefaultLit），这正是 A-07 记录的风险，
        // 重建模型时必须同时补 case 与校验。
        default:
            return ShadeDefaultLitForwardSurface(surface);
    }
}

vec3 ShadeForwardSurface(in MaterialSurface surface)
{
    return ShadeForwardSurfaceDetailed(surface).finalColor;
}

#endif
