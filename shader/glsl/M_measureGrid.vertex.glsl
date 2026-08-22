#ifndef VL_M_MEASURE_GRID_VERTEX_GLSL
#define VL_M_MEASURE_GRID_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_measureGrid 的公开 Vertex 入口；网格世界位置由统一模板传入 Surface Context。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif