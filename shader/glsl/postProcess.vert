#version 450

layout(location = 0) out vec2 outUV;

void main() 
{
    // 使用 gl_VertexIndex 生成覆盖全屏的三角形坐标
    // 0: (-1, -1), 1: (-1, 3), 2: (3, -1)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0f - 1.0f, 0.0f, 1.0f);
}