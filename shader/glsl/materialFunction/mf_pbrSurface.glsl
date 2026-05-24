#ifndef VL_MATERIAL_FUNCTION_PBR_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_PBR_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"

#ifndef MATERIAL_SHADING_MODEL
#define MATERIAL_SHADING_MODEL SHADING_MODEL_DEFAULT_LIT
#endif

MaterialSurface EvaluatePbrSurface(in MaterialPixelContext pixel)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = pixel.worldPosition;
    surface.shadingModel = MATERIAL_SHADING_MODEL;

    // 第一版 PBR 先复用一张 albedo 贴图，baseColor = 贴图颜色 * 顶点阶段传来的 tint。
    #if USE_ALBEDO_MAP
        vec4 albedoColor = texture(albedoMap, pixel.texCoord);
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    vec4 baseColor = albedoColor * vec4(pixel.vertexColor, 1.0);
    surface.baseColor = baseColor.rgb;
    surface.opacity = baseColor.a;

    #if defined(RENDER_MODE_OPAQUE_CLIP)
    // u_pbrFactors.w 预留作为 alphaClipThreshold 使用。
    if (surface.opacity < u_pbrFactors.w)
    {
        discard;
    }
    #endif

    // u_pbrFactors: x=roughness, y=metallic, z=ao, w=预留。
    // 粗糙度、金属度、AO 确保在上游 Material Instance 配置正确，此处直接使用。
    #if USE_PBR_MAP
        vec4 pbrParam = texture(pbrParamMap, pixel.texCoord);
        surface.roughness = pbrParam.x;
        surface.metallic = pbrParam.y;
        surface.ambientOcclusion = pbrParam.z;
    #else
        surface.roughness = u_pbrFactors.x;
        surface.metallic = u_pbrFactors.y;
        surface.ambientOcclusion = u_pbrFactors.z;
    #endif

    // 自发光强度由材质参数控制。
    #if USE_EMISSION_MAP
        surface.emissiveColor = texture(emissionMap, pixel.texCoord).rgb * u_emissiveStrength;
    #endif

    surface.worldNormal = normalize(pixel.worldNormal);
    #if USE_NORMAL_MAP
        vec3 normalTS = texture(normalMap, pixel.texCoord).xyz * 2.0 - 1.0;
        vec3 bitangent = cross(pixel.worldNormal, pixel.worldTangent.xyz) * pixel.worldTangent.w;
        // 这里沿用顶点阶段保留下来的未归一化 T/B/N，避免与烘焙端切线空间产生额外偏差。
        surface.worldNormal = normalize(
            normalTS.x * pixel.worldTangent.xyz +
            normalTS.y * bitangent +
            normalTS.z * pixel.worldNormal);
    #endif

    return surface;
}

#endif
