#ifndef VL_M_TEXTURE_ARRAY_SMOKE_VERTEX_GLSL
#define VL_M_TEXTURE_ARRAY_SMOKE_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"

// 纹理数组冒烟材质复用默认顶点路径，只验证 fragment 的 sampler2DArray 采样。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return EvaluateMFDefaultVertex(vertexInput);
}

#endif
