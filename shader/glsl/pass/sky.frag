#version 450

#include "../common/commonUbo.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform samplerCube environmentCube;

vec3 GetViewRayWS(vec2 uv)
{
    vec2 ndc = uv * 2.0f - 1.0f;
    vec4 clipPos = vec4(ndc, 1.0f, 1.0f);
    vec4 viewPos = uboVP.invProjection * clipPos;
    vec3 viewDir = normalize(viewPos.xyz / viewPos.w);
    return normalize((uboVP.invView * vec4(viewDir, 0.0f)).xyz);
}

void main()
{
    vec3 worldDir = GetViewRayWS(inUV);
    vec3 skyColor = texture(environmentCube, worldDir).rgb;
    outColor = vec4(skyColor, 1.0f);
}
