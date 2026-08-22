#ifndef VL_M_CAR_PAINT_VERTEX_GLSL
#define VL_M_CAR_PAINT_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_carPaint 的公开 Vertex 入口；车漆不修改网格位置。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
