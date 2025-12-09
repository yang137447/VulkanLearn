#include "defines.glsl"

struct Light{
    vec4 colorIntensity;
    vec4 positionRadius;
    vec4 directionPad;
    vec4 coneAngleOuterInnerPadPad;
};
// 这里用统一参数 + 分段储存的方式
layout(std430, binding = 1) readonly buffer UBOLight{
    int directionalLightOffset;
    int directionalLightCount;
    int pointLightOffset;
    int pointLightCount;
    int spotLightOffset;
    int spotLightCount;

    Light lights[];
} uboLight;

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness*roughness;
    float a2     = a*a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
	
    float num   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
	
    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float num   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
	
    return num / denom;
}

// 几何遮蔽函数：Smith 联合遮蔽模型（GGX 版本）
// 同时考虑视线方向 V 与光源方向 L 的遮蔽，避免微面元被自身几何遮挡
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    // 计算法线与视线/光源方向的点积，并截断到非负值
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);

    // 分别计算视线方向与光源方向的 Schlick-GGX 遮蔽因子
    float ggx2 = GeometrySchlickGGX(NdotV, roughness); // 视线遮蔽
    float ggx1 = GeometrySchlickGGX(NdotL, roughness); // 光源遮蔽

    // 联合遮蔽 = 视线遮蔽 × 光源遮蔽
    // 值域 [0,1]，越粗糙的表面遮蔽越明显
    return ggx1 * ggx2;
}
vec3 CalculatePointLight(
    in vec3 normal_WS,
    in vec3 vetexPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in Light pointLight)
{
    // 根据金属度计算 F0：金属用 baseColor，非金属用 0.04
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);

    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = pointLight.colorIntensity.xyz;
    float lightIntensity = pointLight.colorIntensity.w;
    vec3 lightPosition = pointLight.positionRadius.xyz;
    float lightRadius = pointLight.positionRadius.w;

    // 中间量计算
    vec3 N = normalize(normal_WS);
    vec3 L = normalize(lightPosition - vetexPos_WS);
    vec3 V = normalize(cameraPos_WS - vetexPos_WS);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // 距离衰减：平方反比，并额外用 lightRadius 做平滑截断
    float distance = length(lightPosition - vetexPos_WS);
    float denom = distance * distance + 1e-4;
    float attenuation = 1.0 / denom;
    // 可选：smoothstep 边缘软化
    // attenuation *= smoothstep(lightRadius, lightRadius * 0.8, distance);
    vec3 radiance = lightColor * attenuation * lightIntensity;

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // BRDF 分量
    vec3 diffuseBRDF  = kD * baseColor / PI;
    // 注意：分母中的 4*NdotL*NdotV 已在 GeometrySmith 中体现，这里直接乘上 NdotL
    vec3 specularBRDF = kS * D * G / max(4.0 * NdotL * NdotV, 1e-4);

    // 最终颜色
    vec3 color = (diffuseBRDF + specularBRDF) * radiance * NdotL;
    return color;
}