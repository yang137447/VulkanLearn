#version 450

#include "common/commonUbo.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

void main()
{
    gl_Position = uboVP.projection * uboVP.view * uboM.model * vec4(inPosition, 1.0);
}