#version 450

#include "generate/M_sssVerticalParamter.glsl"
#include "../engine/subsurfaceProfileFilter.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D sssPing;
layout(set = 3, binding = 1) uniform sampler2D gbufferB;
layout(set = 3, binding = 2) uniform sampler2D gbufferD;
layout(set = 3, binding = 3) uniform sampler2D sceneDepth;

void main()
{
    // 垂直 pass 消费水平结果，保持与水平 pass 完全相同的 profile 合同。
    outColor = FilterSubsurfaceProfile(
        inUV,
        vec2(0.0, 1.0),
        sssPing,
        gbufferB,
        gbufferD,
        sceneDepth,
        subsurfaceProfileTable,
        u_sssFilterParameters);
}
