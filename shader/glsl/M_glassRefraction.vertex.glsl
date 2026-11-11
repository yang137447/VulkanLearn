#ifndef VL_M_GLASS_REFRACTION_VERTEX_GLSL
#define VL_M_GLASS_REFRACTION_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
