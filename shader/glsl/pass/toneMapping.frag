#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D sceneColor;
layout(set = 3, binding = 1) uniform sampler2D bloomColor;

void main()
{
    vec3 scene = texture(sceneColor, inUV).rgb;
    vec3 bloom = texture(bloomColor, inUV).rgb;

    float bloomStrength = 1.0;
    vec3 hdr = scene + bloom * bloomStrength;

    vec3 mapped = hdr / (hdr + vec3(1.0));
    outColor = vec4(mapped, 1.0);
}
