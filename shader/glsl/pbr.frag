#version 450

#include "common/commonUbo.glsl"
#include "common/lighting.glsl"

layout(set = 1, binding = 0) uniform UBOMIParamters{
    vec4 u_tintColor;
    vec4 u_pbrFactors;
    float u_emissiveStrength;
};

layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D pbrParamMap;
layout(set = 1, binding = 4) uniform sampler2D emissionMap;

layout(location = 0) in vec3 v2fPosition;
layout(location = 1) in vec3 v2fNormal;
layout(location = 2) in vec3 v2fColor;
layout(location = 3) in vec2 v2fTexCoord;
layout(location = 4) in vec4 v2fTangent;

layout(location = 0) out vec4 outColor;

void main()
{
    // 第一版 PBR 先复用一张 albedo 贴图，baseColor = 贴图颜色 * 顶点阶段传来的 tint。
    #if defined(USE_ALBEDO_MAP)
        vec4 albedoColor = texture(albedoMap, v2fTexCoord);
    #else
        vec4 albedoColor = u_tintColor;
    #endif
    vec4 baseColor = albedoColor * vec4(v2fColor, 1.0f);
    
    #if defined(RENDER_MODE_OPAQUE_CLIP)
    // pbrFactors.w 预留作为 alphaClipThreshold 使用
    if (baseColor.a < u_pbrFactors.w) {
        discard;
    }
    #endif

    // pbrFactors: x=roughness, y=metallic, z=ao, w=预留。
    // 粗糙度、金属度、AO 确保在上游 Material Instance 配置正确，此处直接使用。
    #if defined(USE_PBR_MAP)
        vec4 pbrParam = texture(pbrParamMap, v2fTexCoord);
        float roughness = pbrParam.x;
        float metallic = pbrParam.y;
        float ao = pbrParam.z;
    #else
        float roughness = u_pbrFactors.x;
        float metallic = u_pbrFactors.y;
        float ao = u_pbrFactors.z;
    #endif

    // 自发光强度由材质参数控制。
    vec3 emissionColor = vec3(0.0f);
    #if defined(USE_EMISSION_MAP)
        emissionColor = texture(emissionMap, v2fTexCoord).rgb * u_emissiveStrength;
    #endif

    vec3 normal = normalize(v2fNormal);
    #if defined(USE_NORMAL_MAP)
        vec3 normalTS = texture(normalMap, v2fTexCoord).xyz * 2.0 - 1.0;
        vec3 bitangent = cross(v2fNormal, v2fTangent.xyz) * v2fTangent.w;
        // 按 MikkTSpace 参考实现建议，直接用未归一化插值后的 T/B/N 做解码，
        // 这里只保存 tangent.xyz + sign，副切线按 cross(N, T) * sign 在片元重建。
        normal = normalize(
            normalTS.x * v2fTangent.xyz +
            normalTS.y * bitangent +
            normalTS.z * v2fNormal);
    #endif
    vec3 viewDir = normalize(uboVP.cameraPosition - v2fPosition);

    // Direct Lighting 只包含显式光源，阴影也只作用在这部分。
    vec3 directLighting = CalculateDirectLighting(normal, v2fPosition, uboVP.cameraPosition, baseColor.rgb, roughness, metallic);
    float shadow = CalculateShadow(uboVP.lightViewProj, v2fPosition, 0.002f);
    directLighting *= shadow;

    // Indirect Lighting 分成 SH 漫反射与 prefiltered environment specular 两部分。
    vec3 indirectDiffuse = CalculateDiffuseIbl(normal, baseColor.rgb, metallic);
    vec3 indirectSpecular = CalculateSpecularIbl(normal, viewDir, baseColor.rgb, roughness, metallic);
    vec3 indirectLighting = indirectDiffuse + indirectSpecular;

    // 最终颜色显式按 Direct + Indirect 组合，避免环境光和直接光职责混在一起。
    vec3 finalColor = emissionColor + directLighting + indirectLighting * ao;

#if defined(ENABLE_DEBUG_VIEW)
    if (uboVP.debugViewMode == 1) {
        outColor = vec4(baseColor.rgb, 1.0);
    } else if (uboVP.debugViewMode == 2) {
        outColor = vec4(emissionColor, 1.0);
    } else if (uboVP.debugViewMode == 3) {
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
    } else if (uboVP.debugViewMode == 4) {
        outColor = vec4(vec3(roughness), 1.0);
    } else if (uboVP.debugViewMode == 5) {
        outColor = vec4(vec3(metallic), 1.0);
    } else if (uboVP.debugViewMode == 6) {
        outColor = vec4(vec3(ao), 1.0);
    } else if (uboVP.debugViewMode == 7) {
        outColor = vec4(vec3(shadow), 1.0);
    } else if (uboVP.debugViewMode == 8) {
        outColor = vec4(directLighting, 1.0);
    } else if (uboVP.debugViewMode == 9) {
        outColor = vec4(indirectDiffuse, 1.0);
    } else if (uboVP.debugViewMode == 10) {
        outColor = vec4(indirectSpecular, 1.0);
    } else {
        outColor = vec4(finalColor, baseColor.a);
    }
#else
    outColor = vec4(finalColor, baseColor.a);
#endif
}
