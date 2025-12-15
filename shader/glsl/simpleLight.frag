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
    vec4 albedo = texture(albedoMap, v2fTexCoord);
    float roughness = 0.1;
    float metallic = 0.0;
    vec3 normal = normalize(v2fNormal);
    vec3 V = normalize(uboVP.cameraPosition - v2fPosition);
    vec3 lighting = vec3(0.0);
    int offset = uboLight.directionalLightOffset;
    int dirCount = uboLight.directionalLightCount;
    int end = offset + dirCount;
    for(int i = offset; i < end; i++)
    {
        lighting += CalculateDirectionalLight(
                                normal, 
                                v2fPosition, 
                                uboVP.cameraPosition, 
                                albedo.rgb, 
                                roughness, 
                                metallic, 
                                uboLight.lights[i]);
    }
    vec3 environment = uboVP.ambient * albedo.rgb;
    outColor = vec4(lighting + environment, albedo.a);
}
