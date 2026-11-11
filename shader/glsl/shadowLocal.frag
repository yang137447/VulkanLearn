#version 450

#include "common/localShadowDepth.glsl"

layout(location = 8) in vec3 vLocalShadowWorldPos;

void main()
{
    WriteLocalShadowDepth(vLocalShadowWorldPos);
}
