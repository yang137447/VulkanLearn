#ifndef VL_MATERIAL_FUNCTION_THIN_TRANSLUCENT_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_THIN_TRANSLUCENT_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"

#ifndef MATERIAL_SHADING_MODEL
#define MATERIAL_SHADING_MODEL SHADING_MODEL_THIN_TRANSLUCENT
#endif

vec3 TransformThinTranslucentNormalToWorld(
    in MaterialPixelContext pixel,
    in vec3 normal_TS)
{
    // 法线贴图仍沿用切线空间输入；这里单独保留函数名，便于明确薄透射的法线同时
    // 参与表面反射、透射 Fresnel 和 NoV 吸收路径长度计算。
    vec3 bitangent =
        cross(pixel.worldNormal, pixel.worldTangent.xyz) *
        pixel.worldTangent.w;
    return normalize(
        normal_TS.x * pixel.worldTangent.xyz +
        normal_TS.y * bitangent +
        normal_TS.z * pixel.worldNormal);
}

MaterialSurface EvaluateMaterialSurface(in MaterialPixelContext pixel)
{
    // Thin Translucent 的 Surface 同时携带 Default Lit 反射参数和 UE Legacy 透射参数。
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = pixel.worldPosition;
    surface.worldTangent = pixel.worldTangent;
    surface.shadingModel = MATERIAL_SHADING_MODEL;

    vec3 baseColor = u_baseColorOpacity.rgb;
    #if USE_BASE_COLOR_MAP
        baseColor *= texture(baseColorMap, pixel.texCoord).rgb;
    #endif
    surface.baseColor = baseColor * pixel.vertexColor.rgb;
    surface.opacity = u_baseColorOpacity.a;

    surface.transmittanceColor =
        u_transmittanceColorCoverage.rgb;
    #if USE_TRANSMITTANCE_MAP
        surface.transmittanceColor *=
            texture(transmittanceMap, pixel.texCoord).rgb;
    #endif
    surface.surfaceCoverage =
        u_transmittanceColorCoverage.a;

    // u_surfaceFactors：x=roughness，y=metallic，z=specular，w=AO。
    surface.roughness = u_surfaceFactors.x;
    surface.metallic = u_surfaceFactors.y;
    surface.specular = u_surfaceFactors.z;
    surface.ambientOcclusion = u_surfaceFactors.w;

    surface.emissiveColor =
        u_emissiveColorStrength.rgb *
        u_emissiveColorStrength.a;
    #if USE_EMISSION_MAP
        surface.emissiveColor *=
            texture(emissionMap, pixel.texCoord).rgb;
    #endif

    surface.worldNormal = normalize(pixel.worldNormal);
    #if USE_NORMAL_MAP
        vec3 normalTS =
            texture(normalMap, pixel.texCoord).xyz * 2.0 - 1.0;
        surface.worldNormal =
            TransformThinTranslucentNormalToWorld(pixel, normalTS);
    #endif
    surface.clearCoatBottomNormal = surface.worldNormal;

    #if MATERIAL_TWO_SIDED
        // 双面材质翻转背面法线，使表面反射与透射都使用当前可见界面的方向。
        surface.worldNormal = gl_FrontFacing
            ? surface.worldNormal
            : -surface.worldNormal;
        surface.clearCoatBottomNormal = surface.worldNormal;
    #endif

    return surface;
}

#endif
