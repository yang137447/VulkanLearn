#ifndef VL_MATERIAL_FUNCTION_SPEEDTREE_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_SPEEDTREE_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"

#ifndef MATERIAL_SHADING_MODEL
#define MATERIAL_SHADING_MODEL SHADING_MODEL_DEFAULT_LIT
#endif

// SpeedTree 的唯一片元材質入口。Base 與 ShadowDepth 模板消費同一份 opacity 語義。
MaterialSurface EvaluateMaterialSurface(in MaterialPixelContext pixel)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = pixel.worldPosition;
    surface.worldTangent = pixel.worldTangent;
    surface.shadingModel = MATERIAL_SHADING_MODEL;

    #if USE_ALBEDO_MAP
        vec4 albedoColor = texture(albedoMap, pixel.texCoord) * u_tintColor;
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    vec4 baseColor = albedoColor * vec4(pixel.vertexColor.rgb, 1.0);
    surface.baseColor = baseColor.rgb;
    surface.opacity = albedoColor.a;

    // pbrParamMap.r/g/b: roughness/metallic/ambient occlusion。
    #if USE_PBR_MAP
        vec4 pbrParam = texture(pbrParamMap, pixel.texCoord);
        surface.roughness = pbrParam.x;
        surface.metallic = pbrParam.y;
        surface.ambientOcclusion = pbrParam.z;
    #else
        surface.roughness = u_pbrFactors.x;
        surface.metallic = u_pbrFactors.y;
        surface.ambientOcclusion = pixel.vertexColor.a;
    #endif
    // 这里树的ao压在了vertexColor.a，所以这里直接用vertexColor.a即可。
    surface.ambientOcclusion = pixel.vertexColor.a;

    surface.worldNormal = normalize(pixel.worldNormal);
    #if USE_NORMAL_MAP
        vec4 normalSample = texture(normalMap, pixel.texCoord);
        vec3 normalTS = normalSample.xyz * 2.0 - 1.0;
        vec3 bitangent = cross(pixel.worldNormal, pixel.worldTangent.xyz) * pixel.worldTangent.w;
        surface.worldNormal = normalize(
            normalTS.x * pixel.worldTangent.xyz +
            normalTS.y * bitangent +
            normalTS.z * pixel.worldNormal);
        #if !USE_PBR_MAP
            surface.roughness = 1.0 - normalSample.a;
        #endif
    #endif

    return surface;
}

#endif
