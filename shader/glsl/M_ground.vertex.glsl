#ifndef VL_M_GROUND_VERTEX_GLSL
#define VL_M_GROUND_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_ground 的公开 Vertex 入口：与 M_pbr 相同的默认顶点函数，
// 平铺只影响 surface 的 UV，不改变顶点变换。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
