#version 450

layout(binding = 3) uniform sampler2D albedoMap;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    //outColor = texture(albedoMap, fragTexCoord);
    lowp vec4 baseColor = texture(albedoMap, fragTexCoord);
    outColor = vec4(fragColor, 1.0) * 0.9 + 0.1*texture(albedoMap, fragTexCoord);
}