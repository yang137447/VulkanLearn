#version 450

#include "common/commonUbo.glsl"
#include "common/lighting.glsl"

layout(binding = 4) uniform sampler2D albedoMap;

layout(location = 0) in vec3 v2fPosition;
layout(location = 1) in vec3 v2fNormal;
layout(location = 2) in vec3 v2fColor;
layout(location = 3) in vec2 v2fTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    //outColor = texture(albedoMap, fragTexCoord);
    vec4 baseColor = texture(albedoMap, v2fTexCoord);
    float roughness = 0.5;
    float metallic = 0.5;
    vec3 normal = normalize(v2fNormal);
    vec3 V = normalize(uboVP.cameraPosition - v2fPosition);
    int offset = uboLight.pointLightOffset;
    for(int i = offset; i < offset + uboLight.pointLightCount; i++)
    {
        baseColor.xyz = CalculatePointLight(normal, v2fPosition, uboVP.cameraPosition, baseColor.xyz, roughness, metallic, uboLight.lights[i]);
    }
    outColor = baseColor;
}