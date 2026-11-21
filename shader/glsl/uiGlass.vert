#version 450

// 全屏三角形：不需要顶点缓冲，矩形与半径全部由 push constant 传入。
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
