#ifndef VL_M_BRDF_PLOT_VERTEX_GLSL
#define VL_M_BRDF_PLOT_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// M_brdfPlot 的公开 Vertex 入口；投影和 Varyings 由统一 MeshPass Template 负责。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
