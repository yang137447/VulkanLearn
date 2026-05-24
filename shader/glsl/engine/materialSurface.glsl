#ifndef VL_ENGINE_MATERIAL_SURFACE_GLSL
#define VL_ENGINE_MATERIAL_SURFACE_GLSL

#include "../common/shadingModel.glsl"

struct MaterialSurface
{
    vec3 worldPosition;
    vec3 worldNormal;
    vec3 baseColor;
    float opacity;
    vec3 emissiveColor;
    float roughness;
    float metallic;
    float ambientOcclusion;
    uint shadingModel;
    vec4 customData;
};

MaterialSurface CreateDefaultMaterialSurface()
{
    MaterialSurface surface;
    surface.worldPosition = vec3(0.0);
    surface.worldNormal = vec3(0.0, 0.0, 1.0);
    surface.baseColor = vec3(1.0);
    surface.opacity = 1.0;
    surface.emissiveColor = vec3(0.0);
    surface.roughness = 1.0;
    surface.metallic = 0.0;
    surface.ambientOcclusion = 1.0;
    surface.shadingModel = SHADING_MODEL_DEFAULT_LIT;
    surface.customData = vec4(0.0);
    return surface;
}

#endif
