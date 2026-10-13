#version 450

#include "../common/commonUbo.glsl"

// 编辑器参考网格使用固定 Mesh 顶点流：位置是世界空间的地面/坐标轴条带，
// 颜色在顶点里编码次线、主线与 X/Y/Z 轴，片元不再分派材质分支。
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 outGridColor;

void main()
{
    outGridColor = inColor;
    gl_Position = uboVP.viewProjection * vec4(inPosition, 1.0);
}
