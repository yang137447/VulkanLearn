#version 450

#include "common/commonUbo.glsl"
#include "common/lighting.glsl"
#include "materialFunction/mf_measureGrid.glsl"

layout(location = 0) in vec3 v2fPosition;
layout(location = 1) in vec3 v2fNormal;
layout(location = 2) in vec3 v2fColor;
layout(location = 3) in vec2 v2fTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec4 baseColor = vec4(0.5, 0.5, 0.5, 1.0);
    const vec4 gridColor = vec4(0.0, 0.0, 0.0, 1.0);
    const vec4 subGridColor = vec4(0.2, 0.2, 0.2, 1.0);

    float subGridMask = CalculateMeasureGridMask(v2fPosition, v2fNormal, 0.2);
    vec4 albedo = mix(baseColor, subGridColor, subGridMask);

    float gridMask = CalculateMeasureGridMask(v2fPosition, v2fNormal, 1.0);
    albedo = mix(albedo, gridColor, gridMask);

    float roughness = mix(1.0, 0.5, max(gridMask, subGridMask));
    float metallic = 0.0;
    vec3 normal = normalize(v2fNormal);
    vec3 V = normalize(uboVP.cameraPosition - v2fPosition);

    vec3 finalColor = albedo.rgb;
    // 计算灯光光照
    finalColor = CalculateLighting(normal, v2fPosition, uboVP.cameraPosition, albedo.rgb, roughness, metallic);
    float shadow = CalculateShadow(uboVP.lightViewProj, v2fPosition, 0.002f);
    finalColor *= shadow;
    // 环境光
    finalColor += uboVP.ambient * albedo.rgb;
    outColor = vec4(finalColor, albedo.a);
}
