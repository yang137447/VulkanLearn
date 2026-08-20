#ifndef VL_ENGINE_MATERIAL_SURFACE_GLSL
#define VL_ENGINE_MATERIAL_SURFACE_GLSL

#include "../common/shadingModel.glsl"

struct MaterialSurface
{
    vec3 worldPosition;
    // worldNormal 表示清漆顶层法线；底层法线用于底漆的漫反射和高光。
    vec3 worldNormal;
    vec3 clearCoatBottomNormal;
    vec3 baseColor;
    float opacity;
    vec3 emissiveColor;
    float roughness;
    float metallic;
    // Thin Translucent 的 Specular 只进入 UE Legacy 透射 Fresnel；表面反射使用 Default Lit F0。
    float specular;
    float ambientOcclusion;
    // 薄介质透射颜色和覆盖率分别控制透射吸收与 Add/Mul 对目标的作用范围。
    vec3 transmittanceColor;
    float surfaceCoverage;
    uint shadingModel;
    uint selectiveOutputMask;
    vec4 customData;
    vec4 precomputedShadowFactors;
    vec4 worldTangent;
    float anisotropy;
};

MaterialSurface CreateDefaultMaterialSurface()
{
    MaterialSurface surface;
    surface.worldPosition = vec3(0.0);
    surface.worldNormal = vec3(0.0, 0.0, 1.0);
    surface.clearCoatBottomNormal = vec3(0.0, 0.0, 1.0);
    surface.baseColor = vec3(1.0);
    surface.opacity = 1.0;
    surface.emissiveColor = vec3(0.0);
    surface.roughness = 1.0;
    surface.metallic = 0.0;
    surface.specular = 0.5;
    surface.ambientOcclusion = 1.0;
    surface.transmittanceColor = vec3(1.0);
    surface.surfaceCoverage = 1.0;
    surface.shadingModel = SHADING_MODEL_DEFAULT_LIT;
    surface.selectiveOutputMask = 0u;
    surface.customData = vec4(0.0);
    surface.precomputedShadowFactors = vec4(1.0);
    surface.worldTangent = vec4(1.0, 0.0, 0.0, 1.0);
    surface.anisotropy = 0.0;
    return surface;
}

#endif
