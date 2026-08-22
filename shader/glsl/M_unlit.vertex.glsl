#ifndef VL_M_UNLIT_VERTEX_GLSL
#define VL_M_UNLIT_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_unlit 的公开 Vertex 入口；投影和 Varyings 由统一 MeshPass Template 负责。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif