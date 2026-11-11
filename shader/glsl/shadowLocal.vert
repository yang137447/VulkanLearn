#version 450

#include "common/commonUbo.glsl"

layout(location = 0) in vec3 inPosition;
// 与材质 ShadowDepth 模板保持同一世界位置插值槽位。
layout(location = 8) out vec3 vLocalShadowWorldPos;

void main()
{
    vec4 worldPosition = uboM.model * vec4(inPosition, 1.0);
    vLocalShadowWorldPos = worldPosition.xyz;
    gl_Position = uboVP.projection * uboVP.view * worldPosition;
}
