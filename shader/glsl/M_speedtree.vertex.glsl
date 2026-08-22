#ifndef VL_M_SPEEDTREE_VERTEX_GLSL
#define VL_M_SPEEDTREE_VERTEX_GLSL

#include "materialFunction/mf_speedtreeVertex.glsl"

// M_speedtree 的公开 Vertex 入口；SpeedTree 风摆在内部顶点 MF 中完成。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFSpeedTreeVertex(vertexInput);
}

#endif
