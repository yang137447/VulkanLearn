#ifndef VL_MATERIAL_FUNCTION_PBR_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_PBR_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"

#ifndef MATERIAL_SHADING_MODEL
#define MATERIAL_SHADING_MODEL SHADING_MODEL_DEFAULT_LIT
#endif

vec3 TransformMaterialNormalToWorld(
    in MaterialPixelContext pixel,
    in vec3 normal_TS)
{
    vec3 bitangent =
        cross(pixel.worldNormal, pixel.worldTangent.xyz) *
        pixel.worldTangent.w;
    return normalize(
        normal_TS.x * pixel.worldTangent.xyz +
        normal_TS.y * bitangent +
        normal_TS.z * pixel.worldNormal);
}

// PBR 唯一片元材質入口。它只描述 Surface 語義，Alpha Clip 和輸出由 Pass 模板處理。
MaterialSurface EvaluateMaterialSurface(in MaterialPixelContext pixel)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = pixel.worldPosition;
    surface.worldTangent = pixel.worldTangent;
    surface.shadingModel = MATERIAL_SHADING_MODEL;

    // 第一版 PBR 先复用一张 albedo 贴图，baseColor = 贴图颜色 * 材质 tint * 顶点色 RGB。
    #if USE_ALBEDO_MAP
        vec4 albedoColor = texture(albedoMap, pixel.texCoord) * u_tintColor;
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    vec4 baseColor = albedoColor * vec4(pixel.vertexColor.rgb, 1.0);
    surface.baseColor = baseColor.rgb;
    surface.opacity = baseColor.a;

    // u_pbrFactors: x=roughness, y=metallic, z=ao, w=reserved.
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

    #if defined(VL_PBR_USE_CLEAR_COAT_INPUTS)
        // customData.xy 对齐 UE Legacy Clear Coat：x=清漆权重，y=清漆粗糙度。
        // 启用贴图后 RG 直接替代常量输入，不再与常量相乘。
        vec2 clearCoatInputs = vec2(
            u_clearCoat,
            u_clearCoatRoughness);
        #if USE_CLEAR_COAT_MAP
            clearCoatInputs = texture(
                clearCoatMap,
                pixel.texCoord).rg;
        #endif
        surface.customData.xy = clearCoatInputs;
        surface.selectiveOutputMask |= GBUFFER_HAS_CUSTOM_DATA_MASK;
    #endif

    surface.worldNormal = normalize(pixel.worldNormal);
    #if USE_NORMAL_MAP
        vec3 normalTS = texture(normalMap, pixel.texCoord).xyz * 2.0 - 1.0;
        // 这里沿用顶点阶段保留下来的未归一化 T/B/N，避免与烘焙端切线空间产生额外偏差。
        surface.worldNormal = TransformMaterialNormalToWorld(pixel, normalTS);
    #endif
    // 未提供独立底层法线时，两层共用同一法线，模型自然退化为光滑清漆覆盖。
    surface.clearCoatBottomNormal = surface.worldNormal;
    #if USE_CLEAR_COAT_BOTTOM_NORMAL_MAP
        #if defined(VL_PBR_USE_CLEAR_COAT_BOTTOM_NORMAL_CONTROLS)
            vec2 bottomNormalTexCoord =
                pixel.texCoord * u_clearCoatBottomNormalTiling;
        #else
            vec2 bottomNormalTexCoord = pixel.texCoord;
        #endif
        vec3 bottomNormalTS =
            texture(
                clearCoatBottomNormalMap,
                bottomNormalTexCoord).xyz * 2.0 - 1.0;
        #if defined(VL_PBR_USE_CLEAR_COAT_BOTTOM_NORMAL_CONTROLS)
            // 对齐 Blender Normal Map Strength：先在切线空间把细节法线向中性法线
            // (0, 0, 1) 混合，再转换到世界空间。
            bottomNormalTS = normalize(mix(
                vec3(0.0, 0.0, 1.0),
                bottomNormalTS,
                u_clearCoatBottomNormalStrength));
        #endif
        surface.clearCoatBottomNormal =
            TransformMaterialNormalToWorld(pixel, bottomNormalTS);
    #endif
    #if MATERIAL_TWO_SIDED
        // 双面材质翻面时两层法线必须同步翻转，否则顶层与底层会落在不同半球。
        surface.worldNormal = gl_FrontFacing ? surface.worldNormal : -surface.worldNormal;
        surface.clearCoatBottomNormal =
            gl_FrontFacing
                ? surface.clearCoatBottomNormal
                : -surface.clearCoatBottomNormal;
    #endif

    return surface;
}

#endif
