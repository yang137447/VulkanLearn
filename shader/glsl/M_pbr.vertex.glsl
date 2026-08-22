#ifndef VL_M_PBR_VERTEX_GLSL
#define VL_M_PBR_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_pbr 的公开 Vertex 入口。
// 这里只组合材质顶点函数；gl_Position、Varyings 和 Pass 行为由 Engine Template 负责。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
