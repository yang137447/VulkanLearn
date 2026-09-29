#version 450

layout(location = 0) out vec2 uv;

void main()
{
    // 全屏三角形无顶点缓冲；film 的首行与 Vulkan 正高度 viewport 的顶边一致。
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
