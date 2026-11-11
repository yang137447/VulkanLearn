#version 450

#include "../common/commonUbo.glsl"

layout(location = 0) in vec3 inPosition;
// 地形顶点在 texCoord1 中携带全局 page 坐标，范围 [0,1]。
layout(location = 7) in vec2 inTexCoord1;
// location 3 是地形的平铺 UV；L4b 的层页 feedback 需要它与 M_terrain
// 完全相同的层 UV 推导。
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec2 outPageUv;
layout(location = 1) out vec2 outMaterialUv;

void main()
{
    outPageUv = inTexCoord1;
    outMaterialUv = inTexCoord;
    gl_Position = uboVP.viewProjection * uboM.model * vec4(inPosition, 1.0);
}
