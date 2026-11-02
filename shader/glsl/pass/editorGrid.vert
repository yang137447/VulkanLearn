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
    // 场景地面可能不在 y=0；参考网格跟随场景高度，才能在枪体/地面之后被深度测试。
    vec3 worldPosition = inPosition + vec3(0.0, uboVP.editorGridHeight, 0.0);
    gl_Position = uboVP.viewProjection * vec4(worldPosition, 1.0);
}
