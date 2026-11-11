#ifndef VL_M_TERRAIN_VERTEX_GLSL
#define VL_M_TERRAIN_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// 地形网格已经在 CPU 生成，顶点阶段只走默认投影/插值路径。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
