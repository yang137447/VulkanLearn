#ifndef VL_M_WATER_VERTEX_GLSL
#define VL_M_WATER_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// 水面网格是 CPU 生成的平面，顶点阶段不需要位移；波纹只在片元法线上做。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
