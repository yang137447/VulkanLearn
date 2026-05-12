#version 450

#include "common/commonUbo.glsl"
#include "common/lighting.glsl"

layout(set = 1, binding = 0) uniform UBOMIParamters{
    vec4 tintColor;
    vec4 pbrFactors;
} uboMIP;

layout(set = 1, binding = 1) uniform sampler2D albedoMap;

layout(location = 0) in vec3 v2fPosition;
layout(location = 1) in vec3 v2fNormal;
layout(location = 2) in vec3 v2fColor;
layout(location = 3) in vec2 v2fTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    // 第一版 PBR 先复用一张 albedo 贴图，baseColor = 贴图颜色 * 顶点阶段传来的 tint。
    vec4 albedoSample = texture(albedoMap, v2fTexCoord);
    vec3 baseColor = albedoSample.rgb * v2fColor;

    // pbrFactors: x=roughness, y=metallic, z=ao, w=预留。
    // 粗糙度、金属度、AO 确保在上游 Material Instance 配置正确，此处直接使用。
    float roughness = uboMIP.pbrFactors.x;
    float metallic = uboMIP.pbrFactors.y;
    float ao = uboMIP.pbrFactors.z;

    vec3 normal = normalize(v2fNormal);
    vec3 viewDir = normalize(uboVP.cameraPosition - v2fPosition);

    // Direct Lighting 只包含显式光源，阴影也只作用在这部分。
    vec3 directLighting = CalculateDirectLighting(normal, v2fPosition, uboVP.cameraPosition, baseColor, roughness, metallic);
    float shadow = CalculateShadow(uboVP.lightViewProj, v2fPosition, 0.002f);
    directLighting *= shadow;

    // Indirect Lighting 分成 SH 漫反射与 prefiltered environment specular 两部分。
    vec3 indirectDiffuse = CalculateDiffuseIbl(normal, baseColor, metallic) * ao;
    vec3 indirectSpecular = CalculateSpecularIbl(normal, viewDir, baseColor, roughness, metallic) * ao;
    vec3 indirectLighting = indirectDiffuse + indirectSpecular;

    // 最终颜色显式按 Direct + Indirect 组合，避免环境光和直接光职责混在一起。
    vec3 finalColor = directLighting + indirectLighting;
    outColor = vec4(finalColor, albedoSample.a);
}
