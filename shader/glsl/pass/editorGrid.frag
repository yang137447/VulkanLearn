#version 450

// 参考网格不是光照表面：它只表达编辑器参考系，因此不接入 GBuffer、阴影或 IBL，
// 颜色直接沿用顶点值。遮挡由 pass 的深度测试完成，见 renderGraphConfig 的 editorGrid。
layout(location = 0) in vec4 inGridColor;
layout(location = 0) out vec4 outSceneColor;

void main()
{
    outSceneColor = vec4(inGridColor.rgb, 1.0);
}
