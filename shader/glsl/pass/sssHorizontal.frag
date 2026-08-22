#version 450

#include "generate/M_sssHorizontalParamter.glsl"
#include "../engine/subsurfaceProfileFilter.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D diffuseLighting;
layout(set = 3, binding = 1) uniform sampler2D gbufferB;
layout(set = 3, binding = 2) uniform sampler2D gbufferD;
layout(set = 3, binding = 3) uniform sampler2D sceneDepth;

void main()
{
    // 水平 pass 只改变采样方向，kernel、rejection 和归一化由共享函数统一实现。
    outColor = FilterSubsurfaceProfile(
        inUV,
        vec2(1.0, 0.0),
        diffuseLighting,
        gbufferB,
        gbufferD,
        sceneDepth,
        subsurfaceProfileTable,
        u_sssFilterParameters);
}
