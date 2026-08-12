#version 450

#include "../common/commonUbo.glsl"
#include "../common/proceduralSky.glsl"

layout(set = 0, binding = 1) uniform samplerCube environmentCube;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

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
    // 背景天空直接读取每帧最新状态；低频 IBL 派生资源按独立预算渐进更新，
    // 因而视觉天空可以连续变化而不必同步执行全量重建。
    vec3 skyColor = uboVP.environmentType == 1
        ? EvaluateProceduralSky(worldDir)
        : textureLod(environmentCube, worldDir, 0.0).rgb;
    skyColor *= uboVP.environmentIntensity;
    outColor = vec4(skyColor, 1.0f);
}
