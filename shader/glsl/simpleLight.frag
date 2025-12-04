#version 450

layout(binding = 4) uniform sampler2D albedoMap;

layout(location = 0) in vec3 v2fColor;
layout(location = 1) in vec2 v2fTexCoord;
layout(location = 2) in vec3 v2fLightColor;

layout(location = 0) out vec4 outColor;

void main()
{
    //outColor = texture(albedoMap, fragTexCoord);
    lowp vec4 baseColor = texture(albedoMap, v2fTexCoord);
    baseColor = baseColor * vec4(v2fColor, 1.0);
    baseColor = baseColor * vec4(v2fLightColor, 1.0);
    outColor = baseColor;
}