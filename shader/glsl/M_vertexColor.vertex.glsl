#ifndef VL_M_VERTEX_COLOR_VERTEX_GLSL
#define VL_M_VERTEX_COLOR_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_vertexColor 的公开 Vertex 入口；颜色由 Surface 入口消费。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif