#ifndef VL_M_THIN_TRANSLUCENT_VERTEX_GLSL
#define VL_M_THIN_TRANSLUCENT_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_thinTranslucent 的公开 Vertex 入口；薄透射不修改网格位置。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
