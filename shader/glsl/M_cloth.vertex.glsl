#ifndef VL_M_CLOTH_VERTEX_GLSL
#define VL_M_CLOTH_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
