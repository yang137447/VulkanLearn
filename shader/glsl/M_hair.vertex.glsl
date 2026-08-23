#ifndef VL_M_HAIR_VERTEX_GLSL
#define VL_M_HAIR_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// Hair Card 第一版不在材质层修改位置；rootward tangent 由规范化顶点输入提供。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
