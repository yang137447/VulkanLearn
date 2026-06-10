#ifndef VL_ENGINE_MATERIAL_DEBUG_VIEW_GLSL
#define VL_ENGINE_MATERIAL_DEBUG_VIEW_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"

struct MaterialDebugLightingData
{
    float shadow;
    float shadowCascadeIndex;
    vec3 directLighting;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
};

MaterialDebugLightingData CreateMaterialDebugLightingData(
    float shadow,
    float shadowCascadeIndex,
    vec3 directLighting,
    vec3 indirectDiffuse,
    vec3 indirectSpecular)
{
    MaterialDebugLightingData data;
    data.shadow = shadow;
    data.shadowCascadeIndex = shadowCascadeIndex;
    data.directLighting = directLighting;
    data.indirectDiffuse = indirectDiffuse;
    data.indirectSpecular = indirectSpecular;
    return data;
}

float MaterialDebugViewModeMask(int mode)
{
    return 1.0 - min(abs(float(uboVP.debugViewMode - mode)), 1.0);
}

// Debug view 只依赖材质表面和一份中性的 lighting 调试数据。
// Forward / Deferred 可以分别把自己的 lighting result 转成 MaterialDebugLightingData，
// 这样 debug view 不需要 include forwardLighting 或 deferredLighting，避免路径互相缠住。
vec4 ResolveMaterialDebugView(
    in MaterialSurface surface,
    in MaterialDebugLightingData lighting,
    in vec4 defaultColor)
{
#if defined(ENABLE_DEBUG_VIEW)
    float baseColorMask = MaterialDebugViewModeMask(1);
    float emissiveMask = MaterialDebugViewModeMask(2);
    float normalMask = MaterialDebugViewModeMask(3);
    float roughnessMask = MaterialDebugViewModeMask(4);
    float metallicMask = MaterialDebugViewModeMask(5);
    float aoMask = MaterialDebugViewModeMask(6);
    float shadowMask = MaterialDebugViewModeMask(7);
    float directLightingMask = MaterialDebugViewModeMask(8);
    float indirectDiffuseMask = MaterialDebugViewModeMask(9);
    float indirectSpecularMask = MaterialDebugViewModeMask(10);
    float shadowCascadeMask = MaterialDebugViewModeMask(11);

    float debugMask = min(
        baseColorMask +
        emissiveMask +
        normalMask +
        roughnessMask +
        metallicMask +
        aoMask +
        shadowMask +
        directLightingMask +
        indirectDiffuseMask +
        indirectSpecularMask +
        shadowCascadeMask,
        1.0);

    vec4 debugColor =
        baseColorMask * vec4(surface.baseColor, 1.0) +
        emissiveMask * vec4(surface.emissiveColor, 1.0) +
        normalMask * vec4(surface.worldNormal * 0.5 + 0.5, 1.0) +
        roughnessMask * vec4(vec3(surface.roughness), 1.0) +
        metallicMask * vec4(vec3(surface.metallic), 1.0) +
        aoMask * vec4(vec3(surface.ambientOcclusion), 1.0) +
        shadowMask * vec4(vec3(lighting.shadow), 1.0) +
        directLightingMask * vec4(lighting.directLighting, 1.0) +
        indirectDiffuseMask * vec4(lighting.indirectDiffuse, 1.0) +
        indirectSpecularMask * vec4(lighting.indirectSpecular, 1.0) +
        shadowCascadeMask * vec4(vec3(lighting.shadowCascadeIndex), 1.0);

    return mix(defaultColor, debugColor, debugMask);
#else
    return defaultColor;
#endif
}

#endif
