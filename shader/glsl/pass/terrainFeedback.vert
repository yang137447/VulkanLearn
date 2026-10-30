#version 450

#include "../common/commonUbo.glsl"

layout(location = 0) in vec3 inPosition;
// 地形顶点在 texCoord1 中携带全局 page 坐标，范围 [0,1]。
layout(location = 7) in vec2 inTexCoord1;

layout(location = 0) out vec2 outPageUv;

void main()
{
    outPageUv = inTexCoord1;
    gl_Position = uboVP.viewProjection * uboM.model * vec4(inPosition, 1.0);
}
